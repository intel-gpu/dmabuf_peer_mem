# Intel® RDMA Enablement Module

This branch provides dmabuf_peer_mem driver source code prepared for **SUSE® Linux® Enterprise Server**.

# Branches

 - **suse/main** will point to the currently supported version of SUSE® Linux® Enterprise Server.  
 A new branch suse/sles<x.y> will be added whenever a version is deprecated or moved to the maintenance phase.

# Prerequisites

A successful installation requires the **i915 main graphics driver** and **Mellanox OFED**.
The following **i915 driver** must be used for proper operation:

- [Intel® Graphics Driver Backports for Linux](https://github.com/intel-gpu/intel-gpu-i915-backports)

There are also dependencies on the following system packages:
  - **dkms**
  - **make**
  - **lsb-release**
  - **rpm-build**

# Installation

The **dmabuf_peer_mem** dkms package should be created with the following command

```
make dkmsrpm-pkg
```

The above command will create an rpm package at ~/rpmbuild/RPMS/. Then install it.

```
sudo rpm -ivh intel-dmabuf-peer-mem-dkms-*.rpm
```

Check the installation status of the driver:

```
dkms status intel-dmabuf-peer-mem-dkms
```

Reboot the system after the installation is complete.
After a successful installation and reboot the driver should be loaded automatically.
