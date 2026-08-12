/*
 * aesdchar.h
 *
 *  Created on: Oct 23, 2019
 *      Author: Dan Walkes
 */
 
#include <linux/cdev.h>
#include <linux/mutex.h>
#include "aesd-circular-buffer.h"

#ifndef AESD_CHAR_DRIVER_AESDCHAR_H_
#define AESD_CHAR_DRIVER_AESDCHAR_H_

#define AESD_DEBUG 1  //Remove comment on this line to enable debug

#undef PDEBUG             /* undef it, just in case */
#ifdef AESD_DEBUG
#  ifdef __KERNEL__
     /* This one if debugging is on, and kernel space */
#    define PDEBUG(fmt, args...) printk( KERN_DEBUG "aesdchar: " fmt, ## args)
#  else
     /* This one for user space */
#    define PDEBUG(fmt, args...) fprintf(stderr, fmt, ## args)
#  endif
#else
#  define PDEBUG(fmt, args...) /* not debugging: nothing */
#endif

struct aesd_dev
{
    struct cdev cdev;

    /* Stores the most recent 10 completed write commands */
    struct aesd_circular_buffer buffer;

    /* Protects reads/writes and circular buffer manipulation */
    struct mutex lock;

    /*
     * Holds a write which has not yet been terminated with '\n'.
     * Future writes are appended here until the command is complete.
     */
    char *pending_buf;
    size_t pending_size;
};


#endif /* AESD_CHAR_DRIVER_AESDCHAR_H_ */
