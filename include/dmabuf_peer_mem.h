/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2022 Intel Corporation
 */

#ifndef __DMABUF_PEER_MEM_H__
#define __DMABUF_PEER_MEM_H__

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/dma-resv.h>

struct dmabuf_peer_mem_context {
	u64 base;
	u64 size;
	int fd;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct scatterlist *first_sg;
	struct scatterlist *last_sg;
	unsigned long first_sg_offset;
	unsigned long last_sg_trim;
	unsigned long page_size;
	unsigned long umem_addr;
	unsigned long umem_len;
	unsigned int nmap;
	u64 core_context;
	struct device *dma_device;
	struct mutex mutex;
};

int dmabuf_peer_mem_unmap_pages(struct dmabuf_peer_mem_context *dmabuf_mem_context);
void dmabuf_peer_mem_invalidate_cb(struct dma_buf_attachment *attach);
void dmabuf_peer_mem_detach(struct dmabuf_peer_mem_context *dmabuf_mem_context);
int dmabuf_peer_mem_attach(struct dmabuf_peer_mem_context *dmabuf_mem_context,
			     struct device *dma_device);
int dmabuf_peer_mem_map_pages(struct dmabuf_peer_mem_context *dmabuf_mem_context);

#endif /* __DMABUF_PEER_MEM_H__ */
