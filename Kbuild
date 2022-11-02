KVER ?= $(shell uname -r)
OFA_KERNEL ?= /usr/src/ofa_kernel/default

# 'dkms status' output format:
#   <name>, <version>, <kernel>, <arch>: installed (original_module exists)
#   <name>, <version>: added
DMABUF_DKMS_STATUS := $(shell /usr/sbin/dkms status -m intel-dmabuf-dkms -k $(KVER))
DMABUF_DKMS_INSTALL := $(shell echo "$(DMABUF_DKMS_STATUS)" | cut -d : -f 2 | sed 's/(.*)//' | sed 's/ //g')

ifeq ($(DMABUF_DKMS_INSTALL), installed)
	DMABUF_DKMS_VER := $(shell echo "$(DMABUF_DKMS_STATUS)" | cut -d , -f 2 | sed 's/ //g')
	BACKPORT_DIR := /var/lib/dkms/intel-dmabuf-dkms/$(DMABUF_DKMS_VER)/source
	LINUXINCLUDE := \
		-DCONFIG_BACKPORT_INTEGRATE \
		-I$(BACKPORT_DIR)/backport-include/ \
		-I$(BACKPORT_DIR)/backport-include/uapi \
		-I$(BACKPORT_DIR)/include \
		-I$(BACKPORT_DIR)/include/uapi \
		-include $(BACKPORT_DIR)/backport-include/backport/backport.h \
		${LINUXINCLUDE}
	CFLAGS_src/dmabuf_nv_peer_mem.o := -I$(OFA_KERNEL)/include
else
	ccflags-y += -I$(OFA_KERNEL)/include
endif

ccflags-y += -I$(src)/include

obj-m = dmabuf_peer_mem.o

dmabuf_peer_mem-y := src/dmabuf_peer_mem.o src/dmabuf_reg.o src/dmabuf_nv_peer_mem.o
