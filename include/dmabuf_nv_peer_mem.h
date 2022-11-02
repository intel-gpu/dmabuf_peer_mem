/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2022 Intel Corporation
 */

#ifndef DMABUF_NV_PEER_MEM_H
#define DMABUF_NV_PEER_MEM_H

void _dmabuf_nv_peer_mem_fini(void);
int _dmabuf_nv_peer_mem_init(const char *DRV_NAME, const char *DRV_VERSION);
void _dmabuf_nv_peer_mem_core_invalidate_cb(u64 core_context);

#endif // DMABUF_NV_PEER_MEM_H
