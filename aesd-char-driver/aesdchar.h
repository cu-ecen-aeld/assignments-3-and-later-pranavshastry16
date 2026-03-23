/*
 * aesdchar.h
 *
 *  Created on: Oct 23, 2019
 *      Author: Dan Walkes
 */

#ifndef AESD_CHAR_DRIVER_AESDCHAR_H_
#define AESD_CHAR_DRIVER_AESDCHAR_H_

#define AESD_DEBUG 1  // Remove comment on this line to enable debug

#undef PDEBUG
#ifdef AESD_DEBUG
#  ifdef __KERNEL__
#    define PDEBUG(fmt, args...) printk(KERN_DEBUG "aesdchar: " fmt, ## args)
#  else
#    define PDEBUG(fmt, args...) fprintf(stderr, fmt, ## args)
#  endif
#else
#  define PDEBUG(fmt, args...)
#endif

#include <linux/cdev.h>
#include <linux/mutex.h>
#include "aesd-circular-buffer.h"

struct aesd_dev
{
    /*
     * Circular buffer holding the last 10 complete newline-terminated write commands.
     */
    struct aesd_circular_buffer circular_buffer;

    /*
     * Temporary working entry used to accumulate partial writes until a newline is received.
     * Once the command becomes complete, this entry is pushed into the circular buffer.
     */
    struct aesd_buffer_entry working_entry;

    /*
     * Mutex protecting all accesses to the circular buffer and working entry.
     * This ensures that multi-threaded or multi-process read/write activity
     * does not corrupt driver state.
     */
    struct mutex lock;

    /*
     * Character device structure registered with the kernel.
     */
    struct cdev cdev;
};

#endif /* AESD_CHAR_DRIVER_AESDCHAR_H_ */
