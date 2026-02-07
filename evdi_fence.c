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

#include <linux/dma-fence.h>
#include <linux/sync_file.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/err.h>

static __always_inline struct dma_fence *evdi_fence_from_syncfd(int fd, int *out_err)
{
	struct dma_fence *f;

	if (out_err)
		*out_err = 0;

	f = NULL;

	if (fd < 0)
		return NULL;

	f = sync_file_get_fence(fd);
	if (IS_ERR(f)) {
		if (out_err)
			*out_err = PTR_ERR(f);
		return NULL;
	}
	return f;
}

/*
 * Create a sync_file from fence and reserve an fd for it.
 *
 * Returns:
 *  >=0 : reserved fd, caller must fd_install(fd, *out_file)
 *  -1  : no fence
 *  <0  : error
 */
int evdi_syncfd_reserve_from_fence(struct dma_fence *f, struct file **out_file)
{
	struct sync_file *sf;
	int fd;

	sf = NULL;
	fd = -1;

	if (!out_file)
		return -EINVAL;
	*out_file = NULL;

	if (!f)
		return -1;

	might_sleep();

	sf = sync_file_create(f);
	if (!sf)
		return -ENOMEM;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fput(sf->file);
		return fd;
	}

	*out_file = sf->file;
	return fd;
}

void evdi_fence_tables_init(struct evdi_device *evdi)
{
	int d;

	if (!evdi)
		return;

	mutex_lock(&evdi->fence_mutex);
	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++) {
		evdi->pending_acquire_id[d] = 0;
		evdi->pending_acquire_owner[d] = NULL;
		evdi->pending_acquire_fence[d] = NULL;
	}
	mutex_unlock(&evdi->fence_mutex);
}

static void evdi_fence_tables_clear_locked(struct evdi_device *evdi,
					  struct dma_fence **old_pending,
					  struct dma_fence **old_release)
{
	int d;

	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++) {
		old_pending[d] = evdi->pending_acquire_fence[d];
		evdi->pending_acquire_fence[d] = NULL;
		evdi->pending_acquire_id[d] = 0;
		evdi->pending_acquire_owner[d] = NULL;

		old_release[d] = evdi->swap_release_fence[d];
		evdi->swap_release_fence[d] = NULL;
		atomic_set(&evdi->swap_release_ready[d], 0);
	}
}

void evdi_fence_tables_reset(struct evdi_device *evdi)
{
	struct dma_fence *old_pending[LINDROID_MAX_CONNECTORS] = { 0 };
	struct dma_fence *old_release[LINDROID_MAX_CONNECTORS] = { 0 };
	int d;

	if (!evdi)
		return;

	mutex_lock(&evdi->fence_mutex);
	evdi_fence_tables_clear_locked(evdi, old_pending, old_release);
	mutex_unlock(&evdi->fence_mutex);

	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++) {
		if (old_pending[d])
			dma_fence_put(old_pending[d]);
		if (old_release[d])
			dma_fence_put(old_release[d]);
	}
}

void evdi_fence_tables_cleanup(struct evdi_device *evdi)
{
	evdi_fence_tables_reset(evdi);
}

int evdi_pending_acquire_fence_set_fd(struct evdi_device *evdi, u32 display_id,
				      struct drm_file *owner,
				      u32 id, int acquire_fence_fd)
{
	struct dma_fence *f = NULL;
	struct dma_fence *old = NULL;
	int err = 0;

	if (!evdi)
		return -EINVAL;
	if (display_id >= LINDROID_MAX_CONNECTORS)
		return -EINVAL;

	/* Clear */
	if (acquire_fence_fd < 0)
		goto store;

	f = evdi_fence_from_syncfd(acquire_fence_fd, &err);
	if (!f) {
		/* Clear the old fence to avoid stale usage after failed update. */
		mutex_lock(&evdi->fence_mutex);
		old = evdi->pending_acquire_fence[display_id];
		evdi->pending_acquire_fence[display_id] = NULL;
		evdi->pending_acquire_id[display_id] = 0;
		evdi->pending_acquire_owner[display_id] = NULL;
		mutex_unlock(&evdi->fence_mutex);
		if (old)
			dma_fence_put(old);
		evdi_err("pending acquire fence import failed display=%u id=%u fd=%d err=%d\n",
			 display_id, id, acquire_fence_fd, err);
		return (err ? err : -EINVAL);
	}

store:
	mutex_lock(&evdi->fence_mutex);
	old = evdi->pending_acquire_fence[display_id];
	evdi->pending_acquire_fence[display_id] = f;
	evdi->pending_acquire_id[display_id] = (f ? id : 0);
	evdi->pending_acquire_owner[display_id] = (f ? owner : NULL);
	mutex_unlock(&evdi->fence_mutex);

	if (old)
		dma_fence_put(old);
	return 0;
}

int evdi_pending_acquire_fence_peek_get(struct evdi_device *evdi,
				       u32 display_id,
				       struct drm_file *requestor,
				       struct dma_fence **out_fence)
{
	struct dma_fence *f = NULL;
	struct drm_file *owner = NULL;

	if (!out_fence)
		return -EINVAL;
	*out_fence = NULL;

	if (!evdi)
		return -EINVAL;
	if (display_id >= LINDROID_MAX_CONNECTORS)
		return -EINVAL;

	mutex_lock(&evdi->fence_mutex);
	owner = READ_ONCE(evdi->drm_client);
	if (owner && requestor && owner != requestor) {
		mutex_unlock(&evdi->fence_mutex);
		return -1;
	}

	f = evdi->pending_acquire_fence[display_id];
	if (f)
		dma_fence_get(f);
	mutex_unlock(&evdi->fence_mutex);

	if (!f)
		return -1;

	*out_fence = f;
	return 0;
}

void evdi_pending_acquire_fence_consume_if(struct evdi_device *evdi,
					  u32 display_id,
					  struct drm_file *requestor,
					  struct dma_fence *fence)
{
	struct dma_fence *old = NULL;
	struct drm_file *owner = NULL;

	if (!evdi || !fence)
		return;
	if (display_id >= LINDROID_MAX_CONNECTORS)
		return;

	mutex_lock(&evdi->fence_mutex);
	owner = READ_ONCE(evdi->drm_client);

	if (evdi->pending_acquire_fence[display_id] == fence &&
	    (!owner || !requestor || owner == requestor)) {
		old = evdi->pending_acquire_fence[display_id];
		evdi->pending_acquire_fence[display_id] = NULL;
		evdi->pending_acquire_id[display_id] = 0;
		evdi->pending_acquire_owner[display_id] = NULL;
	}
	mutex_unlock(&evdi->fence_mutex);

	if (old)
		dma_fence_put(old);
}

void evdi_pending_acquire_fence_clear(struct evdi_device *evdi, u32 display_id)
{
	(void)evdi_pending_acquire_fence_set_fd(evdi, display_id, NULL, 0, -1);
}

void evdi_pending_acquire_fence_drop_owner(struct evdi_device *evdi,
					   struct drm_file *owner)
{
	struct dma_fence *old[LINDROID_MAX_CONNECTORS] = { 0 };
	int d;

	if (!evdi || !owner)
		return;

	mutex_lock(&evdi->fence_mutex);
	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++) {
		if (evdi->pending_acquire_owner[d] != owner)
			continue;
		old[d] = evdi->pending_acquire_fence[d];
		evdi->pending_acquire_fence[d] = NULL;
		evdi->pending_acquire_id[d] = 0;
		evdi->pending_acquire_owner[d] = NULL;
	}
	mutex_unlock(&evdi->fence_mutex);

	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++) {
		if (old[d])
			dma_fence_put(old[d]);
	}
}

void evdi_swap_release_fence_set_fd(struct evdi_device *evdi, u32 display_id,
				    int release_fence_fd)
{
	struct dma_fence *f = NULL;
	struct dma_fence *old = NULL;
	int err = 0;

	if (!evdi)
		return;
	if (display_id >= LINDROID_MAX_CONNECTORS)
		return;

	if (release_fence_fd >= 0) {
		f = evdi_fence_from_syncfd(release_fence_fd, &err);
		if (!f) {
			evdi_err("swap release fence import failed display=%u fd=%d err=%d\n",
					  display_id, release_fence_fd, err);
			f = NULL;
		}
	}

	mutex_lock(&evdi->fence_mutex);
	old = evdi->swap_release_fence[display_id];
	evdi->swap_release_fence[display_id] = f;
	mutex_unlock(&evdi->fence_mutex);

	if (old)
		dma_fence_put(old);

	evdi_smp_wmb();
	atomic_set(&evdi->swap_release_ready[display_id], 1);
}

struct dma_fence *evdi_swap_release_fence_get(struct evdi_device *evdi,
					      u32 display_id)
{
	struct dma_fence *f = NULL;

	if (!evdi)
		return NULL;
	if (display_id >= LINDROID_MAX_CONNECTORS)
		return NULL;

	mutex_lock(&evdi->fence_mutex);
	f = evdi->swap_release_fence[display_id];
	if (f)
		dma_fence_get(f);
	mutex_unlock(&evdi->fence_mutex);

	return f;
}

void evdi_swap_release_fence_clear(struct evdi_device *evdi, u32 display_id)
{
	struct dma_fence *old = NULL;

	if (!evdi)
		return;
	if (display_id >= LINDROID_MAX_CONNECTORS)
		return;

	mutex_lock(&evdi->fence_mutex);
	old = evdi->swap_release_fence[display_id];
	evdi->swap_release_fence[display_id] = NULL;
	mutex_unlock(&evdi->fence_mutex);

	if (old)
		dma_fence_put(old);

	atomic_set(&evdi->swap_release_ready[display_id], 0);
}
