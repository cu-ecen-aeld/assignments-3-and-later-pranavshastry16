/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h>      // file_operations
#include <linux/slab.h>    // kmalloc, kfree, krealloc
#include <linux/uaccess.h> // copy_to_user, copy_from_user
#include <linux/string.h>  // memcpy, memchr, memset
#include <linux/mutex.h>   // mutex
#include <linux/kernel.h>  // container_of, min
#include "aesdchar.h"

int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Pranav Shastry"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

static int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev = NULL;

    PDEBUG("open");
    /**
     * TODO: handle open
     */

    /**
     * Recover the pointer to our aesd_dev structure from the embedded cdev.
     * inode->i_cdev points to the cdev member inside struct aesd_dev.
     * container_of() gives us the address of the parent struct aesd_dev.
     */
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);

    /**
     * Save the device pointer in private_data so future read/write/release
     * calls on this file descriptor can directly access device state.
     */
    filp->private_data = dev;

    return 0;
}

static int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */

     /*
     * Nothing is required on close.
     * The per-device state is kept in the global device object and
     * protected by the mutex.
     */

    return 0;
}

static ssize_t aesd_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle read
     */

        ssize_t retval = 0;
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry = NULL;
    size_t entry_offset = 0;
    size_t bytes_available = 0;
    size_t bytes_to_copy = 0;


    /*
     * Lock the device so the circular buffer contents remain stable while
     * locating the requested offset and copying data to userspace.
     */
    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    /*
     * Translate the linear file position into:
     * 1. which circular buffer entry contains that byte
     * 2. the byte offset within that entry
     */
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(
                &dev->circular_buffer,
                *f_pos,
                &entry_offset);

    /*
     * No entry means we are at end-of-file for the current device contents.
     */
    if (entry == NULL) {
        retval = 0;
        goto out;
    }

    /*
     * We intentionally satisfy reads using at most one circular buffer entry
     * per read call. Userspace will call read again if more data remains.
     */
    bytes_available = entry->size - entry_offset;
    bytes_to_copy = min(count, bytes_available);

    if (copy_to_user(buf, entry->buffptr + entry_offset, bytes_to_copy)) {
        retval = -EFAULT;
        goto out;
    }

    *f_pos += bytes_to_copy;
    retval = bytes_to_copy;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

static ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle write
     */

     ssize_t retval = -ENOMEM;
    struct aesd_dev *dev = filp->private_data;
    char *kernel_buffer = NULL;
    char *new_working_buffer = NULL;
    bool newline_found = false;
    struct aesd_buffer_entry completed_entry;
    struct aesd_buffer_entry overwritten_entry;
 /*
     * Write position is ignored for this assignment.
     */
    (void)f_pos;

    /*
     * Allocate temporary kernel memory for the incoming user data.
     */
    kernel_buffer = kmalloc(count, GFP_KERNEL);
    if (!kernel_buffer) {
        return -ENOMEM;
    }

    if (copy_from_user(kernel_buffer, buf, count)) {
        kfree(kernel_buffer);
        return -EFAULT;
    }

    /*
     * Lock the device so a full write operation completes before another
     * thread/process can modify the working entry or circular buffer.
     */
    if (mutex_lock_interruptible(&dev->lock)) {
        kfree(kernel_buffer);
        return -ERESTARTSYS;
    }

    /*
     * Extend the working entry so this new write can be appended to any
     * previously buffered partial command.
     */
    new_working_buffer = krealloc((void *)dev->working_entry.buffptr,
                                  dev->working_entry.size + count,
                                  GFP_KERNEL);
    if (!new_working_buffer) {
        retval = -ENOMEM;
        goto out;
    }

    /*
     * Append the new bytes to the end of the current working entry.
     */
    memcpy(new_working_buffer + dev->working_entry.size, kernel_buffer, count);
    dev->working_entry.buffptr = new_working_buffer;
    dev->working_entry.size += count;

    /*
     * If this write chunk contains a newline, the accumulated working entry
     * now represents one complete command and should be pushed into the
     * circular buffer.
     */
    if (memchr(kernel_buffer, '\n', count) != NULL) {
        newline_found = true;
    }

    if (newline_found) {
        completed_entry = dev->working_entry;

        /*
         * Store the completed command in the circular buffer.
         * If the buffer was full, free the overwritten old command buffer.
         */
        overwritten_entry = aesd_circular_buffer_add_entry(&dev->circular_buffer,
                                                           &completed_entry);

        if (overwritten_entry.buffptr != NULL) {
            kfree(overwritten_entry.buffptr);
        }

        /*
         * Reset working entry because ownership of this completed command
         * has now moved into the circular buffer.
         */
        dev->working_entry.buffptr = NULL;
        dev->working_entry.size = 0;
    }

    retval = count;

out:
    mutex_unlock(&dev->lock);
    kfree(kernel_buffer);
    return retval;
}



struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}


static int __init aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */

    /*
     * Initialize the circular buffer which stores the last 10 complete
     * newline-terminated write commands.
     */
    aesd_circular_buffer_init(&aesd_device.circular_buffer);

    /*
     * Initialize the working entry used to accumulate partial writes until
     * a newline is received.
     */
    aesd_device.working_entry.buffptr = NULL;
    aesd_device.working_entry.size = 0;

    /*
     * Initialize the mutex used to serialize access to device state.
     */
    mutex_init(&aesd_device.lock);


    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}


static void __exit aesd_cleanup_module(void)
{
    uint8_t index;

    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */

/*
     * Free all dynamically allocated completed command buffers still stored
     * inside the circular buffer.
     */
    for (index = 0; index < AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED; index++) {
        if (aesd_device.circular_buffer.entry[index].buffptr != NULL) {
            kfree(aesd_device.circular_buffer.entry[index].buffptr);
            aesd_device.circular_buffer.entry[index].buffptr = NULL;
            aesd_device.circular_buffer.entry[index].size = 0;
        }
    }

    /*
     * Free any partial write still being accumulated in working_entry.
     */
    if (aesd_device.working_entry.buffptr != NULL) {
        kfree(aesd_device.working_entry.buffptr);
        aesd_device.working_entry.buffptr = NULL;
        aesd_device.working_entry.size = 0;
    }

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
