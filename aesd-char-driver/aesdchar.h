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
 *	Code Comparison & Error Tracing: https://chatgpt.com/share/69a0abe9-1df4-8007-b644-419269c81357
 *  Code Correction: https://chat.deepseek.com/share/2ld00dp5jchxu8bru2
 */

#ifndef AESD_CHAR_DRIVER_AESDCHAR_H_
#define AESD_CHAR_DRIVER_AESDCHAR_H_

#include <linux/cdev.h>
#include <linux/mutex.h>
#include "aesd-circular-buffer.h"

#define AESD_DEBUG 1  /* Remove comment to enable debug */

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

/** Maximum size of a single write operation (to avoid high‑order allocations) */
#define AESDCHAR_MAX_WRITE_SIZE (128 * 1024)   /* 128 KiB */

/**
 * struct aesd_file_private - Per‑file private data (currently unused)
 * @dev:           Pointer to the main device structure
 * @partial_buf:   Dynamically allocated buffer holding partial write data
 * @partial_size:  Number of valid bytes currently in @partial_buf
 * @partial_capacity: Allocated size of @partial_buf
 *
 * This structure is intended for per‑open file data but is not yet used.
 * The current driver uses global partial buffers, which is not safe for
 * concurrent opens. This is a known limitation.
 */
struct aesd_file_private {
    struct aesd_dev *dev;
    char *partial_buf;
    size_t partial_size;
    size_t partial_capacity;
};

/**
 * struct aesd_dev - Main device structure
 * @cdev:        Char device structure (must be first for cdev_init)
 * @lock:        Mutex protecting the circular buffer and serialising all writes
 * @buffer:      Circular buffer holding the most recent completed write commands
 * @partial_buf:   Global accumulation buffer for incomplete lines
 * @partial_size:  Current bytes in @partial_buf
 * @partial_capacity: Allocated size of @partial_buf
 * @total_size:     Total size (in bytes) of all data currently stored in @buffer
 *
 * One instance exists for the whole driver (@aesd_device).
 */
struct aesd_dev {
    struct cdev cdev;
    struct aesd_circular_buffer buffer;
    struct mutex lock;
    char *partial_buf;
    size_t partial_size;
    size_t partial_capacity;
    size_t total_size;                /* sum of sizes of all entries in buffer */
};

#endif /* AESD_CHAR_DRIVER_AESDCHAR_H_ */
