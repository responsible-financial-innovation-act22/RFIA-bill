// SPDX-License-Identifier: GPL-2.0-only
/*
 * VFIO core
 *
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * Derived from original vfio:
 * Copyright 2010 Cisco Systems, Inc.  All rights reserved.
 * Author: Tom Lyon, pugs@cisco.com
 */

#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/device.h>
#include <linux/file.h>
#include <linux/anon_inodes.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/iommu.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/vfio.h>
#include <linux/wait.h>
#include <linux/sched/signal.h>
#include "vfio.h"

#define DRIVER_VERSION	"0.3"
#define DRIVER_AUTHOR	"Alex Williamson <alex.williamson@redhat.com>"
#define DRIVER_DESC	"VFIO - User Level meta-driver"

static struct vfio {
	struct class			*class;
	struct list_head		iommu_drivers_list;
	struct mutex			iommu_drivers_lock;
	struct list_head		group_list;
	struct mutex			group_lock; /* locks group_list */
	struct ida			group_ida;
	dev_t				group_devt;
} vfio;

struct vfio_iommu_driver {
	const struct vfio_iommu_driver_ops	*ops;
	struct list_head			vfio_next;
};

struct vfio_container {
	struct kref			kref;
	struct list_head		group_list;
	struct rw_semaphore		group_lock;
	struct vfio_iommu_driver	*iommu_driver;
	void				*iommu_data;
	bool				noiommu;
};

struct vfio_group {
	struct device 			dev;
	struct cdev			cdev;
	refcount_t			users;
	unsigned int			container_users;
	struct iommu_group		*iommu_group;
	struct vfio_container		*container;
	struct list_head		device_list;
	struct mutex			device_lock;
	struct list_head		vfio_next;
	struct list_head		container_next;
	enum vfio_group_type		type;
	unsigned int			dev_counter;
	struct rw_semaphore		group_rwsem;
	struct kvm			*kvm;
	struct file			*opened_file;
	struct blocking_notifier_head	notifier;
};

#ifdef CONFIG_VFIO_NOIOMMU
static bool noiommu __read_mostly;
module_param_named(enable_unsafe_noiommu_mode,
		   noiommu, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(enable_unsafe_noiommu_mode, "Enable UNSAFE, no-IOMMU mode.  This mode provides no device isolation, no DMA translation, no host kernel protection, cannot be used for device assignment to virtual machines, requires RAWIO permissions, and will taint the kernel.  If you do not know what this is for, step away. (default: false)");
#endif

static DEFINE_XARRAY(vfio_device_set_xa);
static const struct file_operations vfio_group_fops;

int vfio_assign_device_set(struct vfio_device *device, void *set_id)
{
	unsigned long idx = (unsigned long)set_id;
	struct vfio_device_set *new_dev_set;
	struct vfio_device_set *dev_set;

	if (WARN_ON(!set_id))
		return -EINVAL;

	/*
	 * Atomically acquire a singleton object in the xarray for this set_id
	 */
	xa_lock(&vfio_device_set_xa);
	dev_set = xa_load(&vfio_device_set_xa, idx);
	if (dev_set)
		goto found_get_ref;
	xa_unlock(&vfio_device_set_xa);

	new_dev_set = kzalloc(sizeof(*new_dev_set), GFP_KERNEL);
	if (!new_dev_set)
		return -ENOMEM;
	mutex_init(&new_dev_set->lock);
	INIT_LIST_HEAD(&new_dev_set->device_list);
	new_dev_set->set_id = set_id;

	xa_lock(&vfio_device_set_xa);
	dev_set = __xa_cmpxchg(&vfio_device_set_xa, idx, NULL, new_dev_set,
			       GFP_KERNEL);
	if (!dev_set) {
		dev_set = new_dev_set;
		goto found_get_ref;
	}

	kfree(new_dev_set);
	if (xa_is_err(dev_set)) {
		xa_unlock(&vfio_device_set_xa);
		return xa_err(dev_set);
	}

found_get_ref:
	dev_set->device_count++;
	xa_unlock(&vfio_device_set_xa);
	mutex_lock(&dev_set->lock);
	device->dev_set = dev_set;
	list_add_tail(&device->dev_set_list, &dev_set->device_list);
	mutex_unlock(&dev_set->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(vfio_assign_device_set);

static void vfio_release_device_set(struct vfio_device *device)
{
	struct vfio_device_set *dev_set = device->dev_set;

	if (!dev_set)
		return;

	mutex_lock(&dev_set->lock);
	list_del(&device->dev_set_list);
	mutex_unlock(&dev_set->lock);

	xa_lock(&vfio_device_set_xa);
	if (!--dev_set->device_count) {
		__xa_erase(&vfio_device_set_xa,
			   (unsigned long)dev_set->set_id);
		mutex_destroy(&dev_set->lock);
		kfree(dev_set);
	}
	xa_unlock(&vfio_device_set_xa);
}

#ifdef CONFIG_VFIO_NOIOMMU
static void *vfio_noiommu_open(unsigned long arg)
{
	if (arg != VFIO_NOIOMMU_IOMMU)
		return ERR_PTR(-EINVAL);
	if (!capable(CAP_SYS_RAWIO))
		return ERR_PTR(-EPERM);

	return NULL;
}

static void vfio_noiommu_release(void *iommu_data)
{
}

static long vfio_noiommu_ioctl(void *iommu_data,
			       unsigned int cmd, unsigned long arg)
{
	if (cmd == VFIO_CHECK_EXTENSION)
		return noiommu && (arg == VFIO_NOIOMMU_IOMMU) ? 1 : 0;

	return -ENOTTY;
}

static int vfio_noiommu_attach_group(void *iommu_data,
		struct iommu_group *iommu_group, enum vfio_group_type type)
{
	return 0;
}

static void vfio_noiommu_detach_group(void *iommu_data,
				      struct iommu_group *iommu_group)
{
}

static const struct vfio_iommu_driver_ops vfio_noiommu_ops = {
	.name = "vfio-noiommu",
	.owner = THIS_MODULE,
	.open = vfio_noiommu_open,
	.release = vfio_noiommu_release,
	.ioctl = vfio_noiommu_ioctl,
	.attach_group = vfio_noiommu_attach_group,
	.detach_group = vfio_noiommu_detach_group,
};

/*
 * Only noiommu containers can use vfio-noiommu and noiommu containers can only
 * use vfio-noiommu.
 */
static inline bool vfio_iommu_driver_allowed(struct vfio_container *container,
		const struct vfio_iommu_driver *driver)
{
	return container->noiommu == (driver->ops == &vfio_noiommu_ops);
}
#else
static inline bool vfio_iommu_driver_allowed(struct vfio_container *container,
		const struct vfio_iommu_driver *driver)
{
	return true;
}
#endif /* CONFIG_VFIO_NOIOMMU */

/*
 * IOMMU driver registration
 */
int vfio_register_iommu_driver(const struct vfio_iommu_driver_ops *ops)
{
	struct vfio_iommu_driver *driver, *tmp;

	driver = kzalloc(sizeof(*driver), GFP_KERNEL);
	if (!driver)
		return -ENOMEM;

	driver->ops = ops;

	mutex_lock(&vfio.iommu_drivers_lock);

	/* Check for duplicates */
	list_for_each_entry(tmp, &vfio.iommu_drivers_list, vfio_next) {
		if (tmp->ops == ops) {
			mutex_unlock(&vfio.iommu_drivers_lock);
			kfree(driver);
			return -EINVAL;
		}
	}

	list_add(&driver->vfio_next, &vfio.iommu_drivers_list);

	mutex_unlock(&vfio.iommu_drivers_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(vfio_register_iommu_driver);

void vfio_unregister_iommu_driver(const struct vfio_iommu_driver_ops *ops)
{
	struct vfio_iommu_driver *driver;

	mutex_lock(&vfio.iommu_drivers_lock);
	list_for_each_entry(driver, &vfio.iommu_drivers_list, vfio_next) {
		if (driver->ops == ops) {
			list_del(&driver->vfio_next);
			mutex_unlock(&vfio.iommu_drivers_lock);
			kfree(driver);
			return;
		}
	}
	mutex_unlock(&vfio.iommu_drivers_lock);
}
EXPORT_SYMBOL_GPL(vfio_unregister_iommu_driver);

static void vfio_group_get(struct vfio_group *group);

/*
 * Container objects - containers are created when /dev/vfio/vfio is
 * opened, but their lifecycle extends until the last user is done, so
 * it's freed via kref.  Must support container/group/device being
 * closed in any order.
 */
static void vfio_container_get(struct vfio_container *container)
{
	kref_get(&container->kref);
}

static void vfio_container_release(struct kref *kref)
{
	struct vfio_container *container;
	container = container_of(kref, struct vfio_container, kref);

	kfree(container);
}

static void vfio_container_put(struct vfio_container *container)
{
	kref_put(&container->kref, vfio_container_release);
}

/*
 * Group objects - create, release, get, put, search
 */
static struct vfio_group *
__vfio_group_get_from_iommu(struct iommu_group *iommu_group)
{
	struct vfio_group *group;

	list_for_each_entry(group, &vfio.group_list, vfio_next) {
		if (group->iommu_group == iommu_group) {
			vfio_group_get(group);
			return group;
		}
	}
	return NULL;
}

static struct vfio_group *
vfio_group_get_from_iommu(struct iommu_group *iommu_group)
{
	struct vfio_group *group;

	mutex_lock(&vfio.group_lock);
	group = __vfio_group_get_from_iommu(iommu_group);
	mutex_unlock(&vfio.group_lock);
	return group;
}

static void vfio_group_release(struct device *dev)
{
	struct vfio_group *group = container_of(dev, struct vfio_group, dev);

	mutex_destroy(&group->device_lock);
	iommu_group_put(group->iommu_group);
	ida_free(&vfio.group_ida, MINOR(group->dev.devt));
	kfree(group);
}

static struct vfio_group *vfio_group_alloc(struct iommu_group *iommu_group,
					   enum vfio_group_type type)
{
	struct vfio_group *group;
	int minor;

	group = kzalloc(sizeof(*group), GFP_KERNEL);
	if (!group)
		return ERR_PTR(-ENOMEM);

	minor = ida_alloc_max(&vfio.group_ida, MINORMASK, GFP_KERNEL);
	if (minor < 0) {
		kfree(group);
		return ERR_PTR(minor);
	}

	device_initialize(&group->dev);
	group->dev.devt = MKDEV(MAJOR(vfio.group_devt), minor);
	group->dev.class = vfio.class;
	group->dev.release = vfio_group_release;
	cdev_init(&group->cdev, &vfio_group_fops);
	group->cdev.owner = THIS_MODULE;

	refcount_set(&group->users, 1);
	init_rwsem(&group->group_rwsem);
	INIT_LIST_HEAD(&group->device_list);
	mutex_init(&group->device_lock);
	group->iommu_group = iommu_group;
	/* put in vfio_group_release() */
	iommu_group_ref_get(iommu_group);
	group->type = type;
	BLOCKING_INIT_NOTIFIER_HEAD(&group->notifier);

	return group;
}

static struct vfio_group *vfio_create_group(struct iommu_group *iommu_group,
		enum vfio_group_type type)
{
	struct vfio_group *group;
	struct vfio_group *ret;
	int err;

	group = vfio_group_alloc(iommu_group, type);
	if (IS_ERR(group))
		return group;

	err = dev_set_name(&group->dev, "%s%d",
			   group->type == VFIO_NO_IOMMU ? "noiommu-" : "",
			   iommu_group_id(iommu_group));
	if (err) {
		ret = ERR_PTR(err);
		goto err_put;
	}

	mutex_lock(&vfio.group_lock);

	/* Did we race creating this group? */
	ret = __vfio_group_get_from_iommu(iommu_group);
	if (ret)
		goto err_unlock;

	err = cdev_device_add(&group->cdev, &group->dev);
	if (err) {
		ret = ERR_PTR(err);
		goto err_unlock;
	}

	list_add(&group->vfio_next, &vfio.group_list);

	mutex_unlock(&vfio.group_lock);
	return group;

err_unlock:
	mutex_unlock(&vfio.group_lock);
err_put:
	put_device(&group->dev);
	return ret;
}

static void vfio_group_put(struct vfio_group *group)
{
	if (!refcount_dec_and_mutex_lock(&group->users, &vfio.group_lock))
		return;

	/*
	 * These data structures all have paired operations that can only be
	 * undone when the caller holds a live reference on the group. Since all
	 * pairs must be undone these WARN_ON's indicate some caller did not
	 * properly hold the group reference.
	 */
	WARN_ON(!list_empty(&group->device_list));
	WARN_ON(group->container || group->container_users);
	WARN_ON(group->notifier.head);

	list_del(&group->vfio_next);
	cdev_device_del(&group->cdev, &group->dev);
	mutex_unlock(&vfio.group_lock);

	put_device(&group->dev);
}

static void vfio_group_get(struct vfio_group *group)
{
	refcount_inc(&group->users);
}

/*
 * Device objects - create, release, get, put, search
 */
/* Device reference always implies a group reference */
static void vfio_device_put(struct vfio_device *device)
{
	if (refcount_dec_and_test(&device->refcount))
		complete(&device->comp);
}

static bool vfio_device_try_get(struct vfio_device *device)
{
	return refcount_inc_not_zero(&device->refcount);
}

static struct vfio_device *vfio_group_get_device(struct vfio_group *group,
						 struct device *dev)
{
	struct vfio_device *device;

	mutex_lock(&group->device_lock);
	list_for_each_entry(device, &group->device_list, group_next) {
		if (device->dev == dev && vfio_device_try_get(device)) {
			mutex_unlock(&group->device_lock);
			return device;
		}
	}
	mutex_unlock(&group->device_lock);
	return NULL;
}

/*
 * VFIO driver API
 */
void vfio_init_group_dev(struct vfio_device *device, struct device *dev,
			 const struct vfio_device_ops *ops)
{
	init_completion(&device->comp);
	device->dev = dev;
	device->ops = ops;
}
EXPORT_SYMBOL_GPL(vfio_init_group_dev);

void vfio_uninit_group_dev(struct vfio_device *device)
{
	vfio_release_device_set(device);
}
EXPORT_SYMBOL_GPL(vfio_uninit_group_dev);

static struct vfio_group *vfio_noiommu_group_alloc(struct device *dev,
		enum vfio_group_type type)
{
	struct iommu_group *iommu_group;
	struct vfio_group *group;
	int ret;

	iommu_group = iommu_group_alloc();
	if (IS_ERR(iommu_group))
		return ERR_CAST(iommu_group);

	iommu_group_set_name(iommu_group, "vfio-noiommu");
	ret = iommu_group_add_device(iommu_group, dev);
	if (ret)
		goto out_put_group;

	group = vfio_create_group(iommu_group, type);
	if (IS_ERR(group)) {
		ret = PTR_ERR(group);
		goto out_remove_device;
	}
	iommu_group_put(iommu_group);
	return group;

out_remove_device:
	iommu_group_remove_device(dev);
out_put_group:
	iommu_group_put(iommu_group);
	return ERR_PTR(ret);
}

static struct vfio_group *vfio_group_find_or_alloc(struct device *dev)
{
	struct iommu_group *iommu_group;
	struct vfio_group *group;

	iommu_group = iommu_group_get(dev);
#ifdef CONFIG_VFIO_NOIOMMU
	if (!iommu_group && noiommu) {
		/*
		 * With noiommu enabled, create an IOMMU group for devices that
		 * don't already have one, implying no IOMMU hardware/driver
		 * exists.  Taint the kernel because we're about to give a DMA
		 * capable device to a user without IOMMU protection.
		 */
		group = vfio_noiommu_group_alloc(dev, VFIO_NO_IOMMU);
		if (!IS_ERR(group)) {
			add_taint(TAINT_USER, LOCKDEP_STILL_OK);
			dev_warn(dev, "Adding kernel taint for vfio-noiommu group on device\n");
		}
		return group;
	}
#endif
	if (!iommu_group)
		return ERR_PTR(-EINVAL);

	group = vfio_group_get_from_iommu(iommu_group);
	if (!group)
		group = vfio_create_group(iommu_group, VFIO_IOMMU);

	/* The vfio_group holds a reference to the iommu_group */
	iommu_group_put(iommu_group);
	return group;
}

static int __vfio_register_dev(struct vfio_device *device,
		struct vfio_group *group)
{
	struct vfio_device *existing_device;

	if (IS_ERR(group))
		return PTR_ERR(group);

	/*
	 * If the driver doesn't specify a set then the device is added to a
	 * singleton set just for itself.
	 */
	if (!device->dev_set)
		vfio_assign_device_set(device, device);

	existing_device = vfio_group_get_device(group, device->dev);
	if (existing_device) {
		dev_WARN(device->dev, "Device already exists on group %d\n",
			 iommu_group_id(group->iommu_group));
		vfio_device_put(existing_device);
		if (group->type == VFIO_NO_IOMMU ||
		    group->type == VFIO_EMULATED_IOMMU)
			iommu_group_remove_device(device->dev);
		vfio_group_put(group);
		return -EBUSY;
	}

	/* Our reference on group is moved to the device */
	device->group = group;

	/* Refcounting can't start until the driver calls register */
	refcount_set(&device->refcount, 1);

	mutex_lock(&group->device_lock);
	list_add(&device->group_next, &group->device_list);
	group->dev_counter++;
	mutex_unlock(&group->device_lock);

	return 0;
}

int vfio_register_group_dev(struct vfio_device *device)
{
	/*
	 * VFIO always sets IOMMU_CACHE because we offer no way for userspace to
	 * restore cache coherency.
	 */
	if (!iommu_capable(device->dev->bus, IOMMU_CAP_CACHE_COHERENCY))
		return -EINVAL;

	return __vfio_register_dev(device,
		vfio_group_find_or_alloc(device->dev));
}
EXPORT_SYMBOL_GPL(vfio_register_group_dev);

/*
 * Register a virtual device without IOMMU backing.  The user of this
 * device must not be able to directly trigger unmediated DMA.
 */
int vfio_register_emulated_iommu_dev(struct vfio_device *device)
{
	return __vfio_register_dev(device,
		vfio_noiommu_group_alloc(device->dev, VFIO_EMULATED_IOMMU));
}
EXPORT_SYMBOL_GPL(vfio_register_emulated_iommu_dev);

static struct vfio_device *vfio_device_get_from_name(struct vfio_group *group,
						     char *buf)
{
	struct vfio_device *it, *device = ERR_PTR(-ENODEV);

	mutex_lock(&group->device_lock);
	list_for_each_entry(it, &group->device_list, group_next) {
		int ret;

		if (it->ops->match) {
			ret = it->ops->match(it, buf);
			if (ret < 0) {
				device = ERR_PTR(ret);
				break;
			}
		} else {
			ret = !strcmp(dev_name(it->dev), buf);
		}

		if (ret && vfio_device_try_get(it)) {
			device = it;
			break;
		}
	}
	mutex_unlock(&group->device_lock);

	return device;
}

/*
 * Decrement the device reference count and wait for the device to be
 * removed.  Open file descriptors for the device... */
void vfio_unregister_group_dev(struct vfio_device *device)
{
	struct vfio_group *group = device->group;
	unsigned int i = 0;
	bool interrupted = false;
	long rc;

	vfio_device_put(device);
	rc = try_wait_for_completion(&device->comp);
	while (rc <= 0) {
		if (device->ops->request)
			device->ops->request(device, i++);

		if (interrupted) {
			rc = wait_for_completion_timeout(&device->comp,
							 HZ * 10);
		} else {
			rc = wait_for_completion_interruptible_timeout(
				&device->comp, HZ * 10);
			if (rc < 0) {
				interrupted = true;
				dev_warn(device->dev,
					 "Device is currently in use, task"
					 " \"%s\" (%d) "
					 "blocked until device is released",
					 current->comm, task_pid_nr(current));
			}
		}
	}

	mutex_lock(&group->device_lock);
	list_del(&device->group_next);
	group->dev_counter--;
	mutex_unlock(&group->device_lock);

	if (group->type == VFIO_NO_IOMMU || group->type == VFIO_EMULATED_IOMMU)
		iommu_group_remove_device(device->dev);

	/* Matches the get in vfio_register_group_dev() */
	vfio_group_put(group);
}
EXPORT_SYMBOL_GPL(vfio_unregister_group_dev);

/*
 * VFIO base fd, /dev/vfio/vfio
 */
static long vfio_ioctl_check_extension(struct vfio_container *container,
				       unsigned long arg)
{
	struct vfio_iommu_driver *driver;
	long ret = 0;

	down_read(&container->group_lock);

	driver = container->iommu_driver;

	switch (arg) {
		/* No base extensions yet */
	default:
		/*
		 * If no driver is set, poll all registered drivers for
		 * extensions and return the first positive result.  If
		 * a driver is already set, further queries will be passed
		 * only to that driver.
		 */
		if (!driver) {
			mutex_lock(&vfio.iommu_drivers_lock);
			list_for_each_entry(driver, &vfio.iommu_drivers_list,
					    vfio_next) {

				if (!list_empty(&container->group_list) &&
				    !vfio_iommu_driver_allowed(container,
							       driver))
					continue;
				if (!try_module_get(driver->ops->owner))
					continue;

				ret = driver->ops->ioctl(NULL,
							 VFIO_CHECK_EXTENSION,
							 arg);
				module_put(driver->ops->owner);
				if (ret > 0)
					break;
			}
			mutex_unlock(&vfio.iommu_drivers_lock);
		} else
			ret = driver->ops->ioctl(container->iommu_data,
						 VFIO_CHECK_EXTENSION, arg);
	}

	up_read(&container->group_lock);

	return ret;
}

/* hold write lock on container->group_lock */
static int __vfio_container_attach_groups(struct vfio_container *container,
					  struct vfio_iommu_driver *driver,
					  void *data)
{
	struct vfio_group *group;
	int ret = -ENODEV;

	list_for_each_entry(group, &container->group_list, container_next) {
		ret = driver->ops->attach_group(data, group->iommu_group,
						group->type);
		if (ret)
			goto unwind;
	}

	return ret;

unwind:
	list_for_each_entry_continue_reverse(group, &container->group_list,
					     container_next) {
		driver->ops->detach_group(data, group->iommu_group);
	}

	return ret;
}

static long vfio_ioctl_set_iommu(struct vfio_container *container,
				 unsigned long arg)
{
	struct vfio_iommu_driver *driver;
	long ret = -ENODEV;

	down_write(&container->group_lock);

	/*
	 * The container is designed to be an unprivileged interface while
	 * the group can be assigned to specific users.  Therefore, only by
	 * adding a group to a container does the user get the privilege of
	 * enabling the iommu, which may allocate finite resources.  There
	 * is no unset_iommu, but by removing all the groups from a container,
	 * the container is deprivileged and returns to an unset state.
	 */
	if (list_empty(&container->group_list) || container->iommu_driver) {
		up_write(&container->group_lock);
		return -EINVAL;
	}

	mutex_lock(&vfio.iommu_drivers_lock);
	list_for_each_entry(driver, &vfio.iommu_drivers_list, vfio_next) {
		void *data;

		if (!vfio_iommu_driver_allowed(container, driver))
			continue;
		if (!try_module_get(driver->ops->owner))
			continue;

		/*
		 * The arg magic for SET_IOMMU is the same as CHECK_EXTENSION,
		 * so test which iommu driver reported support for this
		 * extension and call open on them.  We also pass them the
		 * magic, allowing a single driver to support multiple
		 * interfaces if they'd like.
		 */
		if (driver->ops->ioctl(NULL, VFIO_CHECK_EXTENSION, arg) <= 0) {
			module_put(driver->ops->owner);
			continue;
		}

		data = driver->ops->open(arg);
		if (IS_ERR(data)) {
			ret = PTR_ERR(data);
			module_put(driver->ops->owner);
			continue;
		}

		ret = __vfio_container_attach_groups(container, driver, data);
		if (ret) {
			driver->ops->release(data);
			module_put(driver->ops->owner);
			continue;
		}

		container->iommu_driver = driver;
		container->iommu_data = data;
		break;
	}

	mutex_unlock(&vfio.iommu_drivers_lock);
	up_write(&container->group_lock);

	return ret;
}

static long vfio_fops_unl_ioctl(struct file *filep,
				unsigned int cmd, unsigned long arg)
{
	struct vfio_container *container = filep->private_data;
	struct vfio_iommu_driver *driver;
	void *data;
	long ret = -EINVAL;

	if (!container)
		return ret;

	switch (cmd) {
	case VFIO_GET_API_VERSION:
		ret = VFIO_API_VERSION;
		break;
	case VFIO_CHECK_EXTENSION:
		ret = vfio_ioctl_check_extension(container, arg);
		break;
	case VFIO_SET_IOMMU:
		ret = vfio_ioctl_set_iommu(container, arg);
		break;
	default:
		driver = container->iommu_driver;
		data = container->iommu_data;

		if (driver) /* passthrough all unrecognized ioctls */
			ret = driver->ops->ioctl(data, cmd, arg);
	}

	return ret;
}

static int vfio_fops_open(struct inode *inode, struct file *filep)
{
	struct vfio_container *container;

	container = kzalloc(sizeof(*container), GFP_KERNEL);
	if (!container)
		return -ENOMEM;

	INIT_LIST_HEAD(&container->group_list);
	init_rwsem(&container->group_lock);
	kref_init(&container->kref);

	filep->private_data = container;

	return 0;
}

static int vfio_fops_release(struct inode *inode, struct file *filep)
{
	struct vfio_container *container = filep->private_data;
	struct vfio_iommu_driver *driver = container->iommu_driver;

	if (driver && driver->ops->notify)
		driver->ops->notify(container->iommu_data,
				    VFIO_IOMMU_CONTAINER_CLOSE);

	filep->private_data = NULL;

	vfio_container_put(container);

	return 0;
}

static const struct file_operations vfio_fops = {
	.owner		= THIS_MODULE,
	.open		= vfio_fops_open,
	.release	= vfio_fops_release,
	.unlocked_ioctl	= vfio_fops_unl_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

/*
 * VFIO Group fd, /dev/vfio/$GROUP
 */
static void __vfio_group_unset_container(struct vfio_group *group)
{
	struct vfio_container *container = group->container;
	struct vfio_iommu_driver *driver;

	lockdep_assert_held_write(&group->group_rwsem);

	down_write(&container->group_lock);

	driver = container->iommu_driver;
	if (driver)
		driver->ops->detach_group(container->iommu_data,
					  group->iommu_group);

	if (group->type == VFIO_IOMMU)
		iommu_group_release_dma_owner(group->iommu_group);

	group->container = NULL;
	group->container_users = 0;
	list_del(&group->container_next);

	/* Detaching the last group deprivileges a container, remove iommu */
	if (driver && list_empty(&container->group_list)) {
		driver->ops->release(container->iommu_data);
		module_put(driver->ops->owner);
		container->iommu_driver = NULL;
		container->iommu_data = NULL;
	}

	up_write(&container->group_lock);

	vfio_container_put(container);
}

/*
 * VFIO_GROUP_UNSET_CONTAINER should fail if there are other users or
 * if there was no container to unset.  Since the ioctl is called on
 * the group, we know that still exists, therefore the only valid
 * transition here is 1->0.
 */
static int vfio_group_unset_container(struct vfio_group *group)
{
	lockdep_assert_held_write(&group->group_rwsem);

	if (!group->container)
		return -EINVAL;
	if (group->container_users != 1)
		return -EBUSY;
	__vfio_group_unset_container(group);
	return 0;
}

static int vfio_group_set_container(struct vfio_group *group, int container_fd)
{
	struct fd f;
	struct vfio_container *container;
	struct vfio_iommu_driver *driver;
	int ret = 0;

	lockdep_assert_held_write(&group->group_rwsem);

	if (group->container || WARN_ON(group->container_users))
		return -EINVAL;

	if (group->type == VFIO_NO_IOMMU && !capable(CAP_SYS_RAWIO))
		return -EPERM;

	f = fdget(container_fd);
	if (!f.file)
		return -EBADF;

	/* Sanity check, is this really our fd? */
	if (f.file->f_op != &vfio_fops) {
		fdput(f);
		return -EINVAL;
	}

	container = f.file->private_data;
	WARN_ON(!container); /* fget ensures we don't race vfio_release */

	down_write(&container->group_lock);

	/* Real groups and fake groups cannot mix */
	if (!list_empty(&container->group_list) &&
	    container->noiommu != (group->type == VFIO_NO_IOMMU)) {
		ret = -EPERM;
		goto unlock_out;
	}

	if (group->type == VFIO_IOMMU) {
		ret = iommu_group_claim_dma_owner(group->iommu_group, f.file);
		if (ret)
			goto unlock_out;
	}

	driver = container->iommu_driver;
	if (driver) {
		ret = driver->ops->attach_group(container->iommu_data,
						group->iommu_group,
						group->type);
		if (ret) {
			if (group->type == VFIO_IOMMU)
				iommu_group_release_dma_owner(
					group->iommu_group);
			goto unlock_out;
		}
	}

	group->container = container;
	group->container_users = 1;
	container->noiommu = (group->type == VFIO_NO_IOMMU);
	list_add(&group->container_next, &container->group_list);

	/* Get a reference on the container and mark a user within the group */
	vfio_container_get(container);

unlock_out:
	up_write(&container->group_lock);
	fdput(f);
	return ret;
}

static const struct file_operations vfio_device_fops;

/* true if the vfio_device has open_device() called but not close_device() */
static bool vfio_assert_device_open(struct vfio_device *device)
{
	return !WARN_ON_ONCE(!READ_ONCE(device->open_count));
}

static int vfio_device_assign_container(struct vfio_device *device)
{
	struct vfio_group *group = device->group;

	lockdep_assert_held_write(&group->group_rwsem);

	if (!group->container || !group->container->iommu_driver ||
	    WARN_ON(!group->container_users))
		return -EINVAL;

	if (group->type == VFIO_NO_IOMMU && !capable(CAP_SYS_RAWIO))
		return -EPERM;

	get_file(group->opened_file);
	group->container_users++;
	return 0;
}

static void vfio_device_unassign_container(struct vfio_device *device)
{
	down_write(&device->group->group_rwsem);
	WARN_ON(device->group->container_users <= 1);
	device->group->container_users--;
	fput(device->group->opened_file);
	up_write(&device->group->group_rwsem);
}

static struct file *vfio_device_open(struct vfio_device *device)
{
	struct file *filep;
	int ret;

	down_write(&device->group->group_rwsem);
	ret = vfio_device_assign_container(device);
	up_write(&device->group->group_rwsem);
	if (ret)
		return ERR_PTR(ret);

	if (!try_module_get(device->dev->driver->owner)) {
		ret = -ENODEV;
		goto err_unassign_container;
	}

	mutex_lock(&device->dev_set->lock);
	device->open_count++;
	if (device->open_count == 1) {
		/*
		 * Here we pass the KVM pointer with the group under the read
		 * lock.  If the device driver will use it, it must obtain a
		 * reference and release it during close_device.
		 */
		down_read(&device->group->group_rwsem);
		device->kvm = device->group->kvm;

		if (device->ops->open_device) {
			ret = device->ops->open_device(device);
			if (ret)
				goto err_undo_count;
		}
		up_read(&device->group->group_rwsem);
	}
	mutex_unlock(&device->dev_set->lock);

	/*
	 * We can't use anon_inode_getfd() because we need to modify
	 * the f_mode flags directly to allow more than just ioctls
	 */
	filep = anon_inode_getfile("[vfio-device]", &vfio_device_fops,
				   device, O_RDWR);
	if (IS_ERR(filep)) {
		ret = PTR_ERR(filep);
		goto err_close_device;
	}

	/*
	 * TODO: add an anon_inode interface to do this.
	 * Appears to be missing by lack of need rather than
	 * explicitly prevented.  Now there's need.
	 */
	filep->f_mode |= (FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE);

	if (device->group->type == VFIO_NO_IOMMU)
		dev_warn(device->dev, "vfio-noiommu device opened by user "
			 "(%s:%d)\n", current->comm, task_pid_nr(current));
	/*
	 * On success the ref of device is moved to the file and
	 * put in vfio_device_fops_release()
	 */
	return filep;

err_close_device:
	mutex_lock(&device->dev_set->lock);
	down_read(&device->group->group_rwsem);
	if (device->open_count == 1 && device->ops->close_device)
		device->ops->close_device(device);
err_undo_count:
	device->open_count--;
	if (device->open_count == 0 && device->kvm)
		device->kvm = NULL;
	up_read(&device->group->group_rwsem);
	mutex_unlock(&device->dev_set->lock);
	module_put(device->dev->driver->owner);
err_unassign_container:
	vfio_device_unassign_container(device);
	return ERR_PTR(ret);
}

static int vfio_group_get_device_fd(struct vfio_group *group, char *buf)
{
	struct vfio_device *device;
	struct file *filep;
	int fdno;
	int ret;

	device = vfio_device_get_from_name(group, buf);
	if (IS_ERR(device))
		return PTR_ERR(device);

	fdno = get_unused_fd_flags(O_CLOEXEC);
	if (fdno < 0) {
		ret = fdno;
		goto err_put_device;
	}

	filep = vfio_device_open(device);
	if (IS_ERR(filep)) {
		ret = PTR_ERR(filep);
		goto err_put_fdno;
	}

	fd_install(fdno, filep);
	return fdno;

err_put_fdno:
	put_unused_fd(fdno);
err_put_device:
	vfio_device_put(device);
	return ret;
}

static long vfio_group_fops_unl_ioctl(struct file *filep,
				      unsigned int cmd, unsigned long arg)
{
	struct vfio_group *group = filep->private_data;
	long ret = -ENOTTY;

	switch (cmd) {
	case VFIO_GROUP_GET_STATUS:
	{
		struct vfio_group_status status;
		unsigned long minsz;

		minsz = offsetofend(struct vfio_group_status, flags);

		if (copy_from_user(&status, (void __user *)arg, minsz))
			return -EFAULT;

		if (status.argsz < minsz)
			return -EINVAL;

		status.flags = 0;

		down_read(&group->group_rwsem);
		if (group->container)
			status.flags |= VFIO_GROUP_FLAGS_CONTAINER_SET |
					VFIO_GROUP_FLAGS_VIABLE;
		else if (!iommu_group_dma_owner_claimed(group->iommu_group))
			status.flags |= VFIO_GROUP_FLAGS_VIABLE;
		up_read(&group->group_rwsem);

		if (copy_to_user((void __user *)arg, &status, minsz))
			return -EFAULT;

		ret = 0;
		break;
	}
	case VFIO_GROUP_SET_CONTAINER:
	{
		int fd;

		if (get_user(fd, (int __user *)arg))
			return -EFAULT;

		if (fd < 0)
			return -EINVAL;

		down_write(&group->group_rwsem);
		ret = vfio_group_set_container(group, fd);
		up_write(&group->group_rwsem);
		break;
	}
	case VFIO_GROUP_UNSET_CONTAINER:
		down_write(&group->group_rwsem);
		ret = vfio_group_unset_container(group);
		up_write(&group->group_rwsem);
		break;
	case VFIO_GROUP_GET_DEVICE_FD:
	{
		char *buf;

		buf = strndup_user((const char __user *)arg, PAGE_SIZE);
		if (IS_ERR(buf))
			return PTR_ERR(buf);

		ret = vfio_group_get_device_fd(group, buf);
		kfree(buf);
		break;
	}
	}

	return ret;
}

static int vfio_group_fops_open(struct inode *inode, struct file *filep)
{
	struct vfio_group *group =
		container_of(inode->i_cdev, struct vfio_group, cdev);
	int ret;

	down_write(&group->group_rwsem);

	/* users can be zero if this races with vfio_group_put() */
	if (!refcount_inc_not_zero(&group->users)) {
		ret = -ENODEV;
		goto err_unlock;
	}

	if (group->type == VFIO_NO_IOMMU && !capable(CAP_SYS_RAWIO)) {
		ret = -EPERM;
		goto err_put;
	}

	/*
	 * Do we need multiple instances of the group open?  Seems not.
	 */
	if (group->opened_file) {
		ret = -EBUSY;
		goto err_put;
	}
	group->opened_file = filep;
	filep->private_data = group;

	up_write(&group->group_rwsem);
	return 0;
err_put:
	vfio_group_put(group);
err_unlock:
	up_write(&group->group_rwsem);
	return ret;
}

static int vfio_group_fops_release(struct inode *inode, struct file *filep)
{
	struct vfio_group *group = filep->private_data;

	filep->private_data = NULL;

	down_write(&group->group_rwsem);
	/*
	 * Device FDs hold a group file reference, therefore the group release
	 * is only called when there are no open devices.
	 */
	WARN_ON(group->notifier.head);
	if (group->container) {
		WARN_ON(group->container_users != 1);
		__vfio_group_unset_container(group);
	}
	group->opened_file = NULL;
	up_write(&group->group_rwsem);

	vfio_group_put(group);

	return 0;
}

static const struct file_operations vfio_group_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= vfio_group_fops_unl_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.open		= vfio_group_fops_open,
	.release	= vfio_group_fops_release,
};

/*
 * VFIO Device fd
 */
static int vfio_device_fops_release(struct inode *inode, struct file *filep)
{
	struct vfio_device *device = filep->private_data;

	mutex_lock(&device->dev_set->lock);
	vfio_assert_device_open(device);
	down_read(&device->group->group_rwsem);
	if (device->open_count == 1 && device->ops->close_device)
		device->ops->close_device(device);
	up_read(&device->group->group_rwsem);
	device->open_count--;
	if (device->open_count == 0)
		device->kvm = NULL;
	mutex_unlock(&device->dev_set->lock);

	module_put(device->dev->driver->owner);

	vfio_device_unassign_container(device);

	vfio_device_put(device);

	return 0;
}

/*
 * vfio_mig_get_next_state - Compute the next step in the FSM
 * @cur_fsm - The current state the device is in
 * @new_fsm - The target state to reach
 * @next_fsm - Pointer to the next step to get to new_fsm
 *
 * Return 0 upon success, otherwise -errno
 * Upon success the next step in the state progression between cur_fsm and
 * new_fsm will be set in next_fsm.
 *
 * This breaks down requests for combination transitions into smaller steps and
 * returns the next step to get to new_fsm. The function may need to be called
 * multiple times before reaching new_fsm.
 *
 */
int vfio_mig_get_next_state(struct vfio_device *device,
			    enum vfio_device_mig_state cur_fsm,
			    enum vfio_device_mig_state new_fsm,
			    enum vfio_device_mig_state *next_fsm)
{
	enum { VFIO_DEVICE_NUM_STATES = VFIO_DEVICE_STATE_RUNNING_P2P + 1 };
	/*
	 * The coding in this table requires the driver to implement the
	 * following FSM arcs:
	 *         RESUMING -> STOP
	 *         STOP -> RESUMING
	 *         STOP -> STOP_COPY
	 *         STOP_COPY -> STOP
	 *
	 * If P2P is supported then the driver must also implement these FSM
	 * arcs:
	 *         RUNNING -> RUNNING_P2P
	 *         RUNNING_P2P -> RUNNING
	 *         RUNNING_P2P -> STOP
	 *         STOP -> RUNNING_P2P
	 * Without P2P the driver must implement:
	 *         RUNNING -> STOP
	 *         STOP -> RUNNING
	 *
	 * The coding will step through multiple states for some combination
	 * transitions; if all optional features are supported, this means the
	 * following ones:
	 *         RESUMING -> STOP -> RUNNING_P2P
	 *         RESUMING -> STOP -> RUNNING_P2P -> RUNNING
	 *         RESUMING -> STOP -> STOP_COPY
	 *         RUNNING -> RUNNING_P2P -> STOP
	 *         RUNNING -> RUNNING_P2P -> STOP -> RESUMING
	 *         RUNNING -> RUNNING_P2P -> STOP -> STOP_COPY
	 *         RUNNING_P2P -> STOP -> RESUMING
	 *         RUNNING_P2P -> STOP -> STOP_COPY
	 *         STOP -> RUNNING_P2P -> RUNNING
	 *         STOP_COPY -> STOP -> RESUMING
	 *         STOP_COPY -> STOP -> RUNNING_P2P
	 *         STOP_COPY -> STOP -> RUNNING_P2P -> RUNNING
	 */
	static const u8 vfio_from_fsm_table[VFIO_DEVICE_NUM_STATES][VFIO_DEVICE_NUM_STATES] = {
		[VFIO_DEVICE_STATE_STOP] = {
			[VFIO_DEVICE_STATE_STOP] = VFIO_DEVICE_STATE_STOP,
			[VFIO_DEVICE_STATE_RUNNING] = VFIO_DEVICE_STATE_RUNNING_P2P,
			[VFIO_DEVICE_STATE_STOP_COPY] = VFIO_DEVICE_STATE_STOP_COPY,
			[VFIO_DEVICE_STATE_RESUMING] = VFIO_DEVICE_STATE_RESUMING,
			[VFIO_DEVICE_STATE_RUNNING_P2P] = VFIO_DEVICE_STATE_RUNNING_P2P,
			[VFIO_DEVICE_STATE_ERROR] = VFIO_DEVICE_STATE_ERROR,
		},
		[VFIO_DEVICE_STATE_RUNNING] = {
			[VFIO_DEVICE_STATE_STOP] = VFIO_DEVICE_STATE_RUNNING_P2P,
			[VFIO_DEVICE_STATE_RUNNING] = VFIO_DEVICE_STATE_RUNNING,
			[VFIO_DEVICE_STATE_STOP_COPY] = VFIO_DEVICE_STATE_RUNNING_P2P,
			[VFIO_DEVICE_STATE_RESUMING] = VFIO_DEVICE_STATE_RUNNING_P2P,
			[VFIO_DEVICE_STATE_RUNNING_P2P] = VFIO_DEVICE_STATE_RUNNING_P2P,
			[VFIO_DEVICE_STATE_ERROR] = VFIO_DEVICE_STATE_ERROR,
		},
		[VFIO_DEVICE_STATE_STOP_COPY] = {
			[VFIO_DEVICE_STATE_STOP] = VFIO_DEVICE_STATE_STOP,
			[VFIO_DEVICE_STATE_RUNNING] = VFIO_DEVICE_STATE_STOP,
			[VFIO_DEVICE_STATE_STOP_COPY] = VFIO_DEVICE_STATE_STOP_COPY,
			[VFIO_DEVICE_STATE_RESUMING] = VFIO_DEVICE_STATE_STOP,
			[VFIO_DEVICE_STATE_RUNNING_P2P] = VFIO_DEVICE_STATE_STOP,
			[VFIO_DEVICE_STATE_ERROR] = VFIO_DEVICE_STATE_ERROR,
		},
		[VFIO_DEVICE_STATE_RESUMING] = {
			[VFIO_DEVICE_STATE_STOP] = VFIO_DEVICE_STATE_STOP,
			[VFIO_DEVICE_STATE_RUNNING] = VFIO_DEVICE_STATE_STOP,
			[VFIO_DEVICE_STATE_STOP_COPY] = VFIO_DEVICE_STATE_STOP,
			[VFIO_DEVICE_STATE_RESUMING] = VFIO_DEVICE_STATE_RESUMING,
			[VFIO_DEVICE_STATE_RUNNING_P2P] = VFIO_DEVICE_STATE_STOP,
			[VFIO_DEVICE_STATE_ERROR] = VFIO_DEVICE_STATE_ERROR,
		},
		[VFIO_DEVICE_STATE_RUNNING_P2P] = {
			[VFIO_DEVICE_STATE_STOP] = VFIO_DEVICE_STATE_STOP,
			[VFIO_DEVICE_STATE_RUNNING] = VFIO_DEVICE_STATE_RUNNING,
			[VFIO_DEVICE_STATE_STOP_COPY] = VFIO_DEVICE_STATE_STOP,
			[VFIO_DEVICE_STATE_RESUMING] = VFIO_DEVICE_STATE_STOP,
			[VFIO_DEVICE_STATE_RUNNING_P2P] = VFIO_DEVICE_STATE_RUNNING_P2P,
			[VFIO_DEVICE_STATE_ERROR] = VFIO_DEVICE_STATE_ERROR,
		},
		[VFIO_DEVICE_STATE_ERROR] = {
			[VFIO_DEVICE_STATE_STOP] = VFIO_DEVICE_STATE_ERROR,
			[VFIO_DEVICE_STATE_RUNNING] = VFIO_DEVICE_STATE_ERROR,
			[VFIO_DEVICE_STATE_STOP_COPY] = VFIO_DEVICE_STATE_ERROR,
			[VFIO_DEVICE_STATE_RESUMING] = VFIO_DEVICE_STATE_ERROR,
			[VFIO_DEVICE_STATE_RUNNING_P2P] = VFIO_DEVICE_STATE_ERROR,
			[VFIO_DEVICE_STATE_ERROR] = VFIO_DEVICE_STATE_ERROR,
		},
	};

	static const unsigned int state_flags_table[VFIO_DEVICE_NUM_STATES] = {
		[VFIO_DEVICE_STATE_STOP] = VFIO_MIGRATION_STOP_COPY,
		[VFIO_DEVICE_STATE_RUNNING] = VFIO_MIGRATION_STOP_COPY,
		[VFIO_DEVICE_STATE_STOP_COPY] = VFIO_MIGRATION_STOP_COPY,
		[VFIO_DEVICE_STATE_RESUMING] = VFIO_MIGRATION_STOP_COPY,
		[VFIO_DEVICE_STATE_RUNNING_P2P] =
			VFIO_MIGRATION_STOP_COPY | VFIO_MIGRATION_P2P,
		[VFIO_DEVICE_STATE_ERROR] = ~0U,
	};

	if (WARN_ON(cur_fsm >= ARRAY_SIZE(vfio_from_fsm_table) ||
		    (state_flags_table[cur_fsm] & device->migration_flags) !=
			state_flags_table[cur_fsm]))
		return -EINVAL;

	if (new_fsm >= ARRAY_SIZE(vfio_from_fsm_table) ||
	   (state_flags_table[new_fsm] & device->migration_flags) !=
			state_flags_table[new_fsm])
		return -EINVAL;

	/*
	 * Arcs touching optional and unsupported states are skipped over. The
	 * driver will instead see an arc from the original state to the next
	 * logical state, as per the above comment.
	 */
	*next_fsm = vfio_from_fsm_table[cur_fsm][new_fsm];
	while ((state_flags_table[*next_fsm] & device->migration_flags) !=
			state_flags_table[*next_fsm])
		*next_fsm = vfio_from_fsm_table[*next_fsm][new_fsm];

	return (*next_fsm != VFIO_DEVICE_STATE_ERROR) ? 0 : -EINVAL;
}
EXPORT_SYMBOL_GPL(vfio_mig_get_next_state);

/*
 * Convert the drivers's struct file into a FD number and return it to userspace
 */
static int vfio_ioct_mig_return_fd(struct file *filp, void __user *arg,
				   struct vfio_device_feature_mig_state *mig)
{
	int ret;
	int fd;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto out_fput;
	}

	mig->data_fd = fd;
	if (copy_to_user(arg, mig, sizeof(*mig))) {
		ret = -EFAULT;
		goto out_put_unused;
	}
	fd_install(fd, filp);
	return 0;

out_put_unused:
	put_unused_fd(fd);
out_fput:
	fput(filp);
	return ret;
}

static int
vfio_ioctl_device_feature_mig_device_state(struct vfio_device *device,
					   u32 flags, void __user *arg,
					   size_t argsz)
{
	size_t minsz =
		offsetofend(struct vfio_device_feature_mig_state, data_fd);
	struct vfio_device_feature_mig_state mig;
	struct file *filp = NULL;
	int ret;

	if (!device->ops->migration_set_state ||
	    !device->ops->migration_get_state)
		return -ENOTTY;

	ret = vfio_check_feature(flags, argsz,
				 VFIO_DEVICE_FEATURE_SET |
				 VFIO_DEVICE_FEATURE_GET,
				 sizeof(mig));
	if (ret != 1)
		return ret;

	if (copy_from_user(&mig, arg, minsz))
		return -EFAULT;

	if (flags & VFIO_DEVICE_FEATURE_GET) {
		enum vfio_device_mig_state curr_state;

		ret = device->ops->migration_get_state(device, &curr_state);
		if (ret)
			return ret;
		mig.device_state = curr_state;
		goto out_copy;
	}

	/* Handle the VFIO_DEVICE_FEATURE_SET */
	filp = device->ops->migration_set_state(device, mig.device_state);
	if (IS_ERR(filp) || !filp)
		goto out_copy;

	return vfio_ioct_mig_return_fd(filp, arg, &mig);
out_copy:
	mig.data_fd = -1;
	if (copy_to_user(arg, &mig, sizeof(mig)))
		return -EFAULT;
	if (IS_ERR(filp))
		return PTR_ERR(filp);
	return 0;
}

static int vfio_ioctl_device_feature_migration(struct vfio_device *device,
					       u32 flags, void __user *arg,
					       size_t argsz)
{
	struct vfio_device_feature_migration mig = {
		.flags = device->migration_flags,
	};
	int ret;

	if (!device->ops->migration_set_state ||
	    !device->ops->migration_get_state)
		return -ENOTTY;

	ret = vfio_check_feature(flags, argsz, VFIO_DEVICE_FEATURE_GET,
				 sizeof(mig));
	if (ret != 1)
		return ret;
	if (copy_to_user(arg, &mig, sizeof(mig)))
		return -EFAULT;
	return 0;
}

static int vfio_ioctl_device_feature(struct vfio_device *device,
				     struct vfio_device_feature __user *arg)
{
	size_t minsz = offsetofend(struct vfio_device_feature, flags);
	struct vfio_device_feature feature;

	if (copy_from_user(&feature, arg, minsz))
		return -EFAULT;

	if (feature.argsz < minsz)
		return -EINVAL;

	/* Check unknown flags */
	if (feature.flags &
	    ~(VFIO_DEVICE_FEATURE_MASK | VFIO_DEVICE_FEATURE_SET |
	      VFIO_DEVICE_FEATURE_GET | VFIO_DEVICE_FEATURE_PROBE))
		return -EINVAL;

	/* GET & SET are mutually exclusive except with PROBE */
	if (!(feature.flags & VFIO_DEVICE_FEATURE_PROBE) &&
	    (feature.flags & VFIO_DEVICE_FEATURE_SET) &&
	    (feature.flags & VFIO_DEVICE_FEATURE_GET))
		return -EINVAL;

	switch (feature.flags & VFIO_DEVICE_FEATURE_MASK) {
	case VFIO_DEVICE_FEATURE_MIGRATION:
		return vfio_ioctl_device_feature_migration(
			device, feature.flags, arg->data,
			feature.argsz - minsz);
	case VFIO_DEVICE_FEATURE_MIG_DEVICE_STATE:
		return vfio_ioctl_device_feature_mig_device_state(
			device, feature.flags, arg->data,
			feature.argsz - minsz);
	default:
		if (unlikely(!device->ops->device_feature))
			return -EINVAL;
		return device->ops->device_feature(device, feature.flags,
						   arg->data,
						   feature.argsz - minsz);
	}
}

static long vfio_device_fops_unl_ioctl(struct file *filep,
				       unsigned int cmd, unsigned long arg)
{
	struct vfio_device *device = filep->private_data;

	switch (cmd) {
	case VFIO_DEVICE_FEATURE:
		return vfio_ioctl_device_feature(device, (void __user *)arg);
	default:
		if (unlikely(!device->ops->ioctl))
			return -EINVAL;
		return device->ops->ioctl(device, cmd, arg);
	}
}

static ssize_t vfio_device_fops_read(struct file *filep, char __user *buf,
				     size_t count, loff_t *ppos)
{
	struct vfio_device *device = filep->private_data;

	if (unlikely(!device->ops->read))
		return -EINVAL;

	return device->ops->read(device, buf, count, ppos);
}

static ssize_t vfio_device_fops_write(struct file *filep,
				      const char __user *buf,
				      size_t count, loff_t *ppos)
{
	struct vfio_device *device = filep->private_data;

	if (unlikely(!device->ops->write))
		return -EINVAL;

	return device->ops->write(device, buf, count, ppos);
}

static int vfio_device_fops_mmap(struct file *filep, struct vm_area_struct *vma)
{
	struct vfio_device *device = filep->private_data;

	if (unlikely(!device->ops->mmap))
		return -EINVAL;

	return device->ops->mmap(device, vma);
}

static const struct file_operations vfio_device_fops = {
	.owner		= THIS_MODULE,
	.release	= vfio_device_fops_release,
	.read		= vfio_device_fops_read,
	.write		= vfio_device_fops_write,
	.unlocked_ioctl	= vfio_device_fops_unl_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.mmap		= vfio_device_fops_mmap,
};

/**
 * vfio_file_iommu_group - Return the struct iommu_group for the vfio group file
 * @file: VFIO group file
 *
 * The returned iommu_group is valid as long as a ref is held on the file.
 */
struct iommu_group *vfio_file_iommu_group(struct file *file)
{
	struct vfio_group *group = file->private_data;

	if (file->f_op != &vfio_group_fops)
		return NULL;
	return group->iommu_group;
}
EXPORT_SYMBOL_GPL(vfio_file_iommu_group);

/**
 * vfio_file_enforced_coherent - True if the DMA associated with the VFIO file
 *        is always CPU cache coherent
 * @file: VFIO group file
 *
 * Enforced coherency means that the IOMMU ignores things like the PCIe no-snoop
 * bit in DMA transactions. A return of false indicates that the user has
 * rights to access additional instructions such as wbinvd on x86.
 */
bool vfio_file_enforced_coherent(struct file *file)
{
	struct vfio_group *group = file->private_data;
	bool ret;

	if (file->f_op != &vfio_group_fops)
		return true;

	down_read(&group->group_rwsem);
	if (group->container) {
		ret = vfio_ioctl_check_extension(group->container,
						 VFIO_DMA_CC_IOMMU);
	} else {
		/*
		 * Since the coherency state is determined only once a container
		 * is attached the user must do so before they can prove they
		 * have permission.
		 */
		ret = true;
	}
	up_read(&group->group_rwsem);
	return ret;
}
EXPORT_SYMBOL_GPL(vfio_file_enforced_coherent);

/**
 * vfio_file_set_kvm - Link a kvm with VFIO drivers
 * @file: VFIO group file
 * @kvm: KVM to link
 *
 * When a VFIO device is first opened the KVM will be available in
 * device->kvm if one was associated with the group.
 */
void vfio_file_set_kvm(struct file *file, struct kvm *kvm)
{
	struct vfio_group *group = file->private_data;

	if (file->f_op != &vfio_group_fops)
		return;

	down_write(&group->group_rwsem);
	group->kvm = kvm;
	up_write(&group->group_rwsem);
}
EXPORT_SYMBOL_GPL(vfio_file_set_kvm);

/**
 * vfio_file_has_dev - True if the VFIO file is a handle for device
 * @file: VFIO file to check
 * @device: Device that must be part of the file
 *
 * Returns true if given file has permission to manipulate the given device.
 */
bool vfio_file_has_dev(struct file *file, struct vfio_device *device)
{
	struct vfio_group *group = file->private_data;

	if (file->f_op != &vfio_group_fops)
		return false;

	return group == device->group;
}
EXPORT_SYMBOL_GPL(vfio_file_has_dev);

/*
 * Sub-module support
 */
/*
 * Helper for managing a buffer of info chain capabilities, allocate or
 * reallocate a buffer with additional @size, filling in @id and @version
 * of the capability.  A pointer to the new capability is returned.
 *
 * NB. The chain is based at the head of the buffer, so new entries are
 * added to the tail, vfio_info_cap_shift() should be called to fixup the
 * next offsets prior to copying to the user buffer.
 */
struct vfio_info_cap_header *vfio_info_cap_add(struct vfio_info_cap *caps,
					       size_t size, u16 id, u16 version)
{
	void *buf;
	struct vfio_info_cap_header *header, *tmp;

	buf = krealloc(caps->buf, caps->size + size, GFP_KERNEL);
	if (!buf) {
		kfree(caps->buf);
		caps->size = 0;
		return ERR_PTR(-ENOMEM);
	}

	caps->buf = buf;
	header = buf + caps->size;

	/* Eventually copied to user buffer, zero */
	memset(header, 0, size);

	header->id = id;
	header->version = version;

	/* Add to the end of the capability chain */
	for (tmp = buf; tmp->next; tmp = buf + tmp->next)
		; /* nothing */

	tmp->next = caps->size;
	caps->size += size;

	return header;
}
EXPORT_SYMBOL_GPL(vfio_info_cap_add);

void vfio_info_cap_shift(struct vfio_info_cap *caps, size_t offset)
{
	struct vfio_info_cap_header *tmp;
	void *buf = (void *)caps->buf;

	for (tmp = buf; tmp->next; tmp = buf + tmp->next - offset)
		tmp->next += offset;
}
EXPORT_SYMBOL(vfio_info_cap_shift);

int vfio_info_add_capability(struct vfio_info_cap *caps,
			     struct vfio_info_cap_header *cap, size_t size)
{
	struct vfio_info_cap_header *header;

	header = vfio_info_cap_add(caps, size, cap->id, cap->version);
	if (IS_ERR(header))
		return PTR_ERR(header);

	memcpy(header + 1, cap + 1, size - sizeof(*header));

	return 0;
}
EXPORT_SYMBOL(vfio_info_add_capability);

int vfio_set_irqs_validate_and_prepare(struct vfio_irq_set *hdr, int num_irqs,
				       int max_irq_type, size_t *data_size)
{
	unsigned long minsz;
	size_t size;

	minsz = offsetofend(struct vfio_irq_set, count);

	if ((hdr->argsz < minsz) || (hdr->index >= max_irq_type) ||
	    (hdr->count >= (U32_MAX - hdr->start)) ||
	    (hdr->flags & ~(VFIO_IRQ_SET_DATA_TYPE_MASK |
				VFIO_IRQ_SET_ACTION_TYPE_MASK)))
		return -EINVAL;

	if (data_size)
		*data_size = 0;

	if (hdr->start >= num_irqs || hdr->start + hdr->count > num_irqs)
		return -EINVAL;

	switch (hdr->flags & VFIO_IRQ_SET_DATA_TYPE_MASK) {
	case VFIO_IRQ_SET_DATA_NONE:
		size = 0;
		break;
	case VFIO_IRQ_SET_DATA_BOOL:
		size = sizeof(uint8_t);
		break;
	case VFIO_IRQ_SET_DATA_EVENTFD:
		size = sizeof(int32_t);
		break;
	default:
		return -EINVAL;
	}

	if (size) {
		if (hdr->argsz - minsz < hdr->count * size)
			return -EINVAL;

		if (!data_size)
			return -EINVAL;

		*data_size = hdr->count * size;
	}

	return 0;
}
EXPORT_SYMBOL(vfio_set_irqs_validate_and_prepare);

/*
 * Pin a set of guest PFNs and return their associated host PFNs for local
 * domain only.
 * @device [in]  : device
 * @user_pfn [in]: array of user/guest PFNs to be pinned.
 * @npage [in]   : count of elements in user_pfn array.  This count should not
 *		   be greater VFIO_PIN_PAGES_MAX_ENTRIES.
 * @prot [in]    : protection flags
 * @phys_pfn[out]: array of host PFNs
 * Return error or number of pages pinned.
 */
int vfio_pin_pages(struct vfio_device *device, unsigned long *user_pfn,
		   int npage, int prot, unsigned long *phys_pfn)
{
	struct vfio_container *container;
	struct vfio_group *group = device->group;
	struct vfio_iommu_driver *driver;
	int ret;

	if (!user_pfn || !phys_pfn || !npage ||
	    !vfio_assert_device_open(device))
		return -EINVAL;

	if (npage > VFIO_PIN_PAGES_MAX_ENTRIES)
		return -E2BIG;

	if (group->dev_counter > 1)
		return -EINVAL;

	/* group->container cannot change while a vfio device is open */
	container = group->container;
	driver = container->iommu_driver;
	if (likely(driver && driver->ops->pin_pages))
		ret = driver->ops->pin_pages(container->iommu_data,
					     group->iommu_group, user_pfn,
					     npage, prot, phys_pfn);
	else
		ret = -ENOTTY;

	return ret;
}
EXPORT_SYMBOL(vfio_pin_pages);

/*
 * Unpin set of host PFNs for local domain only.
 * @device [in]  : device
 * @user_pfn [in]: array of user/guest PFNs to be unpinned. Number of user/guest
 *		   PFNs should not be greater than VFIO_PIN_PAGES_MAX_ENTRIES.
 * @npage [in]   : count of elements in user_pfn array.  This count should not
 *                 be greater than VFIO_PIN_PAGES_MAX_ENTRIES.
 * Return error or number of pages unpinned.
 */
int vfio_unpin_pages(struct vfio_device *device, unsigned long *user_pfn,
		     int npage)
{
	struct vfio_container *container;
	struct vfio_iommu_driver *driver;
	int ret;

	if (!user_pfn || !npage || !vfio_assert_device_open(device))
		return -EINVAL;

	if (npage > VFIO_PIN_PAGES_MAX_ENTRIES)
		return -E2BIG;

	/* group->container cannot change while a vfio device is open */
	container = device->group->container;
	driver = container->iommu_driver;
	if (likely(driver && driver->ops->unpin_pages))
		ret = driver->ops->unpin_pages(container->iommu_data, user_pfn,
					       npage);
	else
		ret = -ENOTTY;

	return ret;
}
EXPORT_SYMBOL(vfio_unpin_pages);

/*
 * This interface allows the CPUs to perform some sort of virtual DMA on
 * behalf of the device.
 *
 * CPUs read/write from/into a range of IOVAs pointing to user space memory
 * into/from a kernel buffer.
 *
 * As the read/write of user space memory is conducted via the CPUs and is
 * not a real device DMA, it is not necessary to pin the user space memory.
 *
 * @device [in]		: VFIO device
 * @user_iova [in]	: base IOVA of a user space buffer
 * @data [in]		: pointer to kernel buffer
 * @len [in]		: kernel buffer length
 * @write		: indicate read or write
 * Return error code on failure or 0 on success.
 */
int vfio_dma_rw(struct vfio_device *device, dma_addr_t user_iova, void *data,
		size_t len, bool write)
{
	struct vfio_container *container;
	struct vfio_iommu_driver *driver;
	int ret = 0;

	if (!data || len <= 0 || !vfio_assert_device_open(device))
		return -EINVAL;

	/* group->container cannot change while a vfio device is open */
	container = device->group->container;
	driver = container->iommu_driver;

	if (likely(driver && driver->ops->dma_rw))
		ret = driver->ops->dma_rw(container->iommu_data,
					  user_iova, data, len, write);
	else
		ret = -ENOTTY;
	return ret;
}
EXPORT_SYMBOL(vfio_dma_rw);

static int vfio_register_iommu_notifier(struct vfio_group *group,
					unsigned long *events,
					struct notifier_block *nb)
{
	struct vfio_container *container;
	struct vfio_iommu_driver *driver;
	int ret;

	lockdep_assert_held_read(&group->group_rwsem);

	container = group->container;
	driver = container->iommu_driver;
	if (likely(driver && driver->ops->register_notifier))
		ret = driver->ops->register_notifier(container->iommu_data,
						     events, nb);
	else
		ret = -ENOTTY;

	return ret;
}

static int vfio_unregister_iommu_notifier(struct vfio_group *group,
					  struct notifier_block *nb)
{
	struct vfio_container *container;
	struct vfio_iommu_driver *driver;
	int ret;

	lockdep_assert_held_read(&group->group_rwsem);

	container = group->container;
	driver = container->iommu_driver;
	if (likely(driver && driver->ops->unregister_notifier))
		ret = driver->ops->unregister_notifier(container->iommu_data,
						       nb);
	else
		ret = -ENOTTY;

	return ret;
}

int vfio_register_notifier(struct vfio_device *device,
			   enum vfio_notify_type type, unsigned long *events,
			   struct notifier_block *nb)
{
	struct vfio_group *group = device->group;
	int ret;

	if (!nb || !events || (*events == 0) ||
	    !vfio_assert_device_open(device))
		return -EINVAL;

	switch (type) {
	case VFIO_IOMMU_NOTIFY:
		ret = vfio_register_iommu_notifier(group, events, nb);
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}
EXPORT_SYMBOL(vfio_register_notifier);

int vfio_unregister_notifier(struct vfio_device *device,
			     enum vfio_notify_type type,
			     struct notifier_block *nb)
{
	struct vfio_group *group = device->group;
	int ret;

	if (!nb || !vfio_assert_device_open(device))
		return -EINVAL;

	switch (type) {
	case VFIO_IOMMU_NOTIFY:
		ret = vfio_unregister_iommu_notifier(group, nb);
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}
EXPORT_SYMBOL(vfio_unregister_notifier);

/*
 * Module/class support
 */
static char *vfio_devnode(struct device *dev, umode_t *mode)
{
	return kasprintf(GFP_KERNEL, "vfio/%s", dev_name(dev));
}

static struct miscdevice vfio_dev = {
	.minor = VFIO_MINOR,
	.name = "vfio",
	.fops = &vfio_fops,
	.nodename = "vfio/vfio",
	.mode = S_IRUGO | S_IWUGO,
};

static int __init vfio_init(void)
{
	int ret;

	ida_init(&vfio.group_ida);
	mutex_init(&vfio.group_lock);
	mutex_init(&vfio.iommu_drivers_lock);
	INIT_LIST_HEAD(&vfio.group_list);
	INIT_LIST_HEAD(&vfio.iommu_drivers_list);

	ret = misc_register(&vfio_dev);
	if (ret) {
		pr_err("vfio: misc device register failed\n");
		return ret;
	}

	/* /dev/vfio/$GROUP */
	vfio.class = class_create(THIS_MODULE, "vfio");
	if (IS_ERR(vfio.class)) {
		ret = PTR_ERR(vfio.class);
		goto err_class;
	}

	vfio.class->devnode = vfio_devnode;

	ret = alloc_chrdev_region(&vfio.group_devt, 0, MINORMASK + 1, "vfio");
	if (ret)
		goto err_alloc_chrdev;

	pr_info(DRIVER_DESC " version: " DRIVER_VERSION "\n");

#ifdef CONFIG_VFIO_NOIOMMU
	vfio_register_iommu_driver(&vfio_noiommu_ops);
#endif
	return 0;

err_alloc_chrdev:
	class_destroy(vfio.class);
	vfio.class = NULL;
err_class:
	misc_deregister(&vfio_dev);
	return ret;
}

static void __exit vfio_cleanup(void)
{
	WARN_ON(!list_empty(&vfio.group_list));

#ifdef CONFIG_VFIO_NOIOMMU
	vfio_unregister_iommu_driver(&vfio_noiommu_ops);
#endif
	ida_destroy(&vfio.group_ida);
	unregister_chrdev_region(vfio.group_devt, MINORMASK + 1);
	class_destroy(vfio.class);
	vfio.class = NULL;
	misc_deregister(&vfio_dev);
	xa_destroy(&vfio_device_set_xa);
}

module_init(vfio_init);
module_exit(vfio_cleanup);

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_ALIAS_MISCDEV(VFIO_MINOR);
MODULE_ALIAS("devname:vfio/vfio");
MODULE_SOFTDEP("post: vfio_iommu_type1 vfio_iommu_spapr_tce");
