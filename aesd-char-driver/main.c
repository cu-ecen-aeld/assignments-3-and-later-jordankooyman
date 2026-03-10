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

/* ---------- Helper: adjust file offset (must be called with dev->lock held) ---------- */
static long aesd_adjust_file_offset_locked(struct file *filp,
                                           unsigned int write_cmd,
                                           unsigned int write_cmd_offset)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_circular_buffer *buf = &dev->buffer;
    struct aesd_buffer_entry *entry;
    uint8_t idx;
    unsigned int cmd_seen = 0;
    unsigned int num_entries = 0;
    loff_t abs_offset = 0;

    /* Count how many valid entries are currently in the buffer */
    AESD_CIRCULAR_BUFFER_FOREACH(entry, buf, idx) {
        if (entry->buffptr)
            num_entries++;
    }

    if (write_cmd >= num_entries)
        return -EINVAL;

    /* Traverse entries in order from oldest (out_offs) to newest */
    for (idx = 0; idx < AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED; idx++) {
        entry = &buf->entry[(buf->out_offs + idx) %
                             AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED];
        if (!entry->buffptr)
            continue;   /* skip unused slots (should not happen in counted range) */

        if (cmd_seen == write_cmd) {
            if (write_cmd_offset >= entry->size)
                return -EINVAL;
            abs_offset += write_cmd_offset;
            filp->f_pos = abs_offset;
            return 0;
        }

        abs_offset += entry->size;
        cmd_seen++;
        if (cmd_seen >= num_entries)
            break;
    }

    /* Should never reach here if validation passed */
    return -EINVAL;
}

/* ---------- Circular buffer helper with total_size update ---------- */
static void aesd_add_entry_locked(struct aesd_dev *dev, const char *line, size_t size)
{
    /* If the buffer is full, free the entry that will be overwritten */
    if (dev->buffer.full) {
        struct aesd_buffer_entry *old = &dev->buffer.entry[dev->buffer.in_offs];
        if (old->buffptr) {
            dev->total_size -= old->size;   /* subtract size of overwritten entry */
            kfree(old->buffptr);
            old->buffptr = NULL;
            old->size = 0;
        }
    }

    struct aesd_buffer_entry new_entry = {
        .buffptr = line,
        .size = size
    };
    aesd_circular_buffer_add_entry(&dev->buffer, &new_entry);
    dev->total_size += size;   /* add size of new entry */
}

/* ---------- unlocked_ioctl ---------- */
long aesd_unlocked_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto seekto;
    long ret;

    /* Check magic number */
    if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC)
        return -ENOTTY;

    switch (cmd) {
    case AESDCHAR_IOCSEEKTO:
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
        /* Check for overflow when adding to current position */
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
        if (off > 0 && dev->total_size > LLONG_MAX - off) {
            mutex_unlock(&dev->lock);
            return -EINVAL;
        }
        if (off < 0 && dev->total_size < -off) {
            mutex_unlock(&dev->lock);
            return -EINVAL;
        }
        newpos = dev->total_size + off;
        break;
    default:
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    if (newpos < 0 || newpos > dev->total_size) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    filp->f_pos = newpos;
    mutex_unlock(&dev->lock);
    return newpos;
}

/* ---------- write ---------- */
ssize_t aesd_write(struct file *filp,
                   const char __user *buf,
                   size_t count,
                   loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    ssize_t retval = count;
    size_t i;
    int error = 0;
    char **lines = NULL;
    size_t *line_lengths = NULL;   /* parallel array of lengths */
    size_t num_lines = 0;
    size_t line_idx;

    if (count > AESDCHAR_MAX_WRITE_SIZE)
        return -ENOMEM;

    mutex_lock(&dev->lock);

    /* Ensure global accumulation buffer has enough capacity */
    size_t new_size = dev->partial_size + count;
    if (dev->partial_capacity < new_size) {
        size_t new_cap = max(dev->partial_capacity * 2, new_size);
        if (new_cap > AESDCHAR_MAX_WRITE_SIZE)
            new_cap = AESDCHAR_MAX_WRITE_SIZE;
        if (new_cap < new_size) {
            error = -ENOMEM;
            goto out_unlock;
        }
        char *new_buf = krealloc(dev->partial_buf, new_cap, GFP_KERNEL);
        if (!new_buf) {
            error = -ENOMEM;
            goto out_unlock;
        }
        dev->partial_buf = new_buf;
        dev->partial_capacity = new_cap;
    }

    /* Append user data (copy_from_user before any allocations) */
    if (copy_from_user(dev->partial_buf + dev->partial_size, buf, count)) {
        error = -EFAULT;
        goto out_unlock;
    }
    dev->partial_size += count;

    /* First pass: count complete lines */
    for (i = 0; i < dev->partial_size; i++) {
        if (dev->partial_buf[i] == '\n')
            num_lines++;
    }

    if (num_lines == 0)
        goto out_unlock;   /* nothing complete yet */

    /* Allocate arrays for line pointers and their lengths */
    lines = kmalloc_array(num_lines, sizeof(char *), GFP_KERNEL);
    line_lengths = kmalloc_array(num_lines, sizeof(size_t), GFP_KERNEL);
    if (!lines || !line_lengths) {
        kfree(lines);
        kfree(line_lengths);
        error = -ENOMEM;
        goto out_unlock;
    }

    /* Second pass: extract each line and record its length */
    size_t line_start = 0;
    line_idx = 0;
    for (i = 0; i < dev->partial_size; i++) {
        if (dev->partial_buf[i] != '\n')
            continue;

        size_t line_len = i - line_start + 1;   /* include newline */
        char *line_buf = kmalloc(line_len, GFP_KERNEL);
        if (!line_buf) {
            /* Allocation failed: free everything allocated so far */
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
        lines[line_idx] = line_buf;
        line_lengths[line_idx] = line_len;
        line_idx++;
        line_start = i + 1;
    }

    /* All lines successfully allocated – add them to the circular buffer */
    for (line_idx = 0; line_idx < num_lines; line_idx++) {
        aesd_add_entry_locked(dev, lines[line_idx], line_lengths[line_idx]);
        /* Note: lines[line_idx] is now owned by the circular buffer */
    }

    kfree(lines);
    kfree(line_lengths);

    /* Preserve leftover partial command (if any) */
    if (line_start > 0) {
        size_t leftover = dev->partial_size - line_start;
        if (leftover > 0) {
            memmove(dev->partial_buf,
                    dev->partial_buf + line_start,
                    leftover);
        }
        dev->partial_size = leftover;
    }

    /* Do NOT update f_pos – writes are append‑only but should not move
     * the read/write pointer; the position remains unchanged. */

out_unlock:
    mutex_unlock(&dev->lock);
    return error ? error : retval;
}

/* ---------- file operations structure ---------- */
struct file_operations aesd_fops = {
    .owner          = THIS_MODULE,
    .read           = aesd_read,
    .write          = aesd_write,
    .open           = aesd_open,
    .release        = aesd_release,
    .llseek         = aesd_llseek,
    .unlocked_ioctl = aesd_unlocked_ioctl,
};

/* ---------- open ---------- */
int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    PDEBUG("open");

    /* Store the device pointer directly in private_data */
    filp->private_data = dev;
    return 0;
}

/* ---------- release ---------- */
int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /* Nothing to free – the global partial buffer is managed by the device */
    return 0;
}

/* ---------- read ---------- */
ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                  loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    ssize_t retval = 0;
    size_t bytes_copied = 0;
    size_t offset = *f_pos;
    struct aesd_buffer_entry *entry;
    size_t entry_offset;

    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    mutex_lock(&dev->lock);

    /* Empty if not full and in/out equal */
    if (!dev->buffer.full && dev->buffer.in_offs == dev->buffer.out_offs)
        goto out;

    while (count > 0) {
        entry = aesd_circular_buffer_find_entry_offset_for_fpos(
                    &dev->buffer, offset, &entry_offset);
        if (!entry)
            break;  /* No more data */

        size_t available = entry->size - entry_offset;
        size_t to_copy = (count < available) ? count : available;

        if (copy_to_user(buf + bytes_copied,
                         entry->buffptr + entry_offset, to_copy)) {
            retval = -EFAULT;
            goto out;
        }

        bytes_copied += to_copy;
        count -= to_copy;
        offset += to_copy;
    }

    *f_pos += bytes_copied;
    retval = bytes_copied;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

/* ---------- setup cdev ---------- */
static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

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

    /* Initialize global partial‑line buffer fields */
    aesd_device.partial_buf = NULL;
    aesd_device.partial_size = 0;
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

    /* Free any leftover partial data */
    if (aesd_device.partial_buf)
        kfree(aesd_device.partial_buf);

    mutex_destroy(&aesd_device.lock);
    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
