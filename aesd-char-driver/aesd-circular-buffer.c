/**
 * @file aesd-circular-buffer.c
 * @brief Circular buffer helper functions shared by Assignment 7 and Assignment 8
 *
 * This file implements a fixed-size circular buffer storing the 10 most recent
 * complete write operations. In Assignment 8, each stored entry may point to
 * dynamically allocated memory managed by the driver.
 *
 * Original author: Dan Walkes
 * Modified by: Pranav Shastry
 */

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "aesd-circular-buffer.h"

/**
 * @brief Find the entry and byte offset corresponding to a linear file position.
 *
 * Conceptually, if all valid entries in the circular buffer were concatenated
 * into one long byte stream, this function maps a requested character offset
 * into:
 *   1. which entry contains that byte
 *   2. the byte offset within that entry
 *
 * @param buffer Circular buffer to search. Caller must hold any required lock.
 * @param char_offset Zero-based character offset into the concatenated stream.
 * @param entry_offset_byte_rtn Output parameter returning the byte offset within
 *        the matched entry.
 *
 * @return Pointer to the matching entry if found, or NULL if the requested
 *         offset is beyond the amount of valid data currently stored.
 */
struct aesd_buffer_entry *
aesd_circular_buffer_find_entry_offset_for_fpos(struct aesd_circular_buffer *buffer,
                                                size_t char_offset,
                                                size_t *entry_offset_byte_rtn)
{
    size_t total_bytes_seen = 0;
    uint8_t valid_entry_count;
    uint8_t i;

    if ((buffer == NULL) || (entry_offset_byte_rtn == NULL)) {
        return NULL;
    }

    *entry_offset_byte_rtn = 0;

    /*
     * Determine how many valid entries currently exist in the circular buffer.
     *
     * Case 1: full == true
     *         All 10 entries are valid.
     *
     * Case 2: in_offs >= out_offs
     *         Valid entries occupy one contiguous region.
     *
     * Case 3: in_offs < out_offs
     *         Valid entries wrap around the end of the array.
     */
    if (buffer->full) {
        valid_entry_count = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    } else if (buffer->in_offs >= buffer->out_offs) {
        valid_entry_count = (uint8_t)(buffer->in_offs - buffer->out_offs);
    } else {
        valid_entry_count = (uint8_t)(AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED
                            - buffer->out_offs + buffer->in_offs);
    }

    /*
     * Walk through entries from oldest to newest, accumulating total size
     * until we find the entry containing char_offset.
     */
    for (i = 0; i < valid_entry_count; i++) {
        uint8_t entry_index =
            (uint8_t)((buffer->out_offs + i) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED);

        struct aesd_buffer_entry *entry = &buffer->entry[entry_index];

        /*
         * Ignore empty/uninitialized entries defensively.
         */
        if ((entry->buffptr == NULL) || (entry->size == 0)) {
            continue;
        }

        /*
         * If char_offset falls within the current entry, return it.
         */
        if (char_offset >= total_bytes_seen) {
            size_t offset_within_entry = char_offset - total_bytes_seen;

            if (offset_within_entry < entry->size) {
                *entry_offset_byte_rtn = offset_within_entry;
                return entry;
            }
        }

        total_bytes_seen += entry->size;
    }

    /*
     * Requested offset lies beyond the valid stored data.
     */
    return NULL;
}

/**
 * @brief Add a completed write entry into the circular buffer.
 *
 * The new entry is always inserted at in_offs.
 *
 * If the buffer is not full:
 *   - the entry is inserted
 *   - in_offs advances
 *   - full becomes true only if insertion causes in_offs == out_offs
 *
 * If the buffer is already full:
 *   - the oldest entry (currently at in_offs) will be overwritten
 *   - that overwritten entry is returned to the caller
 *   - out_offs advances so the logical "oldest entry" moves forward
 *
 * @param buffer Circular buffer to update. Caller must hold any required lock.
 * @param add_entry Entry to insert.
 *
 * @return The overwritten entry if buffer was full, else { NULL, 0 }.
 */
struct aesd_buffer_entry
aesd_circular_buffer_add_entry(struct aesd_circular_buffer *buffer,
                               const struct aesd_buffer_entry *add_entry)
{
    struct aesd_buffer_entry overwritten_entry = {
        .buffptr = NULL,
        .size = 0
    };

    if ((buffer == NULL) || (add_entry == NULL)) {
        return overwritten_entry;
    }

    /*
     * If the buffer is full, the slot at in_offs currently contains the
     * oldest valid entry and is about to be replaced.
     * Save it so the caller can free associated dynamic memory if needed.
     */
    if (buffer->full) {
        overwritten_entry = buffer->entry[buffer->in_offs];

        /*
         * Since the oldest entry is being overwritten, advance out_offs
         * to the next logical oldest entry.
         */
        buffer->out_offs = (uint8_t)((buffer->out_offs + 1) %
                           AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED);
    }

    /*
     * Store the new completed entry into the insertion slot.
     */
    buffer->entry[buffer->in_offs] = *add_entry;

    /*
     * Advance insertion pointer circularly.
     */
    buffer->in_offs = (uint8_t)((buffer->in_offs + 1) %
                      AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED);

    /*
     * Buffer becomes full once insertion wraps in_offs around to out_offs.
     */
    buffer->full = (buffer->in_offs == buffer->out_offs);

    return overwritten_entry;
}

/**
 * @brief Initialize a circular buffer to the empty state.
 *
 * This clears:
 *   - all entry metadata
 *   - in_offs
 *   - out_offs
 *   - full flag
 */
void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer)
{
    if (buffer != NULL) {
        memset(buffer, 0, sizeof(struct aesd_circular_buffer));
    }
}
