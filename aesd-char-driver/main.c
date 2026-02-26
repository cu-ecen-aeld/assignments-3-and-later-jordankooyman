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
 *	Code Comparison: https://chatgpt.com/share/69a0abe9-1df4-8007-b644-419269c81357
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
    struct aesd_file_private *priv;

    PDEBUG("open");

    priv = kmalloc(sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    priv->dev = dev;
    priv->partial_buf = NULL;
    priv->partial_size = 0;
    priv->partial_capacity = 0;

    filp->private_data = priv;
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    struct aesd_file_private *priv = filp->private_data;
    PDEBUG("release");

    if (priv) {
        if (priv->partial_buf)
            kfree(priv->partial_buf);
        kfree(priv);
    }
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                  loff_t *f_pos)
{
    struct aesd_file_private *priv = filp->private_data;
    struct aesd_dev *dev = priv->dev;
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

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                   loff_t *f_pos)
{
    struct aesd_file_private *priv = filp->private_data;
    struct aesd_dev *dev = priv->dev;
    ssize_t retval = count;   /* We'll return count on success */
    size_t new_size;
    size_t processed = 0;
    size_t i;
    int error = 0;

    PDEBUG("write %zu bytes with offset %lld", count, *f_pos);

    /* Reject writes that exceed the maximum allowed size */
    if (count > AESDCHAR_MAX_WRITE_SIZE)
        return -ENOMEM;

    mutex_lock(&dev->lock);   /* Serialise all writes */

    new_size = priv->partial_size + count;

    /* Ensure the per-file buffer is large enough */
    if (priv->partial_capacity < new_size) {
        size_t new_cap = max(priv->partial_capacity * 2, new_size);
        if (new_cap > AESDCHAR_MAX_WRITE_SIZE)
            new_cap = AESDCHAR_MAX_WRITE_SIZE;
        if (new_cap < new_size) {
            error = -ENOMEM;
            goto out_unlock;
        }

        char *new_buf = krealloc(priv->partial_buf, new_cap, GFP_KERNEL);
        if (!new_buf) {
            error = -ENOMEM;
            goto out_unlock;
        }
        priv->partial_buf = new_buf;
        priv->partial_capacity = new_cap;
    }

    /* Copy new data from user space */
    if (copy_from_user(priv->partial_buf + priv->partial_size, buf, count)) {
        error = -EFAULT;
        goto out_unlock;
    }
    priv->partial_size = new_size;

    /* Scan the entire buffer for complete lines (terminated by '\n') */
    processed = 0;
    for (i = 0; i < priv->partial_size; i++) {
        if (priv->partial_buf[i] == '\n') {
            size_t line_len = i - processed + 1;
            char *line_buf = kmalloc(line_len, GFP_KERNEL);
            if (!line_buf) {
                error = -ENOMEM;
                /* Stop processing; lines already added remain in the buffer */
                break;
            }
            memcpy(line_buf, priv->partial_buf + processed, line_len);

            /* Add the line to the circular buffer (lock already held) */
            aesd_add_entry_locked(dev, line_buf, line_len);

            processed = i + 1;   /* Move past this line */
        }
    }

    /* Any remaining data becomes the new partial buffer */
    if (processed < priv->partial_size) {
        size_t leftover = priv->partial_size - processed;
        memmove(priv->partial_buf, priv->partial_buf + processed, leftover);
        priv->partial_size = leftover;
    } else {
        /* All data was consumed; free the buffer to save memory */
        kfree(priv->partial_buf);
        priv->partial_buf = NULL;
        priv->partial_size = 0;
        priv->partial_capacity = 0;
    }

out_unlock:
    mutex_unlock(&dev->lock);
    return error ? error : retval;
}

struct file_operations aesd_fops = {
    .owner   = THIS_MODULE,
    .read    = aesd_read,
    .write   = aesd_write,
    .open    = aesd_open,
    .release = aesd_release,
    .llseek  = no_llseek,   /* No seeking support */
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

    mutex_destroy(&aesd_device.lock);
    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
