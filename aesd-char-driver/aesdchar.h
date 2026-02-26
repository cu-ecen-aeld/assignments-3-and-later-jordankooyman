/*
 * aesdchar.h
 *
 *  Created on: Oct 23, 2019
 *      Author: Dan Walkes
 * 
 * Code completed with assistance from ChatGPT and DeepSeek AI Tools
 *	Code Variation 2: https://chat.deepseek.com/share/blqdq52rrkyiuabq9f 
 *	Code Variation 3: https://chat.deepseek.com/share/dqf5xmgb4wjh0dinb4
 *	Code Variation 4 (kept): https://chat.deepseek.com/share/51svkx0j4vgx9uegsw
 *	Code Comparison: https://chatgpt.com/share/69a0abe9-1df4-8007-b644-419269c81357
 * 
 */

#ifndef AESD_CHAR_DRIVER_AESDCHAR_H_
#define AESD_CHAR_DRIVER_AESDCHAR_H_

#include <linux/cdev.h>
#include <linux/mutex.h>
#include "aesd-circular-buffer.h"

#define AESD_DEBUG 1  // Remove comment to enable debug

#undef PDEBUG
#ifdef AESD_DEBUG
#  ifdef __KERNEL__
#    define PDEBUG(fmt, args...) printk(KERN_DEBUG "aesdchar: " fmt, ## args)
#  else
#    define PDEBUG(fmt, args...) fprintf(stderr, fmt, ## args)
#  endif
#else
#  define PDEBUG(fmt, args...) /* nothing */
#endif

/** Maximum size of a single write operation (to avoid high-order allocations) */
#define AESDCHAR_MAX_WRITE_SIZE (128 * 1024)   /* 128 KiB */

/**
 * struct aesd_file_private - Per-file private data
 * @dev:           Pointer to the main device structure (for convenience)
 * @partial_buf:   Dynamically allocated buffer holding partial write data
 *                 (data not yet terminated by a newline)
 * @partial_size:  Number of valid bytes currently in @partial_buf
 * @partial_capacity: Allocated size of @partial_buf (for efficient reallocation)
 *
 * This structure is allocated in @aesd_open and freed in @aesd_release.
 * It holds incomplete write data for a specific open file instance.
 * All writes are serialised by the global device lock, so no per‑file lock
 * is needed.
 */
struct aesd_file_private {
    struct aesd_dev *dev;
    char *partial_buf;
    size_t partial_size;
    size_t partial_capacity;
};

/**
 * struct aesd_dev - Main device structure
 * @cdev:   Char device structure (must be first for cdev_init)
 * @lock:   Mutex protecting the circular buffer and serialising all writes
 * @buffer: Circular buffer holding the most recent completed write commands
 *
 * One instance exists for the whole driver (@aesd_device).
 */
struct aesd_dev
{
    struct cdev cdev;
    struct mutex lock;
    struct aesd_circular_buffer buffer;
};

#endif /* AESD_CHAR_DRIVER_AESDCHAR_H_ */
