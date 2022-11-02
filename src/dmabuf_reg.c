// SPDX-License-Identifier: BSD-2-Clause
//
// Copyright (c) 2022 Intel Corporation


#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sched/mm.h>
#include "dmabuf_reg.h"

#define DEV_NAME	DMABUF_REG_DEV_NAME
#define CLASS_NAME	DMABUF_REG_DEV_NAME

struct dmabuf_entry {
	struct rb_node node;		/* rbtree entry for range table */
	struct dmabuf_ucontext *ucontext;
	u64 base;
	u64 size;
	u32 fd;
	struct kref refcnt;		/* count for repetitive registrations */
};

struct dmabuf_ucontext {
	struct rb_node node;		/* rbtree entry for ucontext table */
	struct mm_struct *mm;		/* to identify the accress space */
	struct xarray fd_table;		/* registered dmabuf fds */
	struct rb_root range_table;	/* address ranges of dmabufs */
	struct kref refcnt;		/* count for opens from the same process */
	struct mutex mutex;
};

/*
 * The dmabuf registry is only valid within a specific address space,
 * represented here as "ucontext". The mm_struct pointer of the current
 * process is used as the key to identify the address space. This is
 * necessary because the producer (i.e. the device memory allocator) and
 * the consumer (i.e. the rdma driver) of the dmabuf info don't have other
 * way to share a context reference.
 */

static DEFINE_MUTEX(ucontext_table_mutex);
static struct rb_root ucontext_table = RB_ROOT;

static struct dmabuf_ucontext *dmabuf_ucontext_get(struct mm_struct *mm)
{
	struct rb_node *node;
	struct dmabuf_ucontext *ucontext;

	mutex_lock(&ucontext_table_mutex);
	node = ucontext_table.rb_node;
	while (node) {
		ucontext = container_of(node, struct dmabuf_ucontext, node);
		if (mm == ucontext->mm) {
			kref_get(&ucontext->refcnt);
			mutex_unlock(&ucontext_table_mutex);
			return ucontext;
		}

		if (mm < ucontext->mm)
			node = node->rb_left;
		else
			node = node->rb_right;
	}
	mutex_unlock(&ucontext_table_mutex);
	return NULL;
}

static int dmabuf_ucontext_insert(struct dmabuf_ucontext *ucontext)
{
	struct rb_root *root = &ucontext_table;
	struct rb_node **new, *parent = NULL;

	mutex_lock(&ucontext_table_mutex);
	new = &root->rb_node;
	while (*new) {
		struct dmabuf_ucontext *p =
			container_of((*new), struct dmabuf_ucontext, node);

		parent = *new;
		if (ucontext->mm == p->mm) {
			mutex_unlock(&ucontext_table_mutex);
			return -EEXIST;
		}

		if (ucontext->mm < p->mm)
			new = &((*new)->rb_left);
		else
			new = &((*new)->rb_right);
	}

	rb_link_node(&ucontext->node, parent, new);
	rb_insert_color(&ucontext->node, root);
	mutex_unlock(&ucontext_table_mutex);
	return 0;
}

static void dmabuf_ucontext_delete(struct dmabuf_ucontext *ucontext)
{
	mutex_lock(&ucontext_table_mutex);
	rb_erase(&ucontext->node, &ucontext_table);
	mutex_unlock(&ucontext_table_mutex);
}

static struct dmabuf_ucontext *dmabuf_ucontext_get_or_new(void)
{
	struct mm_struct *mm = current->mm;
	struct dmabuf_ucontext *ucontext;
	int err;

	ucontext = dmabuf_ucontext_get(mm);
	if (ucontext)
		return ucontext;

	ucontext = kzalloc(sizeof(*ucontext), GFP_KERNEL);
	if (!ucontext)
		return ERR_PTR(-ENOMEM);

	mutex_init(&ucontext->mutex);
	kref_init(&ucontext->refcnt);
	xa_init(&ucontext->fd_table);
	ucontext->range_table = RB_ROOT;
	ucontext->mm = mm;

	err = dmabuf_ucontext_insert(ucontext);
	if (err) {
		kfree(ucontext);
		return ERR_PTR(err);
	}

	mmgrab(mm);
	return ucontext;
}

static void dmabuf_ucontext_release(struct kref *kref)
{
	struct dmabuf_ucontext *ucontext;
	struct dmabuf_entry *entry;
	unsigned long idx;

	ucontext = container_of(kref, struct dmabuf_ucontext, refcnt);
	dmabuf_ucontext_delete(ucontext);

	xa_for_each(&ucontext->fd_table, idx, entry)
		kfree(entry);
	xa_destroy(&ucontext->fd_table);
	/* The same nodes are used in rbtree so no need to erase range_table */

	mmdrop(ucontext->mm);
	kfree(ucontext);
}

static void dmabuf_ucontext_put(struct dmabuf_ucontext *ucontext)
{
	kref_put(&ucontext->refcnt, dmabuf_ucontext_release);
}

/*
 * Check if a virtual address range of the current process is completely
 * covered by a dmabuf. Return values:
 *
 * 0:         Success. Base, size, fd are set on return.
 * -EEXIST:   Partial overlap. Base, size, fd are set on return.
 * -ENOENT:   No overlap with any dmabuf. Base, set, fd are unchanged.
 * -EINVAL:   Invalid address or size. Base, set, fd are unchanged.
 */
int dmabuf_reg_query(u64 addr, u64 len, u64 *base, u64 *size, int *fd)
{
	struct mm_struct *mm = current->mm;
	struct dmabuf_ucontext *ucontext;
	struct dmabuf_entry *entry;
	struct rb_root *root;
	struct rb_node *node;
	int err = -ENOENT;

	if (((U64_MAX - len) < addr) || !len)
		return -EINVAL;

	ucontext = dmabuf_ucontext_get(mm);
	if (!ucontext)
		return -ENOENT;

	mutex_lock(&ucontext->mutex);
	root = &ucontext->range_table;
	node = root->rb_node;
	while (node) {
		entry = container_of(node, struct dmabuf_entry, node);
		if (addr >= entry->base &&
		    addr + len <= entry->base + entry->size) {
			err = 0;
			break;
		} else if (addr + len <= entry->base) {
			node = node->rb_left;
		} else if (addr >= entry->base + entry->size) {
			node = node->rb_right;
		} else {
			err = -EEXIST;
			break;
		}
	}

	if (node) {
		*base = entry->base;
		*size = entry->size;
		*fd = entry->fd;
	}
	mutex_unlock(&ucontext->mutex);
	dmabuf_ucontext_put(ucontext);
	return err;
}

/*
 * Add a dmabuf entry to the registry. Return values:
 *
 * 0:         Success. Either a new entry is added or a matching entry is
 *            found. A reference to the entry is taken.
 * -EINVAL:   The address range conflicts with an existing entry or overflows.
 * -ENOMEM:   Failed to allocate a new entry.
 */
static int dmabuf_reg_add(struct dmabuf_ucontext *ucontext, u64 base, u64 size, u32 fd)
{
	struct dmabuf_entry *entry;
	struct rb_root *root;
	struct rb_node **new, *parent = NULL;
	void *old;

	if (((U64_MAX - size) < base) || !size)
		return -EINVAL;

	mutex_lock(&ucontext->mutex);
	entry = xa_load(&ucontext->fd_table, fd);
	if (entry) {
		if (base != entry->base || size != entry->size) {
			mutex_unlock(&ucontext->mutex);
			return -EINVAL;
		}
		kref_get(&entry->refcnt);
		mutex_unlock(&ucontext->mutex);
		return 0;
	}

	root = &ucontext->range_table;
	new = &root->rb_node;
	while (*new) {
		parent = *new;
		entry = container_of((*new), struct dmabuf_entry, node);
		if (base + size <= entry->base) {
			new = &((*new)->rb_left);
		} else if (base >= entry->base + entry->size) {
			new = &((*new)->rb_right);
		} else {
			/* dmabuf should not overlap */
			mutex_unlock(&ucontext->mutex);
			return -EINVAL;
		}
	}

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		mutex_unlock(&ucontext->mutex);
		return -ENOMEM;
	}

	entry->base = base;
	entry->size = size;
	entry->fd = fd;
	entry->ucontext = ucontext;
	kref_init(&entry->refcnt);

	old = xa_store(&ucontext->fd_table, fd, entry, GFP_KERNEL);
	if (xa_is_err(old)) {
		kfree(entry);
		mutex_unlock(&ucontext->mutex);
		return xa_err(old);
	}

	rb_link_node(&entry->node, parent, new);
	rb_insert_color(&entry->node, root);
	mutex_unlock(&ucontext->mutex);
	return 0;
}

static void dmabuf_reg_release(struct kref *kref)
{
	struct dmabuf_entry *entry =
			container_of(kref, struct dmabuf_entry, refcnt);
	struct dmabuf_ucontext *ucontext = entry->ucontext;

	lockdep_assert_held(&ucontext->mutex);

	rb_erase(&entry->node, &ucontext->range_table);
	xa_erase(&ucontext->fd_table, entry->fd);
	kfree(entry);
}

/*
 * Remove a dmabuf entry from the registry using buffer address as the key.
 * Return values:
 *
 * 0:         Success. The reference counter to the entry is decreased and
 *            the entry is removed if the counter reaches 0.
 * -EINVAL:   The address doesn't match any entry.
 */
static int dmabuf_reg_remove_addr(struct dmabuf_ucontext *ucontext, u64 addr)
{
	struct dmabuf_entry *entry;
	struct rb_root *root;
	struct rb_node *node;

	mutex_lock(&ucontext->mutex);
	root = &ucontext->range_table;
	node = root->rb_node;
	while (node) {
		entry = container_of(node, struct dmabuf_entry, node);
		if (addr < entry->base)
			node = node->rb_left;
		else if (addr >= entry->base + entry->size)
			node = node->rb_right;
		else
			break;
	}

	if (!node) {
		mutex_unlock(&ucontext->mutex);
		return -EINVAL;
	}

	kref_put(&entry->refcnt, dmabuf_reg_release);
	mutex_unlock(&ucontext->mutex);
	return 0;
}

/*
 * Remove a dmabuf entry from the registry using dmabuf fd as the key.
 * Return values:
 *
 * 0:         Success. The reference counter to the entry is decreased and
 *            the entry is removed if the counter reaches 0.
 * -EINVAL:   The address doesn't match any entry.
 */
static int dmabuf_reg_remove_fd(struct dmabuf_ucontext *ucontext, u32 fd)
{
	struct dmabuf_entry *entry;

	mutex_lock(&ucontext->mutex);
	entry = xa_load(&ucontext->fd_table, fd);
	if (!entry) {
		mutex_unlock(&ucontext->mutex);
		return -EINVAL;
	}

	kref_put(&entry->refcnt, dmabuf_reg_release);
	mutex_unlock(&ucontext->mutex);
	return 0;
}

static int dmabuf_reg_open(struct inode *inode, struct file *fp)
{
	struct dmabuf_ucontext *ucontext;

	ucontext = dmabuf_ucontext_get_or_new();
	if (IS_ERR(ucontext))
		return PTR_ERR(ucontext);

	fp->private_data = ucontext;
	__module_get(THIS_MODULE);
	return 0;
}

static int dmabuf_reg_close(struct inode *inode, struct file *fp)
{
	struct dmabuf_ucontext *ucontext = fp->private_data;

	dmabuf_ucontext_put(ucontext);
	fp->private_data = NULL;
	module_put(THIS_MODULE);
	return 0;
}

static long dmabuf_reg_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	struct dmabuf_reg_param param;
	struct dmabuf_ucontext *ucontext = fp->private_data;
	int ret;

	if (cmd != DMABUF_REG_IOCTL)
		return -EINVAL;

	if (copy_from_user(&param, u64_to_user_ptr((u64)arg), sizeof(param)))
		return -EFAULT;

	switch (param.op) {
	case DMABUF_REG_ADD:
		return dmabuf_reg_add(ucontext, param.base, param.size, param.fd);

	case DMABUF_REG_REMOVE_ADDR:
		return dmabuf_reg_remove_addr(ucontext, param.base);

	case DMABUF_REG_REMOVE_FD:
		return dmabuf_reg_remove_fd(ucontext, param.fd);

	case DMABUF_REG_QUERY:
		ret = dmabuf_reg_query(param.base, param.size,
				       &param.base, &param.size, &param.fd);
		if (ret == -ENOENT || ret == -EINVAL)
			return ret;
		return (copy_to_user(u64_to_user_ptr((u64)arg), &param,
				     sizeof(param))) ? -EFAULT : ret;

	default:
		return -EINVAL;
	}
}

static const struct file_operations dmabuf_reg_fops = {
	.owner = THIS_MODULE,
	.open = dmabuf_reg_open,
	.release = dmabuf_reg_close,
	.unlocked_ioctl = dmabuf_reg_ioctl,
};

static struct dmabuf_reg_device {
	dev_t devt;
	struct cdev cdev;
	struct device *device;
	struct class *class;
} dmabuf_reg_dev;

int dmabuf_reg_init(void)
{
	struct device *device;
	struct class *class;
	struct cdev *cdev;
	dev_t devt;
	int err;

	/* allocate device number */
	err = alloc_chrdev_region(&devt, 0, 1, DEV_NAME);
	if (err)
		return err;

	/* class is needed for device_create() */
	class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(class)) {
		err = PTR_ERR(class);
		goto err_unregister_cdev_region;
	}

	/* create the device node under /dev */
	device = device_create(class, NULL, devt, NULL, DEV_NAME);
	if (IS_ERR(device)) {
		err = PTR_ERR(device);
		goto err_destroy_class;
	}

	/* create the character device for ioctl */
	cdev = &dmabuf_reg_dev.cdev;
	cdev_init(cdev, &dmabuf_reg_fops);
	err = cdev_add(cdev, devt, 1);
	if (err)
		goto err_destroy_device;

	dmabuf_reg_dev.device = device;
	dmabuf_reg_dev.class = class;
	dmabuf_reg_dev.devt = devt;
	return 0;

err_destroy_device:
	device_destroy(class, devt);

err_destroy_class:
	class_destroy(class);

err_unregister_cdev_region:
	unregister_chrdev_region(devt, 1);
	return err;
}

void dmabuf_reg_fini(void)
{
	cdev_del(&dmabuf_reg_dev.cdev);
	device_destroy(dmabuf_reg_dev.class, dmabuf_reg_dev.devt);
	class_destroy(dmabuf_reg_dev.class);
	unregister_chrdev_region(dmabuf_reg_dev.devt, 1);
}

