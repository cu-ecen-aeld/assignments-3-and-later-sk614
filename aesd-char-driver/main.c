/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include "aesdchar.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Linh Dang"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev;

    PDEBUG("open");
    /**
     * TODO: handle open
     */
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset_byte = 0;
    size_t bytes_to_copy;

    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle read
     */
    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    entry = aesd_circular_buffer_find_entry_offset_for_fpos(
            &dev->buffer, (size_t)*f_pos, &entry_offset_byte);

    if (entry != NULL) {
        bytes_to_copy = entry->size - entry_offset_byte;
        if (bytes_to_copy > count) {
            bytes_to_copy = count;
        }

        if (copy_to_user(buf, entry->buffptr + entry_offset_byte,
                         bytes_to_copy)) {
            retval = -EFAULT;
        } else {
            *f_pos += bytes_to_copy;
            retval = bytes_to_copy;
        }
    }

    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    struct aesd_dev *dev = filp->private_data;
    char *newbuf = NULL;
    char *command = NULL;
    size_t new_size;
    size_t start = 0;
    size_t index;
    struct aesd_buffer_entry add_entry;

    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle write
     */
    if (count == 0) {
        return 0;
    }

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    new_size = dev->pending_size + count;
    newbuf = kmalloc(new_size, GFP_KERNEL);
    if (newbuf == NULL) {
        goto out;
    }

    if (dev->pending_size > 0) {
        memcpy(newbuf, dev->pending_buf, dev->pending_size);
    }

    if (copy_from_user(newbuf + dev->pending_size, buf, count)) {
        retval = -EFAULT;
        goto out;
    }

    kfree(dev->pending_buf);
    dev->pending_buf = NULL;
    dev->pending_size = 0;

    for (index = 0; index < new_size; index++) {
        if (newbuf[index] == '\n') {
            size_t command_size = index - start + 1;

            command = kmalloc(command_size, GFP_KERNEL);
            if (command == NULL) {
                retval = -ENOMEM;
                goto out;
            }

            memcpy(command, newbuf + start, command_size);

            if (dev->buffer.full) {
                kfree(dev->buffer.entry[dev->buffer.in_offs].buffptr);
            }

            add_entry.buffptr = command;
            add_entry.size = command_size;
            aesd_circular_buffer_add_entry(&dev->buffer, &add_entry);

            command = NULL;
            start = index + 1;
        }
    }

    if (start < new_size) {
        dev->pending_size = new_size - start;
        dev->pending_buf = kmalloc(dev->pending_size, GFP_KERNEL);
        if (dev->pending_buf == NULL) {
            dev->pending_size = 0;
            retval = -ENOMEM;
            goto out;
        }

        memcpy(dev->pending_buf, newbuf + start, dev->pending_size);
    }

    retval = count;

out:
    kfree(command);
    kfree(newbuf);
    mutex_unlock(&dev->lock);
    return retval;
}
struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */
    aesd_circular_buffer_init(&aesd_device.buffer);
    mutex_init(&aesd_device.lock);
    aesd_device.pending_buf = NULL;
    aesd_device.pending_size = 0;

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */
    {
        uint8_t index;
        struct aesd_buffer_entry *entry;

        kfree(aesd_device.pending_buf);
        aesd_device.pending_buf = NULL;
        aesd_device.pending_size = 0;

        AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
            if (entry->buffptr != NULL) {
                kfree(entry->buffptr);
                entry->buffptr = NULL;
                entry->size = 0;
            }
        }
    }

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
