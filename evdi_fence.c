// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Red Hat
 * Copyright (c) 2015 - 2020 DisplayLink (UK) Ltd.
 * Copyright (c) 2026 Lindroid Authors
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#include "evdi_drv.h"
#include "uapi/evdi_drm.h"
#include <linux/sync_file.h>
#include <linux/file.h>
#include <linux/slab.h>

static struct dma_fence *evdi_fence_from_fd(int fd)
{
	struct dma_fence *fence;

	if (fd < 0)
		return NULL;

	fence = sync_file_get_fence(fd);
	if (IS_ERR(fence))
		return NULL;

	return fence;
}

static int evdi_fence_to_fd(struct dma_fence *fence)
{
	struct sync_file *sync_file;
	int fd;

	if (!fence)
		return -1;

	sync_file = sync_file_create(fence);
	if (!sync_file)
		return -ENOMEM;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fput(sync_file->file);
		return fd;
	}

	fd_install(fd, sync_file->file);
	return fd;
}

int evdi_pending_acquire_fence_set_fd(struct evdi_device *evdi, u32 display_id,
				      int acquire_fence_fd)
{
	struct dma_fence *fence = NULL;
	struct dma_fence *old;

	if (!evdi)
		return -EINVAL;
	if (display_id >= LINDROID_MAX_CONNECTORS)
		return -EINVAL;

	if (acquire_fence_fd >= 0) {
		fence = evdi_fence_from_fd(acquire_fence_fd);
		if (!fence)
			return -EINVAL;
	}

	mutex_lock(&evdi->fence_mutex);
	old = evdi->pending_acquire_fence[display_id];
	evdi->pending_acquire_fence[display_id] = fence;
	mutex_unlock(&evdi->fence_mutex);

	if (old)
		dma_fence_put(old);

	return 0;
}

void evdi_swap_acquire_fence_snapshot(struct evdi_device *evdi, u32 display_id)
{
	struct dma_fence *old_swap = NULL;
	struct dma_fence *pending = NULL;

	if (!evdi || display_id >= LINDROID_MAX_CONNECTORS)
		return;

	mutex_lock(&evdi->fence_mutex);
	pending = evdi->pending_acquire_fence[display_id];
	/* Only replace the swap fence for a new pending fence */
	if (pending) {
		old_swap = evdi->swap_acquire_fence[display_id];
		evdi->swap_acquire_fence[display_id] = pending;
		evdi->pending_acquire_fence[display_id] = NULL;
	}
	mutex_unlock(&evdi->fence_mutex);

	if (old_swap)
		dma_fence_put(old_swap);
}

int evdi_swap_acquire_fence_get_fd(struct evdi_device *evdi, u32 display_id)
{
	struct dma_fence *fence;
	int fd;

	if (!evdi || display_id >= LINDROID_MAX_CONNECTORS)
		return -1;

	mutex_lock(&evdi->fence_mutex);
	fence = evdi->swap_acquire_fence[display_id];
	if (fence)
		dma_fence_get(fence);
	mutex_unlock(&evdi->fence_mutex);

	if (!fence)
		return -1;

	fd = evdi_fence_to_fd(fence);
	dma_fence_put(fence);

	return fd;
}

void evdi_swap_release_fence_set_fd(struct evdi_device *evdi, u32 display_id,
				    int release_fence_fd)
{
	struct dma_fence *fence = NULL;
	struct dma_fence *old;

	if (!evdi || display_id >= LINDROID_MAX_CONNECTORS)
		return;

	if (release_fence_fd >= 0) {
		fence = evdi_fence_from_fd(release_fence_fd);
		if (!fence) {
			evdi_warn("Failed to import release fence fd=%d for display=%u\n",
				  release_fence_fd, display_id);
			return;
		}
	}

	mutex_lock(&evdi->fence_mutex);
	old = evdi->swap_release_fence[display_id];
	evdi->swap_release_fence[display_id] = fence;
	mutex_unlock(&evdi->fence_mutex);

	if (old)
		dma_fence_put(old);
}

static struct dma_fence *evdi_swap_release_fence_take(struct evdi_device *evdi,
						       u32 display_id)
{
	struct dma_fence *fence;

	if (!evdi || display_id >= LINDROID_MAX_CONNECTORS)
		return NULL;

	mutex_lock(&evdi->fence_mutex);
	fence = evdi->swap_release_fence[display_id];
	evdi->swap_release_fence[display_id] = NULL;
	mutex_unlock(&evdi->fence_mutex);

	return fence;
}

static void evdi_fence_clear_all_locked(struct evdi_device *evdi)
{
	int d;

	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++) {
		if (evdi->pending_acquire_fence[d]) {
			dma_fence_put(evdi->pending_acquire_fence[d]);
			evdi->pending_acquire_fence[d] = NULL;
		}
		if (evdi->swap_acquire_fence[d]) {
			dma_fence_put(evdi->swap_acquire_fence[d]);
			evdi->swap_acquire_fence[d] = NULL;
		}
		if (evdi->swap_release_fence[d]) {
			dma_fence_put(evdi->swap_release_fence[d]);
			evdi->swap_release_fence[d] = NULL;
		}
	}
}

void evdi_fence_cleanup(struct evdi_device *evdi)
{
	if (!evdi)
		return;

	mutex_lock(&evdi->fence_mutex);
	evdi_fence_clear_all_locked(evdi);
	mutex_unlock(&evdi->fence_mutex);
}

/* Called from modeset when swap_pending is cleared */
void evdi_swap_release_fence_wait_and_clear(struct evdi_device *evdi, u32 display_id)
{
	struct dma_fence *fence;
	long ret;

	fence = evdi_swap_release_fence_take(evdi, display_id);
	if (!fence)
		return;

	ret = dma_fence_wait_timeout(fence, true, msecs_to_jiffies(250));
	if (ret <= 0)
		evdi_warn("Release fence wait failed display=%u ret=%ld\n",
			  display_id, ret);

	dma_fence_put(fence);
}

void evdi_fence_init(struct evdi_device *evdi)
{
	int i;

	if (!evdi)
		return;

	mutex_init(&evdi->fence_mutex);
	for (i = 0; i < LINDROID_MAX_CONNECTORS; i++) {
		evdi->pending_acquire_fence[i] = NULL;
		evdi->swap_acquire_fence[i] = NULL;
		evdi->swap_release_fence[i] = NULL;
	}
}
