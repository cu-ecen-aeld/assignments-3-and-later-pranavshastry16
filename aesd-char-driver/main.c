/**
 * @file aesdchar.c
 * @brief Character driver implementation for AESD Assignment 8
 *
 * This driver provides a /dev/aesdchar character device backed by a circular
 * buffer storing the 10 most recent complete write commands.
 *
 * Key Assignment 8 behavior:
 *   - Writes are accumulated until a newline '\n' is received
 *   - Each complete newline-terminated command is stored as one buffer entry
 *   - The 10 most recent complete commands are retained
 *   - Old overwritten commands are freed to avoid memory leaks
 *   - Reads expose the stored commands as one linear byte stream
 *
 * Based on the "scull" character driver style from Linux Device Drivers.
 *
 * Original author: Dan Walkes
 * Modified by: Pranav Shastry
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h>      /* file_operations */
#include <linux/slab.h>    /* kmalloc, kfree, krealloc */
#include <linux/uaccess.h> /* copy_to_user, copy_from_user */
#include <linux/string.h>  /* memcpy, memchr, memset */
#include <linux/mutex.h>   /* mutex */
#include <linux/kernel.h>  /* container_of, min */

#include "aesdchar.h"

/*
 * Use dynamically allocated major number.
 */
int aesd_major = 0;
int aesd_minor = 0;

MODULE_AUTHOR("Pranav Shastry");
MODULE_LICENSE("Dual BSD/GPL");

/*
 * Single global device instance for this assignment.
 */
struct aesd_dev aesd_device;

/**
 * @brief Open handler for /dev/aesdchar
 *
 * The kernel gives us an inode containing a pointer to the embedded cdev.
 * We recover the parent aesd_dev structure using container_of(), then store
 * it in filp->private_data so subsequent read/write/release operations can
 * access device state easily.
 */
static int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev = NULL;

    PDEBUG("open");

    /*
     * Recover the enclosing aesd_dev from the embedded cdev structure.
     */
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);

    /*
     * Save device pointer for later file operations on this open instance.
     */
    filp->private_data = dev;

    return 0;
}

/**
 * @brief Release handler for /dev/aesdchar
 *
 * No special close-time action is required because the device state is global
 * and persists independently of any one file descriptor.
 */
static int aesd_release(struct inode *inode, struct file *filp)
{
    (void)inode;
    (void)filp;

    PDEBUG("release");
    return 0;
}

/**
 * @brief Read from the device
 *
 * Reads treat the stored circular buffer entries as one logical concatenated
 * byte stream. The file position (*f_pos) is used to determine:
 *   1. which circular buffer entry contains the requested byte
 *   2. the offset within that entry
 *
 * To simplify the logic, each read operation returns bytes from at most one
 * circular buffer entry. Userspace can call read() repeatedly until EOF.
 */
static ssize_t aesd_read(struct file *filp, char __user *buf,
                         size_t count, loff_t *f_pos)
{
    ssize_t retval = 0;
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry = NULL;
    size_t entry_offset = 0;
    size_t bytes_available = 0;
    size_t bytes_to_copy = 0;

    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    /*
     * Lock device state so the circular buffer does not change while we are
     * locating the requested offset and copying data to userspace.
     */
    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    /*
     * Map the current file position into a specific circular buffer entry and
     * byte offset within that entry.
     */
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(
                &dev->circular_buffer,
                *f_pos,
                &entry_offset);

    /*
     * No matching entry means end-of-file for the currently stored data.
     */
    if (entry == NULL) {
        retval = 0;
        goto out;
    }

    /*
     * Return data from at most one entry during this read call.
     */
    bytes_available = entry->size - entry_offset;
    bytes_to_copy = min(count, bytes_available);

    if (copy_to_user(buf, entry->buffptr + entry_offset, bytes_to_copy)) {
        retval = -EFAULT;
        goto out;
    }

    /*
     * Advance file position by the number of bytes successfully returned.
     */
    *f_pos += bytes_to_copy;
    retval = bytes_to_copy;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

/**
 * @brief Write to the device
 *
 * The Assignment 8 driver ignores write file position and instead behaves like:
 *   - accumulate bytes into a working buffer
 *   - once a newline '\n' is seen, treat the accumulated bytes as one complete
 *     command and push it into the circular buffer
 *
 * Complete commands are dynamically allocated. If the circular buffer is full,
 * the overwritten oldest entry is returned by aesd_circular_buffer_add_entry()
 * so it can be freed.
 */
static ssize_t aesd_write(struct file *filp, const char __user *buf,
                          size_t count, loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    struct aesd_dev *dev = filp->private_data;
    char *kernel_buffer = NULL;
    char *new_working_buffer = NULL;
    bool newline_found = false;
    struct aesd_buffer_entry completed_entry;
    struct aesd_buffer_entry overwritten_entry;

    PDEBUG("write %zu bytes with offset %lld", count, *f_pos);

    /*
     * File position is ignored for Assignment 8 write semantics.
     */
    (void)f_pos;

    /*
     * Allocate temporary kernel memory for the incoming user data.
     */
    kernel_buffer = kmalloc(count, GFP_KERNEL);
    if (kernel_buffer == NULL) {
        return -ENOMEM;
    }

    /*
     * Copy the user payload into kernel-owned memory.
     */
    if (copy_from_user(kernel_buffer, buf, count)) {
        kfree(kernel_buffer);
        return -EFAULT;
    }

    /*
     * Lock device state so the entire write operation is atomic relative to
     * other writers/readers.
     */
    if (mutex_lock_interruptible(&dev->lock)) {
        kfree(kernel_buffer);
        return -ERESTARTSYS;
    }

    /*
     * Extend the working entry to append this new write chunk onto any
     * previously buffered partial command.
     */
    new_working_buffer = krealloc((void *)dev->working_entry.buffptr,
                                  dev->working_entry.size + count,
                                  GFP_KERNEL);
    if (new_working_buffer == NULL) {
        retval = -ENOMEM;
        goto out;
    }

    /*
     * Append new data to the end of the working entry buffer.
     */
    memcpy(new_working_buffer + dev->working_entry.size, kernel_buffer, count);
    dev->working_entry.buffptr = new_working_buffer;
    dev->working_entry.size += count;

    /*
     * If this write contains a newline, the accumulated working entry now
     * represents one complete command ready for the circular buffer.
     */
    if (memchr(kernel_buffer, '\n', count) != NULL) {
        newline_found = true;
    }

    if (newline_found) {
        completed_entry = dev->working_entry;

        /*
         * Add the completed command to the circular buffer.
         * If the buffer was full, free the overwritten old entry.
         */
        overwritten_entry =
            aesd_circular_buffer_add_entry(&dev->circular_buffer,
                                           &completed_entry);

        if (overwritten_entry.buffptr != NULL) {
            kfree(overwritten_entry.buffptr);
        }

        /*
         * Reset working entry because ownership of the completed command has
         * moved into the circular buffer.
         */
        dev->working_entry.buffptr = NULL;
        dev->working_entry.size = 0;
    }

    /*
     * A successful write reports that all input bytes were consumed.
     */
    retval = count;

out:
    mutex_unlock(&dev->lock);
    kfree(kernel_buffer);
    return retval;
}

/*
 * File operations table for /dev/aesdchar
 */
struct file_operations aesd_fops = {
    .owner   = THIS_MODULE,
    .read    = aesd_read,
    .write   = aesd_write,
    .open    = aesd_open,
    .release = aesd_release,
};

/**
 * @brief Register and initialize the embedded cdev
 */
static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err;
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;

    err = cdev_add(&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev\n", err);
    }

    return err;
}

/**
 * @brief Module initialization
 *
 * Steps:
 *   - allocate device number
 *   - clear and initialize device state
 *   - initialize circular buffer
 *   - initialize working entry for partial writes
 *   - initialize mutex
 *   - register character device
 */
static int __init aesd_init_module(void)
{
    dev_t dev = 0;
    int result;

    result = alloc_chrdev_region(&dev, aesd_minor, 1, "aesdchar");
    aesd_major = MAJOR(dev);

    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }

    /*
     * Start from a known zeroed state.
     */
    memset(&aesd_device, 0, sizeof(struct aesd_dev));

    /*
     * Initialize circular buffer holding the 10 most recent complete commands.
     */
    aesd_circular_buffer_init(&aesd_device.circular_buffer);

    /*
     * Initialize working entry used for accumulating partial writes until '\n'.
     */
    aesd_device.working_entry.buffptr = NULL;
    aesd_device.working_entry.size = 0;

    /*
     * Initialize mutex for safe concurrent access.
     */
    mutex_init(&aesd_device.lock);

    result = aesd_setup_cdev(&aesd_device);
    if (result) {
        unregister_chrdev_region(dev, 1);
    }

    return result;
}

/**
 * @brief Module cleanup
 *
 * Free all dynamically allocated completed entries stored in the circular
 * buffer, as well as any partially accumulated working entry, then unregister
 * the character device region.
 */
static void __exit aesd_cleanup_module(void)
{
    uint8_t index;
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    /*
     * Remove cdev first so no new opens occur during cleanup.
     */
    cdev_del(&aesd_device.cdev);

    /*
     * Free all completed command buffers stored in the circular buffer.
     */
    for (index = 0; index < AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED; index++) {
        if (aesd_device.circular_buffer.entry[index].buffptr != NULL) {
            kfree(aesd_device.circular_buffer.entry[index].buffptr);
            aesd_device.circular_buffer.entry[index].buffptr = NULL;
            aesd_device.circular_buffer.entry[index].size = 0;
        }
    }

    /*
     * Free any partially accumulated write still pending in working_entry.
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
