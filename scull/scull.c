#include <linux/init.h>
#include <linux/module.h>
#include <linux/kdev_t.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>

MODULE_LICENSE("Dual BSD/GPL");

#define SCULL_QUANTUM 4000;
#define SCULL_QSET 1000;

dev_t dev;
int scull_major;
int scull_minor;
int scull_nr_devs = 4;
int scull_quantum = SCULL_QUANTUM;
int scull_qset = SCULL_QSET;

int scull_open(struct inode *inode, struct file *filp);
int scull_release(struct inode *inode, struct file *filp);

ssize_t scull_read(struct file *filp, char __user *buff, size_t count, loff_t *offp);
ssize_t scull_write(struct file *filp, const char __user *buff, size_t count, loff_t *offp);

struct file_operations scull_fops = {
	.owner = THIS_MODULE,
	.read = scull_read,
	.write = scull_write,
	//.llseek = scull_llseek,
	.open = scull_open,
	.release = scull_release,
};

struct scull_dev {
	struct scull_qset *head;  /* Pointer to first quantum set */
	int quantum;              /* the current quantum size */
	int qset;                 /* the current array size */
	unsigned long size;       /* amount of data stored here */
	unsigned int access_key;  /* used by sculluid and scullpriv */
	struct mutex mutex;       /* mutual exclusion now with mutex     */
	struct cdev cdev;         /* Char device structure      */
};

struct scull_qset {
	void **data;
	struct scull_qset *next;
};

struct scull_dev devices[4];


/*
 * Follow the list.
 */
struct scull_qset *scull_follow(struct scull_dev *dev, int n)
{
	struct scull_qset *qs = dev->head;

	if (! qs) {
		qs = dev->head = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
		if (! qs)
			return NULL;
		memset(dev->head, 0, sizeof(struct scull_qset));
	}

	/* Then follow the list. */
	while (n--) {
		if (! qs->next) {
			qs->next = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
			if (! qs->next)
				return NULL;
			memset(qs->next, 0, sizeof(struct scull_qset));
		}
		qs = qs->next;
		continue;
	}
	return qs;
}

static void scull_setup_cdev(struct scull_dev *dev, int index)
{
	int err, devno = MKDEV(scull_major, scull_minor + index);

	cdev_init(&dev->cdev, &scull_fops);

	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &scull_fops;
	mutex_init(&dev->mutex);

	dev->quantum = scull_quantum;
	dev->qset = scull_qset;

	err = cdev_add(&dev->cdev, devno, 1);
	if (err)
		printk(KERN_NOTICE "Error %d adding scull%d", err, index);

}

int scull_trim(struct scull_dev *dev)
{
	struct scull_qset *next, *dptr;
	int qset = dev->qset;
	int i;

	for (dptr = dev->head; dptr; dptr = next) {
		if (dptr->data) {
			for (i = 0; i < qset; i++)
				kfree(dptr->data[i]);
			kfree(dptr->data);
			dptr->data = NULL;
		}
		next = dptr->next;
		kfree(dptr);
	}
	dev->size = 0;
	dev->quantum = scull_quantum;
	dev->qset = scull_qset;
	dev->head = NULL;
	return 0;
}

int scull_open(struct inode *inode, struct file *filp)
{
	struct scull_dev *dev; /* device information */

	dev = container_of(inode->i_cdev, struct scull_dev, cdev);
	filp->private_data = dev;

	printk(KERN_INFO "stored pointer %p in filp->private_data", filp->private_data);

	printk(KERN_INFO "scull: Opening devices minor %d", MINOR(inode->i_rdev));

	if(filp->f_flags & O_APPEND) {
		printk(KERN_INFO "scull: Append detected");
	}
	/* now trim to 0 the length of the device if open was write-only */
	else if ( (filp->f_flags & O_ACCMODE) == O_WRONLY) {
		printk(KERN_INFO "scull: Trimming File to 0 length");
		scull_trim(dev); /* ignore errors */
	}
	return 0;
}

int scull_release(struct inode *inode, struct file *filp)
{
	printk(KERN_INFO "scull: Release devices minor %d", MINOR(inode->i_rdev));
	return 0;
}

ssize_t scull_read(struct file *filp, char __user *buff, size_t count, loff_t *f_pos)
{

	struct scull_dev *dev = filp->private_data;
	struct scull_qset *dptr;    /* the first listitem */
	int quantum = dev->quantum, qset = dev->qset;
	int itemsize = quantum * qset; /* how many bytes in the listitem */
	int item, s_pos, q_pos, rest;
	ssize_t retval = 0;

	printk(KERN_INFO "scull: Read from devices size: %lu", count);
	printk(KERN_INFO "scull: dev *: %p",dev);
	printk(KERN_INFO "scull: dev->mutex *: %p",&dev->mutex);


	if (mutex_lock_interruptible(&dev->mutex))
		return -ERESTARTSYS;

	printk(KERN_INFO "scull: mutex lock success");

	if (*f_pos >= dev->size)
		goto out;
	if (*f_pos + count > dev->size)
		count = dev->size - *f_pos;

	item = (long)*f_pos / itemsize;
	rest = (long)*f_pos % itemsize;
	s_pos = rest / quantum; q_pos = rest % quantum;
	dptr = scull_follow(dev, item);
	if (dptr == NULL || !dptr->data || ! dptr->data[s_pos])
		goto out; /* don't fill holes */

	/* read only up to the end of this quantum */
	if (count > quantum - q_pos)
		count = quantum - q_pos;

	if (copy_to_user(buff, dptr->data[s_pos] + q_pos, count)) {
		retval = -EFAULT;
		goto out;
	}
	*f_pos += count;
	retval = count;

out:
	mutex_unlock(&dev->mutex);
	return retval;
}

ssize_t scull_write(struct file *filp, const char __user *buff, size_t count, loff_t *f_pos)
{
	struct scull_dev *dev = filp->private_data;
	struct scull_qset *dptr;    /* the first listitem */
	int quantum = dev->quantum, qset = dev->qset;
	int itemsize = quantum * qset; /* how many bytes in the listitem */
	int item, s_pos, q_pos, rest;
	ssize_t retval = -ENOMEM; /* value used in "goto out" statements */

	printk(KERN_INFO "scull: Write to devices size: %lu", count);
	printk(KERN_INFO "scull: Write to offset %p", f_pos);

	if (mutex_lock_interruptible(&dev->mutex))
		return -ERESTARTSYS;

	if((filp->f_flags & O_APPEND) == O_APPEND)
		*f_pos = dev->size;

	item = (long)*f_pos / itemsize;
	rest = (long)*f_pos % itemsize;
	s_pos = rest / quantum; q_pos = rest % quantum;

	dptr = scull_follow(dev, item);
	if (dptr == NULL)
		goto out;
	if (!dptr->data) {
		dptr->data = kmalloc(qset * sizeof(char *), GFP_KERNEL);
		if(!dptr->data)
			goto out;
		memset(dptr->data, 0, qset * sizeof(char *));
	}
	if (!dptr->data[s_pos]) {
		dptr->data[s_pos] = kmalloc(quantum, GFP_KERNEL);
		if (!dptr->data[s_pos])
			goto out;
	}

	/* write only up to the end of this quantum */
	if (count > quantum - q_pos)
		count = quantum - q_pos;

	if (copy_from_user(dptr->data[s_pos] + q_pos, buff, count)) {
		retval = -EFAULT;
		goto out;
	}
	*f_pos += count;
	retval = count;

	/* update the size */
	if (dev->size < *f_pos)
		dev->size = *f_pos;

out:
	mutex_unlock(&dev->mutex);
	return retval;
}

static int __init scull_init(void)
{
	int result;
	int i;
	if (scull_major) {
		dev = MKDEV(scull_major, scull_minor);
		result = register_chrdev_region(dev, scull_nr_devs, "scull");
	} else {
		result = alloc_chrdev_region(&dev, scull_minor, scull_nr_devs,
			"scull");
		scull_major = MAJOR(dev);
	}
	if (result < 0) {
		printk(KERN_WARNING "scull: can't get major %d\n", scull_major);
		return result;
	}

	printk(KERN_INFO "scull: major: %d, minor: %d", MAJOR(dev), MINOR(dev));


	for (i=0; i < scull_nr_devs; ++i) {
		scull_setup_cdev(&devices[i], i);
		printk(KERN_INFO "registered device %d", i);
	}

	printk(KERN_ALERT "Char driver registered\n");
	return 0;
}

static void __exit scull_exit(void)
{
	int i;
	for (i=0; i < scull_nr_devs; ++i) {
		cdev_del(&devices[i].cdev);
		scull_trim(&devices[i]);
		printk(KERN_INFO "scull: deregistered device %d", i);
	}
	unregister_chrdev_region(dev, scull_nr_devs);
	printk(KERN_ALERT "Char driver deregistered\n");

}

module_init(scull_init);
module_exit(scull_exit);
