// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Red Hat
 * Copyright (c) 2015 - 2020 DisplayLink (UK) Ltd.
 * Copyright (c) 2025 Lindroid Authors
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
#include <linux/limits.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/err.h>

static __always_inline bool evdi_bufid_valid_u32(u32 bufid)
{
	return bufid > 0 && bufid <= (u32)INT_MAX;
}

static __always_inline struct dma_fence *evdi_fence_from_syncfd(int fd)
{
	struct dma_fence *f;

	f = NULL;
	if (fd < 0)
		return NULL;

	f = sync_file_get_fence(fd);
	if (IS_ERR(f))
		return NULL;
	return f;
}

/*
 * Create a sync_file from fence and reserve an fd for it.
 * Returns:
 *  - >= 0: reserved fd, caller must fd_install(fd, *out_file)
 *  - -1  : no fence
 *  - < 0 : error
 */
static __always_inline int evdi_syncfd_reserve_from_fence(struct dma_fence *f,
							  struct file **out_file)
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

#ifdef EVDI_HAVE_XARRAY

static __always_inline void evdi_xa_store_fence(struct xarray *xa, u32 bufid,
					       struct dma_fence *f)
{
	struct dma_fence *of;
	void *old;

	old = NULL;

	if (!xa || !evdi_bufid_valid_u32(bufid) || !f) {
		if (f)
			dma_fence_put(f);
		return;
	}

	old = xa_store(xa, bufid, f, GFP_ATOMIC);

	if (xa_is_err(old)) {
		dma_fence_put(f);
		return;
	}

	if (old) {
		of = (struct dma_fence *)old;
		dma_fence_put(of);
	}
}

static __always_inline struct dma_fence *evdi_xa_erase_fence(struct xarray *xa,
							    u32 bufid)
{
	void *entry;
	struct dma_fence *f;

	entry = NULL;
	f = NULL;

	if (!xa || !bufid)
		return NULL;

	entry = xa_erase(xa, bufid);

	if (xa_is_err(entry))
		return NULL;

	f = (struct dma_fence *)entry;
	return f;
}

static void evdi_xa_purge_fences(struct xarray *xa)
{
	void *entry;
	struct dma_fence *f;

	XA_STATE(xas, xa, 0);
	entry = NULL;
	f = NULL;

	if (!xa)
		return;

	xa_lock(xa);
	xas_for_each(&xas, entry, ~0UL) {
		xas_store(&xas, NULL);
		f = (struct dma_fence *)entry;
		if (f)
			dma_fence_put(f);
	}
	xa_unlock(xa);

	xa_destroy(xa);
}

#else /* !EVDI_HAVE_XARRAY */

static __always_inline void evdi_idr_store_fence(struct idr *idr, spinlock_t *lock,
						u32 bufid, struct dma_fence *f)
{
	struct dma_fence *of;
	void *old;
	int ret;

	old = NULL;
	ret = 0;

	if (!idr || !lock || !evdi_bufid_valid_u32(bufid) || !f) {
		if (f)
			dma_fence_put(f);
		return;
	}

	spin_lock(lock);
	old = idr_replace(idr, f, (int)bufid);
	if (!IS_ERR(old)) {
		spin_unlock(lock);

		if (old) {
			of = (struct dma_fence *)old;
			dma_fence_put(of);
		}
		return;
	}

	/* Not present yet: allocate exactly at bufid. */
	ret = idr_alloc(idr, f, (int)bufid, (int)bufid + 1, GFP_ATOMIC);
	spin_unlock(lock);

	if (ret < 0)
		dma_fence_put(f);
}

static __always_inline struct dma_fence *evdi_idr_erase_fence(struct idr *idr,
							     spinlock_t *lock,
							     u32 bufid)
{
	struct dma_fence *f;

	f = NULL;

	if (!idr || !lock || !bufid)
		return NULL;

	spin_lock(lock);
	f = (struct dma_fence *)idr_remove(idr, (int)bufid);
	spin_unlock(lock);

	return f;
}

static void evdi_idr_purge_fences(struct idr *idr, spinlock_t *lock)
{
	int id;
	void *entry;
	struct dma_fence *f;

	id = 0;
	entry = NULL;
	f = NULL;

	if (!idr || !lock)
		return;

	spin_lock(lock);
	for (;;) {
		entry = idr_get_next(idr, &id);
		if (!entry)
			break;

		(void)idr_remove(idr, id);
		spin_unlock(lock);

		f = (struct dma_fence *)entry;
		if (f)
			dma_fence_put(f);

		spin_lock(lock);
		id++;
	}
	spin_unlock(lock);

	idr_destroy(idr);
}

#endif /* EVDI_HAVE_XARRAY */

void evdi_fence_tables_init(struct evdi_device *evdi)
{
	int d;

	d = 0;

	if (!evdi)
		return;

	mutex_lock(&evdi->fence_mutex);
#ifdef EVDI_HAVE_XARRAY
	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++)
		xa_init(&evdi->acquire_fence_xa[d]);
#else
	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++) {
		idr_init(&evdi->acquire_fence_idr[d]);
		spin_lock_init(&evdi->acquire_fence_lock[d]);
	}
#endif
	mutex_unlock(&evdi->fence_mutex);
}

void evdi_fence_tables_reset(struct evdi_device *evdi)
{
	int d;
	struct dma_fence *old[LINDROID_MAX_CONNECTORS] = {0};

	if (!evdi)
		return;

	mutex_lock(&evdi->fence_mutex);

	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++) {
		old[d] = evdi->swap_release_fence[d];
		evdi->swap_release_fence[d] = NULL;
		atomic_set(&evdi->swap_release_ready[d], 0);
	}

#ifdef EVDI_HAVE_XARRAY
	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++)
		evdi_xa_purge_fences(&evdi->acquire_fence_xa[d]);
	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++)
		xa_init(&evdi->acquire_fence_xa[d]);
#else
	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++)
		evdi_idr_purge_fences(&evdi->acquire_fence_idr[d], &evdi->acquire_fence_lock[d]);
	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++)
		idr_init(&evdi->acquire_fence_idr[d]);
#endif

	mutex_unlock(&evdi->fence_mutex);

	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++)
		if (old[d]) dma_fence_put(old[d]);
}

void evdi_fence_tables_cleanup(struct evdi_device *evdi)
{
	int d;
	struct dma_fence *old[LINDROID_MAX_CONNECTORS] = {0};

	d = 0;

	if (!evdi)
		return;

	mutex_lock(&evdi->fence_mutex);
#ifdef EVDI_HAVE_XARRAY
	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++)
		evdi_xa_purge_fences(&evdi->acquire_fence_xa[d]);
#else
	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++)
		evdi_idr_purge_fences(&evdi->acquire_fence_idr[d], &evdi->acquire_fence_lock[d]);
#endif
	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++) {
		old[d] = evdi->swap_release_fence[d];
		evdi->swap_release_fence[d] = NULL;
		atomic_set(&evdi->swap_release_ready[d], 0);
	}
	mutex_unlock(&evdi->fence_mutex);
	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++)
		if (old[d]) dma_fence_put(old[d]);
}

void evdi_acquire_fence_set_fd(struct evdi_device *evdi, u32 display_id, u32 bufid,
			      int acquire_fence_fd)
{
	struct dma_fence *f;

	f = NULL;

	if (!evdi)
		return;
	if (display_id >= LINDROID_MAX_CONNECTORS)
		return;
	if (!evdi_bufid_valid_u32(bufid))
		return;

	mutex_lock(&evdi->fence_mutex);

	/* Clear */
	if (acquire_fence_fd < 0) {
#ifdef EVDI_HAVE_XARRAY
		f = evdi_xa_erase_fence(&evdi->acquire_fence_xa[display_id], bufid);
#else
		f = evdi_idr_erase_fence(&evdi->acquire_fence_idr[display_id],
					 &evdi->acquire_fence_lock[display_id], bufid);
#endif
		if (f)
			dma_fence_put(f);
		mutex_unlock(&evdi->fence_mutex);
		return;
	}

	/* Set/replace */
	f = evdi_fence_from_syncfd(acquire_fence_fd);
	if (!f) {
		evdi_err("evdi: acquire fence import failed: display=%u bufid=%u fd=%d\n",
		        display_id, bufid, acquire_fence_fd);
		mutex_unlock(&evdi->fence_mutex);
		return;
	}

#ifdef EVDI_HAVE_XARRAY
	evdi_xa_store_fence(&evdi->acquire_fence_xa[display_id], bufid, f);
#else
	evdi_idr_store_fence(&evdi->acquire_fence_idr[display_id],
			     &evdi->acquire_fence_lock[display_id], bufid, f);
#endif
	mutex_unlock(&evdi->fence_mutex);
}

/*
 * Direct update from kerne takes a reference to fencel.
 */
void evdi_acquire_fence_update(struct evdi_device *evdi, u32 display_id, u32 bufid,
			      struct dma_fence *fence)
{
	if (!evdi)
		return;
	if (display_id >= LINDROID_MAX_CONNECTORS)
		return;
	if (!evdi_bufid_valid_u32(bufid))
		return;

	if (fence)
		dma_fence_get(fence);
	else
		return;

	mutex_lock(&evdi->fence_mutex);

#ifdef EVDI_HAVE_XARRAY
	evdi_xa_store_fence(&evdi->acquire_fence_xa[display_id], bufid, fence);
#else
	evdi_idr_store_fence(&evdi->acquire_fence_idr[display_id],
		     &evdi->acquire_fence_lock[display_id], bufid, fence);
#endif

	mutex_unlock(&evdi->fence_mutex);
}

static struct dma_fence *evdi_acquire_fence_erase_any_locked(struct evdi_device *evdi,
							    u32 display_id, u32 bufid)
{
	struct dma_fence *f = NULL;
	u32 d;

	if (!evdi)
		return NULL;

	if (display_id < LINDROID_MAX_CONNECTORS) {
#ifdef EVDI_HAVE_XARRAY
		f = evdi_xa_erase_fence(&evdi->acquire_fence_xa[display_id], bufid);
#else
		f = evdi_idr_erase_fence(&evdi->acquire_fence_idr[display_id],
					 &evdi->acquire_fence_lock[display_id], bufid);
#endif
		if (f)
			return f;
	}

	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++) {
		if (d == display_id)
			continue;
#ifdef EVDI_HAVE_XARRAY
		f = evdi_xa_erase_fence(&evdi->acquire_fence_xa[d], bufid);
#else
		f = evdi_idr_erase_fence(&evdi->acquire_fence_idr[d],
					 &evdi->acquire_fence_lock[d], bufid);
#endif
		if (f)
			return f;
	}
	return NULL;
}

/*
 * Consume acquire fence for (display_id, bufid), export as syncfd.
 * On success returns reserved fd and *out_file set to be fd_installed.
 */
int evdi_acquire_fence_take_export_syncfd(struct evdi_device *evdi, u32 display_id,
					 u32 bufid, struct file **out_file)
{
	struct dma_fence *f;
	int fd;

	f = NULL;
	fd = -1;

	if (!out_file)
		return -EINVAL;
	*out_file = NULL;

	if (!evdi)
		return -EINVAL;
	if (display_id >= LINDROID_MAX_CONNECTORS)
		return -EINVAL;
	if (!evdi_bufid_valid_u32(bufid))
		return -EINVAL;

	mutex_lock(&evdi->fence_mutex);
	f = evdi_acquire_fence_erase_any_locked(evdi, display_id, bufid);
	if (!f) {
		mutex_unlock(&evdi->fence_mutex);
		return -1;
	}

	fd = evdi_syncfd_reserve_from_fence(f, out_file);
	dma_fence_put(f);

	mutex_unlock(&evdi->fence_mutex);
	return fd;
}

void evdi_acquire_fence_drop(struct evdi_device *evdi, u32 display_id, u32 bufid)
{
	struct dma_fence *f;

	if (!evdi)
		return;
	if (display_id >= LINDROID_MAX_CONNECTORS)
		return;
	if (!evdi_bufid_valid_u32(bufid))
		return;

	mutex_lock(&evdi->fence_mutex);
#ifdef EVDI_HAVE_XARRAY
	f = evdi_xa_erase_fence(&evdi->acquire_fence_xa[display_id], bufid);
#else
	f = evdi_idr_erase_fence(&evdi->acquire_fence_idr[display_id],
				 &evdi->acquire_fence_lock[display_id], bufid);
#endif
	mutex_unlock(&evdi->fence_mutex);

	if (f)
		dma_fence_put(f);
}

void evdi_acquire_fence_drop_all(struct evdi_device *evdi, u32 bufid)
{
	u32 d;

	if (!evdi)
		return;
	if (!evdi_bufid_valid_u32(bufid))
		return;

	for (d = 0; d < LINDROID_MAX_CONNECTORS; d++)
		evdi_acquire_fence_drop(evdi, d, bufid);
}

void evdi_swap_release_fence_set_fd(struct evdi_device *evdi, u32 display_id, int release_fence_fd)
{
	struct dma_fence *f = NULL;
	struct dma_fence *old = NULL;

	f = NULL;

	if (!evdi)
		return;
	if (display_id >= LINDROID_MAX_CONNECTORS)
		return;

	if (release_fence_fd >= 0)
		f = evdi_fence_from_syncfd(release_fence_fd);

	mutex_lock(&evdi->fence_mutex);
	old = evdi->swap_release_fence[display_id];
	evdi->swap_release_fence[display_id] = f;
	mutex_unlock(&evdi->fence_mutex);

	if (old)
		dma_fence_put(old);
	evdi_smp_wmb();
	atomic_set(&evdi->swap_release_ready[display_id], 1);
}

struct dma_fence *evdi_swap_release_fence_get(struct evdi_device *evdi, u32 display_id)
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
