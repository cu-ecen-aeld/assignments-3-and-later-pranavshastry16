/**
 * @file aesd-circular-buffer.c
 * @brief Functions and data related to a circular buffer imlementation
 *
 * @author Dan Walkes
 * @date 2020-03-01
 * @copyright Copyright (c) 2020
 * 
 *
 * Modified by Pranav Shastry
 * on 01st March 2026 AESD Assignment 7 Part 1
 *
 * AI Declaration:
 * Some inspiration was taken by ChatGPT LLM for development of this code.
 * I took a roadmap from the LLM on how to implement it, did some of my
 * implementation and had some iterations with the LLM on how to make it 
 * better and fully satisfy all the requirements of the assignment.
 *
 * Chat History Link: https://chatgpt.com/share/69a4f7df-26a8-8010-94a1-d93af04261f6
 */

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "aesd-circular-buffer.h"

/**
 * @param buffer the buffer to search for corresponding offset.  Any necessary locking must be performed by caller.
 * @param char_offset the position to search for in the buffer list, describing the zero referenced
 *      character index if all buffer strings were concatenated end to end
 * @param entry_offset_byte_rtn is a pointer specifying a location to store the byte of the returned aesd_buffer_entry
 *      buffptr member corresponding to char_offset.  This value is only set when a matching char_offset is found
 *      in aesd_buffer.
 * @return the struct aesd_buffer_entry structure representing the position described by char_offset, or
 * NULL if this position is not available in the buffer (not enough data is written).
 */
struct aesd_buffer_entry *aesd_circular_buffer_find_entry_offset_for_fpos(struct aesd_circular_buffer *buffer,
            size_t char_offset, size_t *entry_offset_byte_rtn )
{
    /**
    * TODO: implement per description
    */
    if (!buffer || !entry_offset_byte_rtn) {
        return NULL;
    }

    *entry_offset_byte_rtn = 0;

    size_t total = 0;
    uint8_t count;

    if (buffer->full) {
        count = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    } else if (buffer->in_offs >= buffer->out_offs) {
        count = (uint8_t)(buffer->in_offs - buffer->out_offs);
    } else {
        count = (uint8_t)(AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED - buffer->out_offs + buffer->in_offs);
    }

    for (uint8_t i = 0; i < count; i++) {
        uint8_t idx = (uint8_t)((buffer->out_offs + i) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED);
        struct aesd_buffer_entry *ent = &buffer->entry[idx];

        if (!ent->buffptr || ent->size == 0) {
            continue;
        }

        if (char_offset >= total) {
            size_t off = char_offset - total;
            if (off < ent->size) {
                *entry_offset_byte_rtn = off;
                return ent;
            }
        }

        total += ent->size;
    }

    return NULL;
}



/**
* Adds entry @param add_entry to @param buffer in the location specified in buffer->in_offs.
* If the buffer was already full, overwrites the oldest entry and advances buffer->out_offs to the
* new start location.
* Any necessary locking must be handled by the caller
* Any memory referenced in @param add_entry must be allocated by and/or must have a lifetime managed by the caller.
*/
void aesd_circular_buffer_add_entry(struct aesd_circular_buffer *buffer, const struct aesd_buffer_entry *add_entry)
{
    /**
    * TODO: implement per description
    */
    if (!buffer || !add_entry) {
        return;
    }

    if (buffer->full) {
        buffer->out_offs = (uint8_t)((buffer->out_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED);
    }

    buffer->entry[buffer->in_offs] = *add_entry;

    buffer->in_offs = (uint8_t)((buffer->in_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED);

    buffer->full = (buffer->in_offs == buffer->out_offs);
}

/**
* Initializes the circular buffer described by @param buffer to an empty struct
*/
void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer)
{
    memset(buffer,0,sizeof(struct aesd_circular_buffer));
}
