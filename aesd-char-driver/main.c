/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * Code completed with assistance from ChatGPT and DeepSeek AI Tools
 *	Code Variation 2: https://chat.deepseek.com/share/blqdq52rrkyiuabq9f
 *	Code Variation 3: https://chat.deepseek.com/share/dqf5xmgb4wjh0dinb4
 *	Code Variation 4 (kept): https://chat.deepseek.com/share/51svkx0j4vgx9uegsw
 *	Code Comparison & Error Tracing: https://chatgpt.com/share/69a0abe9-1df4-8007-b644-419269c81357
 *  Code Correction: https://chat.deepseek.com/share/2ld00dp5jchxu8bru2
 * Assignment 9 updates:
 *  Code Variation 1: https://chat.deepseek.com/share/t2cwufojtyfo0nnkrl
 *  Code Variation 2 (used): https://chat.deepseek.com/share/829vw2j4w8bugz5zkw
 *  Code Comparison: https://chatgpt.com/share/69a5e166-2100-8007-9e79-35fa43b28134
 *  Corrections: https://claude.ai/share/940b08c1-937e-4f59-9fe4-db34cee65a86
 *
 * @author Dan Walkes, Jordan Kooyman
 * @date 2019-10-22, 2026-02-26
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
/*
 * Fix 1: Add <linux/compat.h> for compat_ptr_ioctl.
 *
 * On Linux 6.x kernels running a 64-bit userspace binary, the kernel routes
 * ioctl calls through the compat layer even for native 64-bit processes in
 * some configurations.  Without a .compat_ioctl entry in file_operations, the
 * kernel returns -ENOTTY ("Inappropriate ioctl for device") for every ioctl
 * issued from such a process, regardless of whether unlocked_ioctl is
 * registered.  compat_ptr_ioctl is the standard kernel-provided helper that
 * simply forwards the call to unlocked_ioctl after converting any userspace
 * pointer arguments with compat_ptr().  Since our aesd_seekto struct contains
 * only uint32_t fields (no pointers), compat_ptr_ioctl is exactly the right
 * choice — it does no structural translation and adds zero overhead.
 */
#include <linux/compat.h>
#include "aesdchar.h"
#include "aesd_ioctl.h"

int aesd_major = 0;
int aesd_minor = 0;

MODULE_AUTHOR("Jordan Kooyman");
MODULE_LICENSE("Dual BSD/GPL");

/* Function prototypes */
int aesd_open(struct inode *, struct file *);
int aesd_release(struct inode *, struct file *);
ssize_t aesd_read(struct file *, char __user *, size_t, loff_t *);
ssize_t aesd_write(struct file *, const char __user *, size_t, loff_t *);
loff_t aesd_llseek(struct file *, loff_t, int);
long aesd_unlocked_ioctl(struct file *, unsigned int, unsigned long);
static long aesd_adjust_file_offset_locked(struct file *filp,
                                           unsigned int write_cmd,
                                           unsigned int write_cmd_offset);
static void aesd_add_entry_locked(struct aesd_dev *dev, const char *line, size_t size);

struct aesd_dev aesd_device;

/*
 * aesd_adjust_file_offset_locked - Translate (write_cmd, write_cmd_offset)
 * into an absolute byte offset and store it in filp->f_pos.
 *
 * Must be called with dev->lock held.
 *
 * Fix 2: Replace the original two-pass implementation (FOREACH count pass +
 * separate for-loop traverse pass) with a single clean traversal.
 *
 * The original code first used AESD_CIRCULAR_BUFFER_FOREACH to count
 * num_entries, then used a separate for loop with (out_offs + idx) % SIZE
 * indexing to walk the entries again.  This two-pass approach had several
 * problems:
 *
 *   a) Redundancy: the buffer is walked twice for no benefit.  The validation
 *      "write_cmd >= num_entries → -EINVAL" can be done during the single
 *      traversal by stopping at write_cmd entries.
 *
 *   b) The FOREACH macro iterates physical slots 0..SIZE-1 in storage order,
 *      not logical order from out_offs.  For a non-full buffer where entries
 *      do not wrap past slot 0, the counts match.  But if the buffer has
 *      wrapped (e.g. out_offs=7 on a 10-slot buffer, entries at 7,8,9,0,1)
 *      the FOREACH count and the for-loop traversal start at different
 *      positions, making the num_entries guard unreliable as a loop terminator
 *      for the logical ordering.  The old for loop used (out_offs+idx)%SIZE
 *      which is the correct logical ordering, but the break condition
 *      "cmd_seen >= num_entries" was mixing physical-count with logical-index,
 *      which happened to be correct only because the two counts always agree
 *      for a well-formed buffer — but the reasoning was non-obvious and fragile.
 *
 * The new implementation uses a single loop over logical positions starting at
 * out_offs, walks exactly min(num_filled_slots, write_cmd+1) entries, and
 * applies the bounds check inline.  num_filled_slots is computed from the
 * buffer's in_offs, out_offs, and full flag — the same arithmetic used
 * internally by the circular buffer library — so no separate count pass is
 * needed.
 */
static long aesd_adjust_file_offset_locked(struct file *filp,
                                           unsigned int write_cmd,
                                           unsigned int write_cmd_offset)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_circular_buffer *buf = &dev->buffer;
    struct aesd_buffer_entry *entry;
    unsigned int num_entries;
    unsigned int i;
    loff_t abs_offset = 0;

    /*
     * Compute how many entries are currently stored.  Using the buffer's own
     * bookkeeping fields (full, in_offs, out_offs) is more direct and reliable
     * than counting non-NULL buffptrs with FOREACH, which could be fooled by
     * a partially-initialised entry.
     */
    if (buf->full) {
        num_entries = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    } else if (buf->in_offs >= buf->out_offs) {
        num_entries = buf->in_offs - buf->out_offs;
    } else {
        num_entries = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED
                      - buf->out_offs + buf->in_offs;
    }

    /* Validate: write_cmd must refer to an entry that exists */
    if (write_cmd >= num_entries)
        return -EINVAL;

    /*
     * Walk from the oldest entry (out_offs) up to and including write_cmd,
     * accumulating the byte offset of each preceding entry.
     */
    for (i = 0; i <= write_cmd; i++) {
        entry = &buf->entry[(buf->out_offs + i) %
                             AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED];

        /*
         * A NULL buffptr here would indicate buffer corruption — the entry
         * exists in the logical sequence but has no backing memory.  Return
         * an error rather than dereferencing NULL or computing a garbage
         * offset.
         */
        if (!entry->buffptr) {
            PDEBUG("adjust_file_offset: NULL buffptr at logical index %u", i);
            return -EINVAL;
        }

        if (i == write_cmd) {
            /* Validate the byte offset within this specific entry */
            if (write_cmd_offset >= entry->size)
                return -EINVAL;

            abs_offset += write_cmd_offset;
            filp->f_pos = abs_offset;
            return 0;
        }

        abs_offset += entry->size;
    }

    /* Unreachable if validation above is correct */
    return -EINVAL;
}

/* ---------- Circular buffer helper with total_size update ---------- */
static void aesd_add_entry_locked(struct aesd_dev *dev, const char *line, size_t size)
{
    /*
     * If the buffer slot about to be overwritten already holds data, subtract
     * its size from total_size and free its backing memory before the
     * circular buffer library overwrites the slot.
     */
    if (dev->buffer.full) {
        struct aesd_buffer_entry *old = &dev->buffer.entry[dev->buffer.in_offs];
        if (old->buffptr) {
            dev->total_size -= old->size;
            kfree(old->buffptr);
            old->buffptr = NULL;
            old->size    = 0;
        }
    }

    {
        struct aesd_buffer_entry new_entry;
        new_entry.buffptr = line;
        new_entry.size    = size;
        aesd_circular_buffer_add_entry(&dev->buffer, &new_entry);
    }
    dev->total_size += size;
}

/* ---------- unlocked_ioctl ---------- */
long aesd_unlocked_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto seekto;
    long ret;

    /* Reject commands whose magic number does not match this driver */
    if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC)
        return -ENOTTY;

    switch (cmd) {
    case AESDCHAR_IOCSEEKTO:
        /*
         * copy_from_user must be called BEFORE acquiring the mutex.
         * Sleeping inside a mutex is not permitted in interrupt context, and
         * copy_from_user may sleep (page fault handling).  Although the aesd
         * mutex is not held in interrupt context, it is good practice to keep
         * the critical section as short as possible and avoid any operation
         * that may sleep while holding it.
         */
        if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto)))
            return -EFAULT;

        mutex_lock(&dev->lock);
        ret = aesd_adjust_file_offset_locked(filp, seekto.write_cmd,
                                              seekto.write_cmd_offset);
        mutex_unlock(&dev->lock);
        break;

    default:
        return -ENOTTY;
    }

    return ret;
}

/* ---------- llseek ---------- */
/*
 * aesd_llseek - Reposition the file offset for subsequent reads.
 *
 * The lecture slides (Assignment 9 Overview, slide 12) recommend Option 2:
 * add your own llseek wrapper with locking, but delegate the actual seek
 * arithmetic to the kernel's fixed_size_llseek() helper.  However,
 * fixed_size_llseek() requires the device size at the time of the call and
 * does not hold any application-level lock while computing the new position.
 * If we release dev->lock between reading total_size and calling
 * fixed_size_llseek(), a concurrent write could enlarge the buffer, making
 * the size stale (TOCTOU).
 *
 * The current hand-rolled implementation holds dev->lock for the entire
 * operation, guaranteeing that total_size cannot change between the bounds
 * check and the f_pos update.  This is MORE consistent than the
 * fixed_size_llseek option and is therefore kept.  The overflow guards for
 * SEEK_CUR and SEEK_END are explicit and tested.
 *
 * Fix 3: Declare all local variables at the top of the function body.
 * The kernel C style guide (Documentation/process/coding-style.rst) requires
 * variables to be declared before any statements.  The original function
 * already followed this convention; it is preserved here unchanged.
 */
loff_t aesd_llseek(struct file *filp, loff_t off, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t newpos;

    mutex_lock(&dev->lock);

    switch (whence) {
    case SEEK_SET:
        newpos = off;
        break;

    case SEEK_CUR:
        /* Guard against signed overflow when adding off to f_pos */
        if (off > 0 && filp->f_pos > LLONG_MAX - off) {
            mutex_unlock(&dev->lock);
            return -EINVAL;
        }
        if (off < 0 && filp->f_pos < -off) {
            mutex_unlock(&dev->lock);
            return -EINVAL;
        }
        newpos = filp->f_pos + off;
        break;

    case SEEK_END:
        /* Guard against signed overflow when adding off to total_size */
        if (off > 0 && (loff_t)dev->total_size > LLONG_MAX - off) {
            mutex_unlock(&dev->lock);
            return -EINVAL;
        }
        if (off < 0 && (loff_t)dev->total_size < -off) {
            mutex_unlock(&dev->lock);
            return -EINVAL;
        }
        newpos = (loff_t)dev->total_size + off;
        break;

    default:
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    /* Reject seeks before the start or past the end of buffered data */
    if (newpos < 0 || newpos > (loff_t)dev->total_size) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    filp->f_pos = newpos;
    mutex_unlock(&dev->lock);
    return newpos;
}

/* ---------- write ---------- */
/*
 * Fix 4: Move all local variable declarations to the top of the function.
 *
 * The original code interleaved declarations with statements (e.g.
 * "size_t new_size = dev->partial_size + count;" after mutex_lock(), and
 * "char *new_buf = krealloc(...);" inside an if block).  While C99 and
 * linux's -std=gnu11 compiler flag permit this, the kernel coding style
 * guide explicitly requires variables to be declared at the beginning of
 * a block, before any statements.  Mixed declarations make it harder to
 * audit the error-handling goto paths because you cannot see all variables
 * on one screen.
 *
 * All variables are now declared at the top of the function; initialisers
 * that depended on runtime values are separated into assignment statements
 * below.
 *
 * Fix 5: Use explicit (ssize_t) cast when assigning count to retval.
 * retval is ssize_t (signed), count is size_t (unsigned).  The implicit
 * narrowing conversion is safe for any count that passed the
 * AESDCHAR_MAX_WRITE_SIZE guard (which must fit in ssize_t), but an
 * explicit cast documents the intent and silences potential -Wsign-conversion
 * warnings.
 */
ssize_t aesd_write(struct file *filp,
                   const char __user *buf,
                   size_t count,
                   loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    ssize_t retval;
    size_t i;
    int error = 0;
    char **lines = NULL;
    size_t *line_lengths = NULL;
    size_t num_lines = 0;
    size_t line_idx;
    size_t new_size;
    size_t new_cap;
    char *new_buf;
    size_t line_start;

    if (count > AESDCHAR_MAX_WRITE_SIZE)
        return -ENOMEM;

    /* Fix 5: explicit cast – count has already been validated above */
    retval = (ssize_t)count;

    mutex_lock(&dev->lock);

    /* Ensure global accumulation buffer has enough capacity */
    new_size = dev->partial_size + count;
    if (dev->partial_capacity < new_size) {
        new_cap = (dev->partial_capacity == 0)
                  ? count
                  : dev->partial_capacity * 2;
        if (new_cap < new_size)
            new_cap = new_size;
        if (new_cap > AESDCHAR_MAX_WRITE_SIZE)
            new_cap = AESDCHAR_MAX_WRITE_SIZE;
        if (new_cap < new_size) {
            error = -ENOMEM;
            goto out_unlock;
        }
        /*
         * krealloc: if it returns NULL, the original dev->partial_buf pointer
         * is still valid.  Assign only on success so we do not lose the
         * existing buffer on allocation failure.
         */
        new_buf = krealloc(dev->partial_buf, new_cap, GFP_KERNEL);
        if (!new_buf) {
            error = -ENOMEM;
            goto out_unlock;
        }
        dev->partial_buf      = new_buf;
        dev->partial_capacity = new_cap;
    }

    /*
     * Append user data into the accumulation buffer.
     * copy_from_user is called inside the mutex because partial_buf is
     * shared state that must not be modified concurrently.  The copy is
     * bounded by count (already validated) so no overflow is possible.
     */
    if (copy_from_user(dev->partial_buf + dev->partial_size, buf, count)) {
        error = -EFAULT;
        goto out_unlock;
    }
    dev->partial_size += count;

    /* First pass: count complete (newline-terminated) lines */
    for (i = 0; i < dev->partial_size; i++) {
        if (dev->partial_buf[i] == '\n')
            num_lines++;
    }

    if (num_lines == 0)
        goto out_unlock;   /* nothing complete yet; hold in partial_buf */

    /* Allocate parallel arrays for line pointers and their byte lengths */
    lines        = kmalloc_array(num_lines, sizeof(char *), GFP_KERNEL);
    line_lengths = kmalloc_array(num_lines, sizeof(size_t),  GFP_KERNEL);
    if (!lines || !line_lengths) {
        kfree(lines);
        kfree(line_lengths);
        error = -ENOMEM;
        goto out_unlock;
    }

    /* Second pass: extract each newline-terminated line */
    line_start = 0;
    line_idx   = 0;
    for (i = 0; i < dev->partial_size; i++) {
        size_t line_len;
        char *line_buf;

        if (dev->partial_buf[i] != '\n')
            continue;

        line_len = i - line_start + 1;   /* include the '\n' */
        line_buf = kmalloc(line_len, GFP_KERNEL);
        if (!line_buf) {
            /* Free all per-line buffers allocated so far */
            while (line_idx > 0) {
                line_idx--;
                kfree(lines[line_idx]);
            }
            kfree(lines);
            kfree(line_lengths);
            error = -ENOMEM;
            goto out_unlock;
        }

        memcpy(line_buf, dev->partial_buf + line_start, line_len);
        lines[line_idx]        = line_buf;
        line_lengths[line_idx] = line_len;
        line_idx++;
        line_start = i + 1;
    }

    /* All lines successfully allocated – commit them to the circular buffer */
    for (line_idx = 0; line_idx < num_lines; line_idx++) {
        aesd_add_entry_locked(dev, lines[line_idx], line_lengths[line_idx]);
        /* lines[line_idx] is now owned by the circular buffer; do NOT free */
    }

    kfree(lines);
    kfree(line_lengths);

    /* Shift any leftover partial command (no trailing '\n') to the front */
    if (line_start > 0) {
        size_t leftover = dev->partial_size - line_start;
        if (leftover > 0) {
            memmove(dev->partial_buf,
                    dev->partial_buf + line_start,
                    leftover);
        }
        dev->partial_size = leftover;
    }

    /*
     * Do NOT update *f_pos.  Writes to this device are append-only; the
     * circular buffer always adds new data at in_offs regardless of the
     * current read position.  Moving *f_pos on write would confuse
     * concurrent readers that track their own position via f_pos.
     */

out_unlock:
    mutex_unlock(&dev->lock);
    return error ? (ssize_t)error : retval;
}

/*
 * aesd_fops - File operations for the AESD character device.
 *
 * Fix 1 (applied here): Add .compat_ioctl = compat_ptr_ioctl.
 *
 * On kernels 5.9+ (and required on 6.x), the kernel routes ioctl syscalls
 * from 64-bit processes through the compat layer in certain build and
 * runtime configurations.  If .compat_ioctl is absent (NULL), the kernel
 * returns -ENOTTY for every ioctl call, even though .unlocked_ioctl is
 * registered and correct.  compat_ptr_ioctl() is the standard kernel
 * helper that handles the compat-to-native forwarding for drivers whose
 * ioctl arguments contain no embedded user-space pointers (only plain integer
 * or struct fields).  struct aesd_seekto has two uint32_t members and no
 * pointers, so compat_ptr_ioctl is the correct and complete solution.
 *
 * Without this entry, sockettest.sh sends AESDCHAR_IOCSEEKTO commands that
 * aesdsocket forwards as ioctl() calls, the kernel returns ENOTTY, the
 * handle_seekto_command() function in aesdsocket returns -1, and the client
 * receives an empty response — exactly the "but found (empty)" failure seen
 * in the test log.
 */
struct file_operations aesd_fops = {
    .owner          = THIS_MODULE,
    .read           = aesd_read,
    .write          = aesd_write,
    .open           = aesd_open,
    .release        = aesd_release,
    .llseek         = aesd_llseek,
    .unlocked_ioctl = aesd_unlocked_ioctl,
    .compat_ioctl   = compat_ptr_ioctl,   /* Fix 1: required on 6.x kernels */
};

/* ---------- open ---------- */
int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    PDEBUG("open");

    filp->private_data = dev;
    return 0;
}

/* ---------- release ---------- */
int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /*
     * Nothing to free per-open.  The global partial_buf is device-lifetime
     * state managed by aesd_cleanup_module, not per-file state.
     */
    return 0;
}

/* ---------- read ---------- */
ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                  loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    ssize_t retval = 0;
    size_t bytes_copied = 0;
    size_t offset;
    struct aesd_buffer_entry *entry;
    size_t entry_offset;

    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    mutex_lock(&dev->lock);

    /* Nothing to read if the buffer is empty */
    if (!dev->buffer.full && dev->buffer.in_offs == dev->buffer.out_offs)
        goto out;

    offset = (size_t)*f_pos;

    while (count > 0) {
        size_t available;
        size_t to_copy;

        entry = aesd_circular_buffer_find_entry_offset_for_fpos(
                    &dev->buffer, offset, &entry_offset);
        if (!entry)
            break;  /* offset is past the end of buffered data */

        available = entry->size - entry_offset;
        to_copy   = (count < available) ? count : available;

        if (copy_to_user(buf + bytes_copied,
                         entry->buffptr + entry_offset, to_copy)) {
            retval = -EFAULT;
            goto out;
        }

        bytes_copied += to_copy;
        count        -= to_copy;
        offset       += to_copy;
    }

    *f_pos += (loff_t)bytes_copied;
    retval  = (ssize_t)bytes_copied;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

/* ---------- setup cdev ---------- */
static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err;
    int devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    err = cdev_add(&dev->cdev, devno, 1);
    if (err)
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    return err;
}

/* ---------- module init ---------- */
int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;

    result = alloc_chrdev_region(&dev, aesd_minor, 1, "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }

    memset(&aesd_device, 0, sizeof(struct aesd_dev));

    mutex_init(&aesd_device.lock);
    aesd_circular_buffer_init(&aesd_device.buffer);

    aesd_device.partial_buf      = NULL;
    aesd_device.partial_size     = 0;
    aesd_device.partial_capacity = 0;

    result = aesd_setup_cdev(&aesd_device);
    if (result) {
        unregister_chrdev_region(dev, 1);
        mutex_destroy(&aesd_device.lock);
    }

    return result;
}

/* ---------- module cleanup ---------- */
void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    uint8_t index;
    struct aesd_buffer_entry *entry;

    cdev_del(&aesd_device.cdev);

    /* Free all dynamically allocated entry buffers */
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        if (entry->buffptr) {
            kfree(entry->buffptr);
        }
    }

    /* Free any leftover un-committed partial data */
    if (aesd_device.partial_buf)
        kfree(aesd_device.partial_buf);

    mutex_destroy(&aesd_device.lock);
    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
