# Intel® RDMA Enablement Module

This is a Linux kernel mode driver that enables direct P2P communication between the memory
of an Intel GPU card that supports the DMA-Buf framework and an HCA device that supports 
the PeerDirect interface from Mellanox.

It provides two interfaces:

- As a Peer-Memory client: **Peer-Memory** is a Mellanox OFED (MOFED) specific kernel
API that allows the owner of a memory allocation to claim the ownership and provide
virtual address to DMA address translation for the RDMA drivers. The dmabuf_peer_mem
KMD registers itself with MOFED as a **Peer-Memory** client.

- As a dma-buf memory registry. The registry is a database of user supplied information of
DMA-buf based memory allocations, including the virtuall address range and the file descriptor.
The registry is accessed via ioctl interface over a character device (`/dev/dmabuf_reg`).


# Dependencies

This driver is part of a collection of kernel-mode drivers that enable support for Intel graphics.
The main graphics driver listed below must be installed for the driver to work properly:

- [Intel® Graphics Driver Backports for Linux](https://github.com/intel-gpu/intel-gpu-i915-backports) - The main graphics driver (includes a compatible DRM subsystem and dmabuf if necessary)

# Supported OS Distributions

|   OSV |Branch   	| installation instructions | 
|---	|---	| --- |
| SUSE® Linux® Enterprise Server 15SP3	| [suse/main](https://github.com/intel-gpu/dmabuf_peer_mem/tree/suse/main) |[Readme](https://github.com/intel-gpu/dmabuf_peer_mem/blob/suse/main/README.md)|
