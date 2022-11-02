MODULE_NAME := intel-dmabuf-peer-mem-dkms

MAJOR_VERSION := 1
MINOR_VERSION := 0
PATCH_VERSION := 0

DRV_MODULE_VERSION := "$(MAJOR_VERSION).$(MINOR_VERSION)"
PKG_VERSION := $(MAJOR_VERSION).$(MINOR_VERSION).$(PATCH_VERSION)

BUILD_ID := 1

ifdef BUILD_NUMBER
	BUILD_ID := $(BUILD_NUMBER)
endif

RPM_SPEC := intel-dmabuf-peer-mem-dkms.spec
DKMS_CONFIG := dkms.conf
TARBALL = $(MODULE_NAME)-$(PKG_VERSION)-$(BUILD_ID)-src.tar.gz
TAR_CONTENT = $(RPM_SPEC) $(DKMS_CONFIG) src/*.c include/*.h README.md AUTHORS LICENSE Makefile Kbuild include udev example/Makefile example/mr-reg-xe.c

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
DMABUF_SYM ?= /var/lib/dkms/intel-dmabuf-dkms/kernel-$(KVER)-x86_64/build/Module.symvers
ifneq ($(shell test -e $(DMABUF_SYM); echo $$?), 0)
	DMABUF_SYM :=
endif

# installed by mlnx-ofa_kernel-devel package
OFA_KERNEL ?= /usr/src/ofa_kernel/default
OFA_SYM ?= $(OFA_KERNEL)/Module.symvers

OFA_STATUS = $(shell for module in `find /lib/modules/${KVER} -name ib_core.ko -print`; do \
	if [[ "`grep -qs ib_register_peer_memory_client $$module; echo $$?`" -eq 0 ]]; then \
	    echo 0; \
	fi; \
done)

.PHONY: build clean dkmsrpm-pkg

build:
ifeq ($(OFA_STATUS), 0)
	make -C $(KDIR) M=$(shell pwd) KBUILD_EXTRA_SYMBOLS="$(OFA_SYM) $(DMABUF_SYM)" KCPPFLAGS+=-DDRV_MODULE_VERSION=\'\"$(DRV_MODULE_VERSION)\"\'
else
	@echo MOFED is not installed for kernel $(KVER), skipping
endif

clean:
	rm -f $(RPM_SPEC) $(DKMS_CONFIG) src/*.o *.o *.ko *.mod *.mod.c .*.cmd src/.*.cmd modules.order Module.symvers
	$(MAKE) -C example clean

dkmsrpm-pkg:
	sed -e "s/_MODULE_NAME_/${MODULE_NAME}/;s/_VERSION_/${PKG_VERSION}/;s/_RELEASE_/${BUILD_ID}/" config_templates/$(RPM_SPEC) > $(RPM_SPEC)
	sed -e "s/_MODULE_NAME_/${MODULE_NAME}/;s/_VERSION_/${PKG_VERSION}-${BUILD_ID}/" config_templates/$(DKMS_CONFIG) > $(DKMS_CONFIG)
	tar -czf $(TARBALL) $(TAR_CONTENT)
	rpmbuild -ta $(TARBALL)
	rm $(TARBALL)
