/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2022 Intel Corporation
 */

#ifndef __DMABUF_REG_H__
#define __DMABUF_REG_H__

#include "uapi/dmabuf_reg.h"

int	dmabuf_reg_init(void);
void	dmabuf_reg_fini(void);
int	dmabuf_reg_query(u64 start, u64 len, u64 *base, u64 *size, int *fd);

#endif /* __DMABUF_REG_H__ */
