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

int aesd_major = 0;   /* dynamic major */
int aesd_minor = 0;

MODULE_AUTHOR("Jordan Kooyman");
MODULE_LICENSE("Dual BSD/GPL");

int aesd_open(struct inode *, struct file *);
int aesd_release(struct inode *, struct file *);
ssize_t aesd_read(struct file *, char __user *, size_t, loff_t *);
ssize_t aesd_write(struct file *, const char __user *, size_t, loff_t *);
loff_t aesd_llseek(struct file *, loff_t, int);
int aesd_init_module(void);
void aesd_cleanup_module(void);

struct aesd_dev aesd_device;

/**
 * aesd_add_entry_locked() - Add a completed line to the circular buffer.
 * @dev:  Device structure (with lock already held)
 * @line: Pointer to the line buffer (must be kmalloc'd)
 * @size: Length of the line (including newline)
 *
 * If the circular buffer is full, the oldest entry is freed before adding.
 * The caller must hold @dev->lock.
 */
static void aesd_add_entry_locked(struct aesd_dev *dev, const char *line, size_t size)
{
    /* If the buffer is full, free the entry that will be overwritten */
    if (dev->buffer.full) {
        struct aesd_buffer_entry *old = &dev->buffer.entry[dev->buffer.in_offs];
        if (old->buffptr) {
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
}

int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    PDEBUG("open");

    /* Store the device pointer directly in private_data */
    filp->private_data = dev;
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /* Nothing to free – the global partial buffer is managed by the device */
    return 0;
}

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

ssize_t aesd_write(struct file *filp,
                   const char __user *buf,
                   size_t count,
                   loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    ssize_t retval = count;
    size_t i;
    int error = 0;

    if (count > AESDCHAR_MAX_WRITE_SIZE)
        return -ENOMEM;

    mutex_lock(&dev->lock);

    /* Ensure global accumulation buffer capacity */
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

    /* Append user data */
    if (copy_from_user(dev->partial_buf + dev->partial_size, buf, count)) {
        error = -EFAULT;
        goto out_unlock;
    }

    dev->partial_size += count;

    /*
     * Scan for complete newline-terminated commands.
     * IMPORTANT: Keep any leftover data for the next write.
     */
    size_t line_start = 0;

    for (i = 0; i < dev->partial_size; i++) {

        if (dev->partial_buf[i] != '\n')
            continue;

        size_t line_len = i - line_start + 1;

        char *line_buf = kmalloc(line_len, GFP_KERNEL);
        if (!line_buf) {
            error = -ENOMEM;
            goto out_unlock;
        }

        memcpy(line_buf,
               dev->partial_buf + line_start,
               line_len);

        aesd_add_entry_locked(dev, line_buf, line_len);

        line_start = i + 1;
    }

    /*
     * Preserve leftover partial command (if any)
     * by shifting it to the start of the buffer.
     */
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
     * DO NOT free partial_buf when empty.
     * Keep allocation for future writes to avoid churn.
     */

out_unlock:
    mutex_unlock(&dev->lock);
    return error ? error : retval;
}

loff_t aesd_llseek(struct file *filp, loff_t off, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t newpos = 0;
    size_t total_size = 0;
    uint8_t index;
    struct aesd_buffer_entry *entry;

    mutex_lock(&dev->lock);

    /* Compute total size of stored data */
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->buffer, index) {
        total_size += entry->size;
    }

    switch (whence) {
    case SEEK_SET:
        newpos = off;
        break;

    case SEEK_CUR:
        newpos = filp->f_pos + off;
        break;

    case SEEK_END:
        newpos = total_size + off;
        break;

    default:
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    if (newpos < 0 || newpos > total_size) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    filp->f_pos = newpos;

    mutex_unlock(&dev->lock);
    return newpos;
}

struct file_operations aesd_fops = {
    .owner   = THIS_MODULE,
    .read    = aesd_read,
    .write   = aesd_write,
    .open    = aesd_open,
    .release = aesd_release,
    .llseek  = aesd_llseek
};

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
