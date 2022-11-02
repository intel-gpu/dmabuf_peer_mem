// SPDX-License-Identifier: BSD-2-Clause
//
// Copyright (c) 2022 Intel Corporation


#include <linux/module.h>
#include <linux/scatterlist.h>
#include <rdma/peer_mem.h>
#include "dmabuf_nv_peer_mem.h"
#include "dmabuf_peer_mem.h"
#include "dmabuf_reg.h"

static invalidate_peer_memory core_invalidate_cb;
static void *dmabuf_nv_peer_mem_reg_handle;

/*
 * Peer-memory client functions
 */

/* return code: 1 - mine, 0 - not mine */
static int dmabuf_nv_peer_mem_acquire(unsigned long addr, size_t size,
			      void *peer_mem_private_data,
			      char *peer_mem_name, void **client_context)
{
	struct dmabuf_peer_mem_context *dmabuf_mem_context;
	struct dma_buf *dmabuf;
	u64 reg_base, reg_size;
	int fd, err;

	if (!client_context)
		return 0;

	err = dmabuf_reg_query(addr, size, &reg_base, &reg_size, &fd);
	if (err)
		return 0;

	dmabuf_mem_context = kzalloc(sizeof(*dmabuf_mem_context), GFP_KERNEL);
	if (!dmabuf_mem_context)
		return 0;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		goto err_free;

	if (dmabuf->size < addr - reg_base + size)
		goto err_put;

	dmabuf_mem_context->dmabuf = dmabuf;
	dmabuf_mem_context->base = reg_base;
	dmabuf_mem_context->size = reg_size;
	dmabuf_mem_context->fd = fd;
	mutex_init(&dmabuf_mem_context->mutex);

	*client_context = dmabuf_mem_context;
	__module_get(THIS_MODULE);
	return 1;

err_put:
	dma_buf_put(dmabuf);

err_free:
	kfree(dmabuf_mem_context);
	return 0;
}

static int dmabuf_nv_peer_mem_get_pages(unsigned long addr, size_t size, int write,
				int force, struct sg_table *sg_head,
				void *context, u64 core_context)
{
	struct dmabuf_peer_mem_context *dmabuf_mem_context = context;

	if (!context)
		return -EINVAL;

	mutex_lock(&dmabuf_mem_context->mutex);

	dmabuf_mem_context->page_size = PAGE_SIZE;
	dmabuf_mem_context->umem_addr = addr;
	dmabuf_mem_context->umem_len = size;
	dmabuf_mem_context->core_context = core_context;

	/*
	 * we can't do more here becasue dma_buf_attach() needs dma_device
	 * which is only available when dma_map() is called. It won't be an
	 * issue though because get_pages() and dma_map() are called in a
	 * row w/o anything touching the sg_table.
	 */

	mutex_unlock(&dmabuf_mem_context->mutex);
	return 0;
}

static int dmabuf_nv_peer_mem_dma_map(struct sg_table *sg_head, void *context,
			      struct device *dma_device, int dmasync,
			      int *nmap)
{
	struct dmabuf_peer_mem_context *dmabuf_mem_context = context;
	int err;

	if (!context || !sg_head || !dma_device || !nmap)
		return -EINVAL;

	mutex_lock(&dmabuf_mem_context->mutex);
	err = dmabuf_peer_mem_attach(dmabuf_mem_context, dma_device);
	if (err) {
		mutex_unlock(&dmabuf_mem_context->mutex);
		return err;
	}

	dma_resv_lock(dmabuf_mem_context->attach->dmabuf->resv, NULL);

	err = dmabuf_peer_mem_map_pages(dmabuf_mem_context);
	if (err)
		goto out;

	sg_head->sgl = dmabuf_mem_context->first_sg;
	sg_head->nents = *nmap = dmabuf_mem_context->nmap;

out:
	dma_resv_unlock(dmabuf_mem_context->attach->dmabuf->resv);
	mutex_unlock(&dmabuf_mem_context->mutex);
	return err;
}

static int dmabuf_nv_peer_mem_dma_unmap(struct sg_table *sg_head, void *context,
				struct device *dma_device)
{
	struct dmabuf_peer_mem_context *dmabuf_mem_context = context;
	int err;

	if (!context || !sg_head)
		return -EINVAL;

	mutex_lock(&dmabuf_mem_context->mutex);
	dma_resv_lock(dmabuf_mem_context->attach->dmabuf->resv, NULL);

	err = dmabuf_peer_mem_unmap_pages(dmabuf_mem_context);
	if (err)
		goto out;

	sg_head->sgl = NULL;
	sg_head->nents = 0;

out:
	dma_resv_unlock(dmabuf_mem_context->attach->dmabuf->resv);
	mutex_unlock(&dmabuf_mem_context->mutex);
	return err;
}

static void dmabuf_nv_peer_mem_put_pages(struct sg_table *sg_head, void *context)
{
	struct dmabuf_peer_mem_context *dmabuf_mem_context = context;

	if (!context)
		return;

	mutex_lock(&dmabuf_mem_context->mutex);
	dmabuf_peer_mem_detach(dmabuf_mem_context);
	mutex_unlock(&dmabuf_mem_context->mutex);
}

static void dmabuf_nv_peer_mem_release(void *context)
{
	struct dmabuf_peer_mem_context *dmabuf_mem_context = context;

	if (!context)
		return;

	dma_buf_put(dmabuf_mem_context->dmabuf);
	kfree(dmabuf_mem_context);
	module_put(THIS_MODULE);
}

static unsigned long dmabuf_nv_peer_mem_get_page_size(void *context)
{
	struct dmabuf_peer_mem_context *dmabuf_mem_context = context;

	if (!context)
		return -EINVAL;

	return dmabuf_mem_context->page_size;
}

static struct peer_memory_client dmabuf_nv_peer_mem_client = {
	.acquire	= dmabuf_nv_peer_mem_acquire,
	.get_pages	= dmabuf_nv_peer_mem_get_pages,
	.dma_map	= dmabuf_nv_peer_mem_dma_map,
	.dma_unmap	= dmabuf_nv_peer_mem_dma_unmap,
	.put_pages	= dmabuf_nv_peer_mem_put_pages,
	.get_page_size	= dmabuf_nv_peer_mem_get_page_size,
	.release	= dmabuf_nv_peer_mem_release,
};


void _dmabuf_nv_peer_mem_fini(void)
{
	ib_unregister_peer_memory_client(dmabuf_nv_peer_mem_reg_handle);
}

int _dmabuf_nv_peer_mem_init(const char *DRV_NAME, const char *DRV_VERSION)
{
	strcpy(dmabuf_nv_peer_mem_client.name, DRV_NAME);
	strcpy(dmabuf_nv_peer_mem_client.version, DRV_VERSION);
	dmabuf_nv_peer_mem_reg_handle =
		ib_register_peer_memory_client(&dmabuf_nv_peer_mem_client,
					       &core_invalidate_cb);
	if (!dmabuf_nv_peer_mem_reg_handle)
		return -EINVAL;

	return 0;
}

void _dmabuf_nv_peer_mem_core_invalidate_cb(u64 core_context)
{
	(*core_invalidate_cb)(dmabuf_nv_peer_mem_reg_handle,
			core_context);
}
