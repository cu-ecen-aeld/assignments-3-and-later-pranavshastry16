/**
 * @file aesdchar.c
 * @brief Character driver implementation for AESD Assignment 8
 *
 * This driver provides a /dev/aesdchar character device backed by a circular
 * buffer storing the 10 most recent complete write commands.
 *
 * Modified for Assignment 9.
 *
 * Based on the "scull" character driver style from Linux Device Drivers.
 *
 * Original author: Dan Walkes
 * Modified by: Pranav Shastry
 *
 * AI Declaration:
 * ChatGPT LLM was used to take some assistance for sections of this assignment. An approach was given by the lLM and I worked towards implementing the functions. Wherever errors were encountered, assistance of the LLM was taken to debug and also generate some sections of the code. I have used the LLM to also generate some of the comments for the code I have written.
 * 
 * Assigment 9 Chat History Link : https://chatgpt.com/share/69c0ac6b-b854-8010-aaf6-cd386ac3283d
 *
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

#include <linux/ioctl.h>

#include "aesd_ioctl.h"

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



static loff_t aesd_llseek(struct file *filp, loff_t offset, int whence);

static long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

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
 * @brief Compute total number of valid bytes currently stored in the circular buffer.
 *
 * The circular buffer stores up to 10 complete write commands. This helper walks
 * from the oldest valid entry to the newest valid entry and sums their sizes.
 *
 * Caller must hold dev->lock before calling this helper.
 *
 * @param buffer Circular buffer to inspect
 * @return Total number of valid bytes stored across all active entries
 */
static size_t aesd_circular_buffer_total_size(struct aesd_circular_buffer *buffer)
{
    size_t total_size = 0;
    uint8_t valid_entry_count;
    uint8_t i;

    if (buffer == NULL) {
        return 0;
    }

    if (buffer->full) {
        valid_entry_count = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    } else if (buffer->in_offs >= buffer->out_offs) {
        valid_entry_count = (uint8_t)(buffer->in_offs - buffer->out_offs);
    } else {
        valid_entry_count = (uint8_t)(AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED
                            - buffer->out_offs + buffer->in_offs);
    }

    for (i = 0; i < valid_entry_count; i++) {
        uint8_t entry_index =
            (uint8_t)((buffer->out_offs + i) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED);

        if (buffer->entry[entry_index].buffptr != NULL) {
            total_size += buffer->entry[entry_index].size;
        }
    }

    return total_size;
}


/**
 * @brief Convert an AESD ioctl seek request into an absolute file position.
 *
 * The write_cmd field identifies which stored complete write command to seek into,
 * using zero-based indexing relative to the oldest currently stored command.
 * The write_cmd_offset field identifies the zero-based byte offset within that
 * command.
 *
 * Example:
 *   commands = ["Grass\n", "Sentosa\n", "Singapore\n"]
 *   write_cmd = 1, write_cmd_offset = 2
 *   absolute position = len("Grass\n") + 2
 *
 * Caller must hold dev->lock before calling this helper.
 *
 * @param dev Device containing the circular buffer
 * @param seekto User-provided ioctl seek description
 * @param absolute_offset_rtn Returned absolute file position on success
 * @return 0 on success, -EINVAL if command or offset is invalid
 */
static int aesd_adjust_file_offset(struct aesd_dev *dev,
                                   const struct aesd_seekto *seekto,
                                   loff_t *absolute_offset_rtn)
{
    uint8_t valid_entry_count;
    uint8_t i;
    loff_t running_offset = 0;

    if ((dev == NULL) || (seekto == NULL) || (absolute_offset_rtn == NULL)) {
        return -EINVAL;
    }

    if (dev->circular_buffer.full) {
        valid_entry_count = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    } else if (dev->circular_buffer.in_offs >= dev->circular_buffer.out_offs) {
        valid_entry_count =
            (uint8_t)(dev->circular_buffer.in_offs - dev->circular_buffer.out_offs);
    } else {
        valid_entry_count =
            (uint8_t)(AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED
            - dev->circular_buffer.out_offs + dev->circular_buffer.in_offs);
    }

    /*
     * write_cmd must refer to one of the currently stored valid commands.
     */
    if (seekto->write_cmd >= valid_entry_count) {
        return -EINVAL;
    }

    /*
     * Walk valid entries from oldest to newest.
     * Sum sizes of all commands before the target command.
     * Then validate the requested byte offset within the target command.
     */
    for (i = 0; i < valid_entry_count; i++) {
        uint8_t entry_index =
            (uint8_t)((dev->circular_buffer.out_offs + i) %
            AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED);
        struct aesd_buffer_entry *entry = &dev->circular_buffer.entry[entry_index];

        if ((entry->buffptr == NULL) || (entry->size == 0)) {
            continue;
        }

        if (i == seekto->write_cmd) {
            if (seekto->write_cmd_offset >= entry->size) {
                return -EINVAL;
            }

            *absolute_offset_rtn = running_offset + seekto->write_cmd_offset;
            return 0;
        }

        running_offset += entry->size;
    }

    return -EINVAL;
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
*f_pos += retval;

out:
    mutex_unlock(&dev->lock);
    kfree(kernel_buffer);
    return retval;
}


/**
 * @brief Custom llseek implementation for /dev/aesdchar
 *
 * This driver exposes the circular buffer contents as one logical concatenated
 * byte stream. llseek moves filp->f_pos within that logical byte stream.
 *
 * We use the kernel helper fixed_size_llseek() once we determine the current
 * total size of valid data stored in the circular buffer.
 *
 * Supported whence values:
 *   SEEK_SET : position = offset
 *   SEEK_CUR : position = current + offset
 *   SEEK_END : position = total_size + offset
 *
 * @param filp Open file pointer for this device instance
 * @param offset Requested offset
 * @param whence SEEK_SET / SEEK_CUR / SEEK_END
 * @return New file position on success, negative error code on failure
 */
static loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    loff_t retval;
    size_t total_size;
    struct aesd_dev *dev = filp->private_data;

    PDEBUG("llseek offset=%lld whence=%d", offset, whence);

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    /*
     * Compute the total currently readable byte count exposed by the device.
     */
    total_size = aesd_circular_buffer_total_size(&dev->circular_buffer);

    /*
     * Let the kernel helper handle SEEK_SET / SEEK_CUR / SEEK_END semantics
     * and range validation against the current logical device size.
     */
    retval = fixed_size_llseek(filp, offset, whence, total_size);

    mutex_unlock(&dev->lock);
    return retval;
}


/**
 * @brief unlocked_ioctl handler for AESDCHAR_IOCSEEKTO
 *
 * This ioctl allows userspace to seek directly to:
 *   - a specific stored write command (zero-based from oldest valid command)
 *   - a specific byte offset within that command
 *
 * The ioctl argument is a userspace pointer to struct aesd_seekto.
 *
 * @param filp Open file pointer for this device instance
 * @param cmd ioctl command number
 * @param arg userspace pointer to struct aesd_seekto
 * @return 0 on success, negative error code on failure
 */
static long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    long retval = 0;
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto seekto;
    loff_t new_offset = 0;

    PDEBUG("ioctl cmd=0x%x", cmd);

    /*
     * Reject unsupported ioctl magic or command numbers.
     */
    if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC) {
        return -ENOTTY;
    }

    if (_IOC_NR(cmd) > AESDCHAR_IOC_MAXNR) {
        return -ENOTTY;
    }

    switch (cmd) {
    case AESDCHAR_IOCSEEKTO:
        /*
         * Copy ioctl payload from userspace into kernel memory.
         */
        if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto))) {
            return -EFAULT;
        }

        if (mutex_lock_interruptible(&dev->lock)) {
            return -ERESTARTSYS;
        }

        /*
         * Translate (write_cmd, write_cmd_offset) into one absolute file position.
         */
        retval = aesd_adjust_file_offset(dev, &seekto, &new_offset);
        if (retval == 0) {
            filp->f_pos = new_offset;
        }

        mutex_unlock(&dev->lock);
        break;

    default:
        retval = -ENOTTY;
        break;
    }

    return retval;
}


/*
 * File operations table for /dev/aesdchar
 */
struct file_operations aesd_fops = {
    .owner          = THIS_MODULE,
    .read           = aesd_read,
    .write          = aesd_write,
    .open           = aesd_open,
    .release        = aesd_release,
    .llseek         = aesd_llseek,
    .unlocked_ioctl = aesd_ioctl,
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
