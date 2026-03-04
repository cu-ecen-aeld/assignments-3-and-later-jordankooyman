/**
 * @file aesd-circular-buffer.c
 * @brief Functions and data related to a circular buffer implementation
 * 
 * Code was written with AI Generation Tools, see chats below:
 * 		Implementation 1: https://chat.deepseek.com/share/1v5chtjo6sot1iynho
 * 		Implementaiton 2: https://chat.deepseek.com/share/4h4fhmx5k7k2i6x4t9
 * 		Comparison: https://chatgpt.com/share/6998baea-4684-8007-b051-4bde111c3994
 *
 * @author Dan Walkes, Jordan Kooyman
 * @date 2020-03-01, 2026-02-20
 * @copyright Copyright (c) 2020
 *
 */

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "aesd-circular-buffer.h"

/**
 * Internal invariants for the circular buffer:
 * - Empty:  (in_offs == out_offs) && !full
 * - Full:   (in_offs == out_offs) && full
 * - Partially filled: (in_offs != out_offs) && !full
 *   Valid entries are from out_offs up to (but not including) in_offs, wrapping around.
 */

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
    /* Defensive: validate input pointers */
    if (!buffer || !entry_offset_byte_rtn) {
        return NULL;
    }

    /* Empty when not full and pointers equal */
    if (!buffer->full && (buffer->in_offs == buffer->out_offs)) {
        *entry_offset_byte_rtn = 0;      /* avoid stale value on failure */
        return NULL;
    }

    size_t cumulative = 0;
    size_t num_entries;
    size_t i;
    uint8_t index;               /* follows the circular order */

    /* Determine number of valid entries currently stored */
    if (buffer->full) {
        num_entries = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    } else {
        /* Number = (in_offs - out_offs) modulo MAX, computed safely */
        num_entries = (buffer->in_offs + AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED
                       - buffer->out_offs) %
                      AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }

    /* Start from the oldest entry */
    index = buffer->out_offs;

    for (i = 0; i < num_entries; i++) {
        struct aesd_buffer_entry *entry = &buffer->entry[index];

        /* Does the requested offset lie within this entry? */
        if (char_offset < cumulative + entry->size) {
            *entry_offset_byte_rtn = char_offset - cumulative;
            return entry;
        }

        cumulative += entry->size;

        /* Move to next valid entry, wrapping around */
        index = (index + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }

    /* Offset beyond total data */
    *entry_offset_byte_rtn = 0;
    return NULL;
}

/**
* Adds entry @param add_entry to @param buffer in the location specified in buffer->in_offs.
* If the buffer was already full, overwrites the oldest entry and advances buffer->out_offs to the
* new start location.
* Any necessary locking must be handled by the caller
* Any memory referenced in @param add_entry must be allocated by and/or must have a lifetime managed by the caller.
*
* NOTE: When the buffer is full, the entry at out_offs is overwritten. If that entry's buffptr
*       points to dynamically allocated memory, the caller is responsible for freeing that memory
*       BEFORE calling this function if ownership of the overwritten data is no longer needed.
*/
void aesd_circular_buffer_add_entry(struct aesd_circular_buffer *buffer, const struct aesd_buffer_entry *add_entry)
{
    /* Defensive: validate input pointers */
    if (!buffer || !add_entry) {
        return;
    }

    /* Store the new entry at the current write position */
    buffer->entry[buffer->in_offs] = *add_entry;

    /* If the buffer was full, we just overwrote the oldest entry.
     * Therefore we must advance out_offs to the next oldest entry.
     */
    if (buffer->full) {
        buffer->out_offs = (buffer->out_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }

    /* Always advance the write pointer to the next slot */
    buffer->in_offs = (buffer->in_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

    /* Update the full flag.
     * The buffer becomes full when the write pointer catches up to the read pointer.
     * This condition also holds after an overwrite (full remains true).
     */
    buffer->full = (buffer->in_offs == buffer->out_offs);
}

/**
* Initializes the circular buffer described by @param buffer to an empty struct
*/
void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer)
{
    if (buffer) {
        memset(buffer, 0, sizeof(struct aesd_circular_buffer));
    }
}
