/*
 * aesd-circular-buffer.h
 *
 *  Created on: March 1st, 2020
 *      Author: Dan Walkes
 *
 *  Modified by Pranav Shastry for AESD Assignment 8
 *
 *  Purpose:
 *      This header defines the circular buffer structures and APIs used by
 *      both the Assignment 7 userspace circular buffer logic and the
 *      Assignment 8 aesdchar kernel driver.
 */

#ifndef AESD_CIRCULAR_BUFFER_H
#define AESD_CIRCULAR_BUFFER_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>   /* size_t */
#include <stdint.h>   /* uint8_t */
#include <stdbool.h>  /* bool */
#endif

/*
 * The circular buffer stores the 10 most recent complete write operations.
 */
#define AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED 10

/*
 * Represents one stored write operation.
 *
 * buffptr points to the beginning of the stored command buffer.
 * size gives the number of valid bytes in that command buffer.
 */
struct aesd_buffer_entry
{
    const char *buffptr;
    size_t size;
};

/*
 * Circular buffer metadata.
 *
 * entry[]   : storage for the last 10 complete write operations
 * in_offs   : index where the next entry will be inserted
 * out_offs  : index of the oldest valid entry
 * full      : true when all 10 slots are occupied
 */
struct aesd_circular_buffer
{
    struct aesd_buffer_entry entry[AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED];
    uint8_t in_offs;
    uint8_t out_offs;
    bool full;
};

/*
 * Find the circular buffer entry corresponding to a linear character offset
 * if all valid buffer entries were concatenated end-to-end.
 *
 * Parameters:
 *   buffer                : circular buffer to search
 *   char_offset           : zero-based offset into the concatenated data stream
 *   entry_offset_byte_rtn : returned byte offset within the matching entry
 *
 * Returns:
 *   Pointer to the matching aesd_buffer_entry if found
 *   NULL if the requested offset is beyond the valid data stored
 *
 * Note:
 *   Caller is responsible for any required locking.
 */
extern struct aesd_buffer_entry *
aesd_circular_buffer_find_entry_offset_for_fpos(struct aesd_circular_buffer *buffer,
                                                size_t char_offset,
                                                size_t *entry_offset_byte_rtn);

/*
 * Add a completed write entry into the circular buffer.
 *
 * If the buffer is already full, the oldest entry is overwritten.
 * In that case, the overwritten entry is returned so the caller can free any
 * dynamically allocated memory associated with it.
 *
 * Parameters:
 *   buffer    : circular buffer to modify
 *   add_entry : entry to insert
 *
 * Returns:
 *   The overwritten entry when the buffer was full
 *   An empty entry { NULL, 0 } when no overwrite occurred
 *
 * Note:
 *   Caller is responsible for any required locking.
 */
extern struct aesd_buffer_entry
aesd_circular_buffer_add_entry(struct aesd_circular_buffer *buffer,
                               const struct aesd_buffer_entry *add_entry);

/*
 * Initialize the circular buffer to an empty state.
 */
extern void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer);

/*
 * Macro to iterate through all entries of the circular buffer storage array.
 *
 * This is especially useful when freeing dynamically allocated buffers during
 * cleanup.
 *
 * Example:
 *
 * uint8_t index;
 * struct aesd_buffer_entry *entry;
 * AESD_CIRCULAR_BUFFER_FOREACH(entry, &buffer, index) {
 *     if (entry->buffptr) {
 *         free((void *)entry->buffptr);
 *     }
 * }
 */
#define AESD_CIRCULAR_BUFFER_FOREACH(entryptr, buffer, index)                 \
    for ((index) = 0, (entryptr) = &((buffer)->entry[(index)]);               \
         (index) < AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;                   \
         (index)++, (entryptr) = &((buffer)->entry[(index)]))

#endif /* AESD_CIRCULAR_BUFFER_H */
