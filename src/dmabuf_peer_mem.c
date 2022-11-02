// SPDX-License-Identifier: BSD-2-Clause
//
// Copyright (c) 2022 Intel Corporation

#include <linux/module.h>
#include "dmabuf_reg.h"
#include "dmabuf_nv_peer_mem.h"
#include "dmabuf_peer_mem.h"

#define DRV_NAME	"dmabuf_peer_mem"
#ifdef DRV_MODULE_VERSION
#define DRV_VERSION	DRV_MODULE_VERSION
#else
#define DRV_VERSION	"1.0"
#endif

MODULE_AUTHOR("Jianxin Xiong");
MODULE_DESCRIPTION("Intel RDMA Enablement Module");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION(DRV_VERSION);

#ifndef for_each_sgtable_dma_sg
#define for_each_sgtable_dma_sg(sgt, sg, i) \
	for_each_sg((sgt)->sgl, sg, (sgt)->nents, i)
#endif

/*
 * DMA mapping/unmapping routines
 */

int dmabuf_peer_mem_unmap_pages(struct dmabuf_peer_mem_context *dmabuf_mem_context)
{
	dma_resv_assert_held(dmabuf_mem_context->attach->dmabuf->resv);

	if (!dmabuf_mem_context->sgt)
		return 0;

	/* retore the original sg list */
	if (dmabuf_mem_context->first_sg) {
		sg_dma_address(dmabuf_mem_context->first_sg) -=
			dmabuf_mem_context->first_sg_offset;
		sg_dma_len(dmabuf_mem_context->first_sg) +=
			dmabuf_mem_context->first_sg_offset;
		dmabuf_mem_context->first_sg = NULL;
		dmabuf_mem_context->first_sg_offset = 0;
	}
	if (dmabuf_mem_context->last_sg) {
		sg_dma_len(dmabuf_mem_context->last_sg) +=
			dmabuf_mem_context->last_sg_trim;
		dmabuf_mem_context->last_sg = NULL;
		dmabuf_mem_context->last_sg_trim = 0;
	}

	dma_buf_unmap_attachment(dmabuf_mem_context->attach,
				 dmabuf_mem_context->sgt,
				 DMA_BIDIRECTIONAL);

	dmabuf_mem_context->sgt = NULL;
	return 0;
}

void dmabuf_peer_mem_invalidate_cb(struct dma_buf_attachment *attach)
{
	struct dmabuf_peer_mem_context *dmabuf_mem_context = attach->importer_priv;

	dma_resv_assert_held(dmabuf_mem_context->attach->dmabuf->resv);

	if (!dmabuf_mem_context->sgt)
		return;
	_dmabuf_nv_peer_mem_core_invalidate_cb(dmabuf_mem_context->core_context);

	dmabuf_peer_mem_unmap_pages(dmabuf_mem_context);
}

static struct dma_buf_attach_ops dmabuf_peer_mem_attach_ops = {
	.allow_peer2peer = 1,
	.move_notify = dmabuf_peer_mem_invalidate_cb,
};

void dmabuf_peer_mem_detach(struct dmabuf_peer_mem_context *dmabuf_mem_context)
{
	if (dmabuf_mem_context->attach) {
		dma_buf_detach(dmabuf_mem_context->dmabuf,
			       dmabuf_mem_context->attach);
		dmabuf_mem_context->attach = NULL;
		dmabuf_mem_context->dma_device = NULL;
	}
}

int dmabuf_peer_mem_attach(struct dmabuf_peer_mem_context *dmabuf_mem_context,
			     struct device *dma_device)
{
	struct dma_buf_attachment *attach;

	if (dmabuf_mem_context->attach &&
	    dmabuf_mem_context->dma_device == dma_device)
		return 0;

	dmabuf_peer_mem_detach(dmabuf_mem_context);

	attach = dma_buf_dynamic_attach(dmabuf_mem_context->dmabuf,
					dma_device,
					&dmabuf_peer_mem_attach_ops,
					dmabuf_mem_context);

	if (IS_ERR(attach))
		return PTR_ERR(attach);

	dmabuf_mem_context->attach = attach;
	dmabuf_mem_context->dma_device = dma_device;
	return 0;
}

int dmabuf_peer_mem_map_pages(struct dmabuf_peer_mem_context *dmabuf_mem_context)
{
	struct sg_table *sgt;
	struct scatterlist *sg;
	struct dma_fence *fence;
	unsigned long start, end, cur, offset, trim;
	unsigned int nmap = 0;
	int i;

	dma_resv_assert_held(dmabuf_mem_context->attach->dmabuf->resv);

	if (dmabuf_mem_context->sgt)
		goto wait_fence;

	sgt = dma_buf_map_attachment(dmabuf_mem_context->attach,
				     DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt))
		return PTR_ERR(sgt);

	/* modify the sg list in-place to match umem address and length */

	start = ALIGN_DOWN(dmabuf_mem_context->umem_addr, PAGE_SIZE);
	end = PAGE_ALIGN(dmabuf_mem_context->umem_addr +
			 dmabuf_mem_context->umem_len);
	cur = dmabuf_mem_context->base;

	for_each_sgtable_dma_sg(sgt, sg, i) {
		if (start < cur + sg_dma_len(sg) && cur < end)
			nmap++;
		if (cur <= start && start < cur + sg_dma_len(sg)) {
			offset = start - cur;
			dmabuf_mem_context->first_sg = sg;
			dmabuf_mem_context->first_sg_offset = offset;
			sg_dma_address(sg) += offset;
			sg_dma_len(sg) -= offset;
			cur += offset;
		}
		if (cur < end && end <= cur + sg_dma_len(sg)) {
			trim = cur + sg_dma_len(sg) - end;
			dmabuf_mem_context->last_sg = sg;
			dmabuf_mem_context->last_sg_trim = trim;
			sg_dma_len(sg) -= trim;
			break;
		}
		cur += sg_dma_len(sg);
	}

	dmabuf_mem_context->sgt = sgt;
	dmabuf_mem_context->nmap = nmap;

wait_fence:
	/*
	 * Although the sg list is valid now, the content of the pages may be
	 * not up-to-date. Wait for the exporter to finish the migration.
	 */
	fence = dma_resv_excl_fence(dmabuf_mem_context->attach->dmabuf->resv);
	if (fence)
		return dma_fence_wait(fence, false);
	return 0;
}


static int __init dmabuf_peer_mem_init(void)
{
	int err;

	err = dmabuf_reg_init();
	if (err)
		return err;

	err = _dmabuf_nv_peer_mem_init(DRV_NAME, DRV_VERSION);
	if (err) {
		dmabuf_reg_fini();
		return err;
	}
	return 0;
}

static void __exit dmabuf_peer_mem_cleanup(void)
{
	_dmabuf_nv_peer_mem_fini();
	dmabuf_reg_fini();
}

module_init(dmabuf_peer_mem_init);
module_exit(dmabuf_peer_mem_cleanup);
