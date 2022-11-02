// SPDX-License-Identifier: BSD-2-Clause
//
// Copyright (c) 2022 Intel Corporation
//

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <infiniband/verbs.h>
#include <level_zero/ze_api.h>

enum buf_location {
	MALLOC,
	HOST,
	DEVICE,
	SHARED
};

static char *buf_location_string[4] = {
	"malloc",
	"host",
	"device",
	"shared"
};

#define CHECK_ERROR(stmt)					\
do {								\
	int err = (stmt);					\
	if (err) {						\
		perror(#stmt);					\
		printf("%s returned error %d\n", #stmt, (err));	\
		exit(-1);					\
	}							\
} while (0)

#define CHECK_NULL(stmt)				\
do {							\
	if (!(stmt)) {					\
		perror(#stmt);				\
		printf("%s returned NULL\n", #stmt);	\
		exit(-1);				\
	}						\
} while (0)

/*
 * dmabuf registry for peer-memory access
 */

#include "dmabuf_reg.h"

static char *dmabuf_reg_dev_name = "/dev/" DMABUF_REG_DEV_NAME;
static int dmabuf_reg_fd = -1;

int dmabuf_reg_open(void)
{
	int fd;

	fd = open(dmabuf_reg_dev_name, 0);
	if (fd < 0) {
		perror(dmabuf_reg_dev_name);
		return fd;
	}

	dmabuf_reg_fd = fd;
	return 0;
}

void dmabuf_reg_close(void)
{
	if (dmabuf_reg_fd >= 0)
		close(dmabuf_reg_fd);
}

int dmabuf_reg_add(uint64_t base, uint64_t size, int fd)
{
	struct dmabuf_reg_param args = {
		.op = DMABUF_REG_ADD,
		.base = base,
		.size = size,
		.fd = fd,
	};
	int err;

	err = ioctl(dmabuf_reg_fd, DMABUF_REG_IOCTL, &args);
	return err ? -errno : 0;
}

int dmabuf_reg_remove_addr(uint64_t addr)
{
	struct dmabuf_reg_param args = {
		.op = DMABUF_REG_REMOVE_ADDR,
		.base = addr,
	};
	int err;

	err = ioctl(dmabuf_reg_fd, DMABUF_REG_IOCTL, &args);
	return err ? -errno : 0;
}

int dmabuf_reg_remove_fd(int fd)
{
	struct dmabuf_reg_param args = {
		.op = DMABUF_REG_REMOVE_FD,
		.fd = fd,
	};
	int err;

	err = ioctl(dmabuf_reg_fd, DMABUF_REG_IOCTL, &args);
	return err ? -errno : 0;
}

int dmabuf_reg_query(uint64_t addr, uint64_t len, int *fd)
{
	struct dmabuf_reg_param args = {
		.op = DMABUF_REG_QUERY,
		.base = addr,
		.size = len,
	};
	int err;

	err = ioctl(dmabuf_reg_fd, DMABUF_REG_IOCTL, &args);
	if (err)
		return -errno;
		/* -ENOENT: the address range is empty in the registry */
		/* -EINVAL: the address range is partially covered by an registry entries */

	*fd = args.fd;
	return 0;
}

/*
 * Memory allocation & copy routines using oneAPI L0
 */

static int card_num;
static ze_driver_handle_t gpu_driver;
static ze_context_handle_t gpu_context;
static ze_device_handle_t gpu_device;

void xe_init(void)
{
	int count;
	ze_device_handle_t *devices;
	ze_device_properties_t device_properties;
	ze_context_desc_t ctxt_desc = {};
	ze_bool_t succ;

	CHECK_ERROR(zeInit(ZE_INIT_FLAG_GPU_ONLY));

	count = 1;
	CHECK_ERROR(zeDriverGet(&count, &gpu_driver));
	printf("%s: driver 0 total >=%d\n", __func__, count);

	count = 0;
	CHECK_ERROR(zeDeviceGet(gpu_driver, &count, NULL));

	if (count < card_num + 1) {
		fprintf(stderr, "device %d does not exist\n", card_num);
		exit(-1);
	}

	devices = calloc(count, sizeof(*devices));
	if (!devices) {
		perror("calloc");
		exit(-1);
	}

	CHECK_ERROR(zeDeviceGet(gpu_driver, &count, devices));
	gpu_device = devices[card_num];

	CHECK_ERROR(zeDeviceGetProperties(gpu_device, &device_properties));
	printf("%s: device %d total %d vendor 0x%x device 0x%x %s\n",
		__func__, card_num, count, device_properties.vendorId,
		device_properties.deviceId, device_properties.name);

	CHECK_ERROR(zeContextCreate(gpu_driver, &ctxt_desc, &gpu_context));
}

int xe_get_buf_fd(void *buf)
{
	ze_ipc_mem_handle_t ipc;

	memset(&ipc, 0, sizeof(ipc));
	CHECK_ERROR((zeMemGetIpcHandle(gpu_context, buf, &ipc)));

	return (int)*(uint64_t *)&ipc;
}

void *xe_alloc_buf(size_t page_size, size_t size, int where)
{
	void *buf = NULL;
	void *alloc_base;
	size_t alloc_size;
	ze_device_mem_alloc_desc_t dev_desc = {
	.stype = ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC,
	};
	ze_host_mem_alloc_desc_t host_desc = {
	.stype = ZE_STRUCTURE_TYPE_HOST_MEM_ALLOC_DESC,
	};
	int fd;

	switch (where) {
	case MALLOC:
		CHECK_ERROR(posix_memalign(&buf, page_size, size));
		break;
	case HOST:
		CHECK_ERROR(zeMemAllocHost(gpu_context, &host_desc,
			size, page_size, &buf));
		break;
	case DEVICE:
		CHECK_ERROR(zeMemAllocDevice(gpu_context, &dev_desc,
				size, page_size, gpu_device, &buf));
		break;
	case SHARED:
	default:
		CHECK_ERROR(zeMemAllocShared(gpu_context, &dev_desc, &host_desc,
				size, page_size, gpu_device, &buf));
		break;
	}

	if (where == MALLOC) {
		alloc_base = buf;
		alloc_size = size;
		fd = -1;
	} else {
		fd = xe_get_buf_fd(buf);
		CHECK_ERROR(zeMemGetAddressRange(gpu_context, buf, &alloc_base, &alloc_size));
		CHECK_ERROR(dmabuf_reg_add((uintptr_t)alloc_base, alloc_size, fd));
	}

	printf("%s: %s buf %p base %p size %ld fd %d\n", __func__,
		buf_location_string[where], buf, alloc_base, alloc_size, fd);
	return buf;
}

void xe_free_buf(void *buf, int where)
{
	if (where == MALLOC) {
		free(buf);
	} else {
		CHECK_ERROR(dmabuf_reg_remove_addr((uint64_t)buf));
		CHECK_ERROR(zeMemFree(gpu_context, buf));
	}
}

/*
 * Buffer initialization & finalization
 */

#define MAX_BUFS 16
static int			num_bufs = 1;
static void			*buf[MAX_BUFS];
static size_t			buf_size = 65536;
static enum buf_location	buf_location = MALLOC;

static void init_buf(void)
{
	int page_size = sysconf(_SC_PAGESIZE);
	int i;

	for (i = 0; i < num_bufs; i++) {
		buf[i] = xe_alloc_buf(page_size, buf_size, buf_location);

		if (!buf[i]) {
			fprintf(stderr, "Couldn't allocate work buf.\n");
			exit(-1);
		}
	}
}

static void free_buf(void)
{
	int i;

	for (i = 0; i < num_bufs; i++)
		xe_free_buf(buf[i], buf_location);
}

/*
 * Verbs initialization & finalization
 */


static int do_ib_mr = 1;
static struct ibv_device	**dev_list;
static struct ibv_device	*dev;
static struct ibv_context	*context;
static struct ibv_pd		*pd;
static struct ibv_mr		*mr;

static void free_ib(void)
{
	ibv_dealloc_pd(pd);
	ibv_close_device(context);
	ibv_free_device_list(dev_list);
}

static int init_ib(void)
{
	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		perror("Failed to get IB devices list");
		exit(-1);
	}

	dev = *dev_list;
	if (!dev) {
		fprintf(stderr, "No IB devices found\n");
		exit(-1);
	}

	printf("%s: device %s\n", __func__, ibv_get_device_name(dev));
	CHECK_NULL((context = ibv_open_device(dev)));
	CHECK_NULL((pd = ibv_alloc_pd(context)));
	return 0;
}

static int reg_mr(void)
{
	int mr_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
			      IBV_ACCESS_REMOTE_WRITE;
	int i;

	for (i = 0; i < num_bufs; i++) {
		CHECK_NULL((mr = ibv_reg_mr(pd, buf[i], buf_size, mr_access_flags)));
		printf("%s: mr %p\n", __func__, mr);
	}
	return 0;
}

static void dereg_mr(void)
{
	if (mr)
		ibv_dereg_mr(mr);
}

static void usage(char *prog)
{
	printf("Usage: %s [-m <location>][-d <card_num>][-n <num_bufs>][-s <buf_size>][-h]\n", prog);
	printf("Options:\n");
	printf("\t-m <location>    Where to allocate the buffer, can be 'malloc', 'host', 'device' or 'shared', default: malloc\n");
	printf("\t-d <card_num>    Use the GPU device specified by <card_num>, default: 1\n");
	printf("\t-n <num_bufs>    Set the number of buffers to allocate, default: 1, max: %d\n", MAX_BUFS);
	printf("\t-s <buf_size>    Set the size of the buffer to allocate, default: 65536\n");
	printf("\t-i               Skip memory registration in the IB\n");
	printf("\t-h               Print this message\n");
}

int main(int argc, char *argv[])
{
	int c;

	while ((c = getopt(argc, argv, "d:m:n:s:ih")) != -1) {
		switch (c) {
		case 'd':
			card_num = atoi(optarg);
			break;
		case 'm':
			if (strcasecmp(optarg, "malloc") == 0)
				buf_location = MALLOC;
			else if (strcasecmp(optarg, "host") == 0)
				buf_location = HOST;
			else if (strcasecmp(optarg, "device") == 0)
				buf_location = DEVICE;
			else if (strcasecmp(optarg, "shared") == 0)
				buf_location = SHARED;
			break;
		case 'n':
			num_bufs = atol(optarg);
			if (num_bufs > MAX_BUFS)
				num_bufs = MAX_BUFS;
			break;
		case 's':
			buf_size = atol(optarg);
			break;
		case 'i':
			do_ib_mr = 0;
			break;
		default:
			usage(argv[0]);
			exit(-1);
		}
	}

	dmabuf_reg_open();

	if (buf_location != MALLOC)
		xe_init();

	init_buf();

	if (do_ib_mr == 1) {
		init_ib();
		reg_mr();
		dereg_mr();
		free_ib();
	}

	free_buf();

	dmabuf_reg_close();
	return 0;
}
