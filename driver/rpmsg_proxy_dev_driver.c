/*
 * RPMSG Proxy Device Kernel Driver
 *
 * Copyright (C) 2014 Mentor Graphics Corporation
 * Copyright (C) 2015 Xilinx, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/* 
 *   Copyright (C) 2019 ZHT, Inc.
 *   Author: mr_xiaogui@163.com
 *   Date : 2019/05/07
 *   Version: V1.1.01
 *   Note :
 *    	1. Add ioctl to send control msg for RPU
 */

 

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/fs.h>
#include <linux/kfifo.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/ioctl.h>
#include <linux/errno.h>
#include <linux/poll.h>
#include <linux/atomic.h>
#include <linux/skbuff.h>
#include <linux/idr.h>


// Define magic for RPU Unit
#define RPU_IOC_MAGIC '$'
// for params set which using pointer
#define RPU_IOC_SET   _IOW(RPU_IOC_MAGIC, 0, int)
// for params get which using pointer
#define RPU_IOC_GET   _IOW(RPU_IOC_MAGIC, 1, int)
// The Max cmd number for ioctl, you should modify it if you add new cmd
#define RPU_IOC_MAXNR 1


#define MAX_RPMSG_BUFF_SIZE		512

/* Shutdown message ID */
#define SHUTDOWN_MSG			0xEF56A55A
#define TERM_SYSCALL_ID			6
#define LOGGER_ENDPOINT			127
#define CONTROL_ENDPOINT			121
#define RPMSG_USER_DEV_MAX_MINORS 10
#define RPMG_INIT_MSG "init_msg"

struct rpu_cmd{
	int cmd_id;
	char data[256];
};

struct _rpmsg_eptdev {
	struct device dev;
	struct cdev cdev;
	wait_queue_head_t usr_wait_q;
	struct rpmsg_device *rpdev;
	struct rpmsg_channel_info chinfo;
	struct rpmsg_endpoint *ept;
	spinlock_t queue_lock;
	struct sk_buff_head queue;
	bool is_sk_queue_closed;
	wait_queue_head_t readq;
	struct rpmsg_endpoint *control_ept;	// for control
	wait_queue_head_t ioctlq;			// for ioctl get_cmd
	struct rpu_cmd cb_get_cmd;		//  for ioctl get_cmd
	volatile int ioctlq_event;			// for ioctl get_cmd
	spinlock_t ioctl_queue_lock;
	struct sk_buff_head ioctl_queue;
	bool is_ioctl_sk_queue_closed;
};




static struct class *rpmsg_class;
static dev_t rpmsg_dev_major;
static DEFINE_IDA(rpmsg_minor_ida);



struct rpmsg_channel_info logger_rpmsg_chn = {
	"rpu-rpmsg-logger-chn",
	LOGGER_ENDPOINT,
	RPMSG_ADDR_ANY,
};

struct rpmsg_channel_info control_rpmsg_chn = {
	"rpu-rpmsg-control-chn",
	CONTROL_ENDPOINT,
	RPMSG_ADDR_ANY,
};




#define dev_to_eptdev(dev) container_of(dev, struct _rpmsg_eptdev, dev)
#define cdev_to_eptdev(i_cdev) container_of(i_cdev, struct _rpmsg_eptdev, cdev)

static int rpmsg_proxy_dev_rpmsg_drv_cb(struct rpmsg_device *rpdev, void *data,
					int len, void *priv, u32 src)
{

	struct _rpmsg_eptdev *local = dev_get_drvdata(&rpdev->dev);
	struct sk_buff *skb;

	skb = alloc_skb(len, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;

	memcpy(skb_put(skb, len), data, len);

	spin_lock(&local->queue_lock);
	if (local->is_sk_queue_closed) {
		kfree(skb);
		spin_unlock(&local->queue_lock);
		return 0;
	}
	skb_queue_tail(&local->queue, skb);
	spin_unlock(&local->queue_lock);

	/* wake up any blocking processes, waiting for new data */
	wake_up_interruptible(&local->readq);

	return 0;
}



static int control_rpmsg_cb(struct rpmsg_device *rpdev, void *data,
					int len, void *priv, u32 src)
{

	struct _rpmsg_eptdev *local = dev_get_drvdata(&rpdev->dev);
	struct rpu_cmd *rpu_cmd = (struct rpu_cmd *)data;
	struct sk_buff *skb;

	skb = alloc_skb(len, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;

	memcpy(skb_put(skb, len), data, len);

	spin_lock(&local->ioctl_queue_lock);
	if (local->is_ioctl_sk_queue_closed) {
		kfree(skb);
		spin_unlock(&local->ioctl_queue_lock);
		return 0;
	}
	skb_queue_tail(&local->ioctl_queue, skb);
	spin_unlock(&local->ioctl_queue_lock);

	local->ioctlq_event = 1;
	wake_up_interruptible( &local->ioctlq); 

	return 0;
}



static int rpmsg_dev_open(struct inode *inode, struct file *filp)
{
	/* Initialize rpmsg instance with device params from inode */
	struct _rpmsg_eptdev *local = cdev_to_eptdev(inode->i_cdev);
	struct rpmsg_device *rpdev = local->rpdev;
	unsigned long flags;
	int  retval = 0;
	struct rpmsg_channel_info logger_chinfo;
	struct rpmsg_channel_info control_chinfo;
	filp->private_data = local;

	spin_lock_irqsave(&local->queue_lock, flags);
	local->is_sk_queue_closed = false;
	spin_unlock_irqrestore(&local->queue_lock, flags);


	spin_lock_irqsave(&local->ioctl_queue_lock, flags);
	local->is_ioctl_sk_queue_closed = false;
	spin_unlock_irqrestore(&local->ioctl_queue_lock, flags);


	local->ept = rpmsg_create_ept(rpdev, rpmsg_proxy_dev_rpmsg_drv_cb, local, logger_rpmsg_chn);
	if (!local->ept) {
		dev_err(&rpdev->dev, "Failed to create proxy ept.\n");
		return -ENODEV;
	}

	if (filp->f_flags & O_NONBLOCK)
		retval = rpmsg_trysendto(local->ept, RPMG_INIT_MSG, sizeof(RPMG_INIT_MSG), rpdev->dst);
	else
		retval = rpmsg_sendto(local->ept, RPMG_INIT_MSG, sizeof(RPMG_INIT_MSG), rpdev->dst);

	if (retval) {
		dev_err(&rpdev->dev, "Failed to send init_msg to target 0x%x.", rpdev->dst);
		return -ENODEV;
	}

	
	// add a control endpoint
	local->control_ept = rpmsg_create_ept(rpdev, control_rpmsg_cb, local, control_rpmsg_chn);
	if (!local->control_ept) {
		dev_err(&rpdev->dev, "Failed to create proxy ept.\n");
		return -ENODEV;
	}


	if (filp->f_flags & O_NONBLOCK)
		retval = rpmsg_trysendto(local->control_ept, RPMG_INIT_MSG, sizeof(RPMG_INIT_MSG), rpdev->dst);
	else
		retval = rpmsg_sendto(local->control_ept, RPMG_INIT_MSG, sizeof(RPMG_INIT_MSG), rpdev->dst);

	if (retval) {
		dev_err(&rpdev->dev, "Failed to send init_msg to target 0x%x.", rpdev->dst);
		return -ENODEV;
	}

	// reset event
	local->ioctlq_event = 0;
	
	dev_info(&rpdev->dev, "Sent init_msg to target 0x%x.", rpdev->dst);

	return 0;
}

static ssize_t rpmsg_dev_write(struct file *filp,
				const char __user *ubuff, size_t len,
				loff_t *p_off)
{
	struct _rpmsg_eptdev *local = filp->private_data;
	struct rpmsg_device *rpdev = local->rpdev;
	void *kbuf;
	int ret;

	kbuf = kzalloc(len, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	if (copy_from_user(kbuf, ubuff, len)) {
		ret = -EFAULT;
		goto free_kbuf;
	}

	if (filp->f_flags & O_NONBLOCK)
		ret = rpmsg_trysendto(local->ept, kbuf, len, LOGGER_ENDPOINT);
	else
		ret = rpmsg_sendto(local->ept, kbuf, len, LOGGER_ENDPOINT);
	

free_kbuf:
	kfree(kbuf);
	return ret < 0 ? ret : len;
}

static ssize_t rpmsg_dev_read(struct file *filp, char __user *ubuff,
				size_t len, loff_t *p_off)
{
	struct _rpmsg_eptdev *local = filp->private_data;
	struct sk_buff *skb;
	unsigned long flags;
	int retlen;

	spin_lock_irqsave(&local->queue_lock, flags);

	/* wait for data int he queue */
	if (skb_queue_empty(&local->queue)) {
		spin_unlock_irqrestore(&local->queue_lock, flags);

		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible(local->readq,
				!skb_queue_empty(&local->queue)))
			return -ERESTARTSYS;

		spin_lock_irqsave(&local->queue_lock, flags);
	}

	skb = skb_dequeue(&local->queue);
	if (!skb) {
		dev_err(&local->dev, "Read failed, RPMsg queue is empty.\n");
		return -EFAULT;
	}

	spin_unlock_irqrestore(&local->queue_lock, flags);
	retlen = min_t(size_t, len, skb->len);
	if (copy_to_user(ubuff, skb->data, retlen)) {
		dev_err(&local->dev, "Failed to copy data to user.\n");
		kfree_skb(skb);
		return -EFAULT;
	}

	kfree_skb(skb);
	return retlen;
}

static unsigned int rpmsg_dev_poll(struct file *filp, poll_table *wait)
{
	struct _rpmsg_eptdev *local = filp->private_data;
	unsigned int mask = 0;

	poll_wait(filp, &local->readq, wait);

	if (!skb_queue_empty(&local->queue))
		mask |= POLLIN | POLLRDNORM;

	return mask;
}

static long rpmsg_dev_ioctl(struct file *filp, unsigned int cmd,
				unsigned long arg)
{
	/* Add ioctl supported by xiaogui */
	int  retval = 0;
	struct _rpmsg_eptdev *local = filp->private_data;
	struct rpmsg_device *rpdev = local->rpdev;
	unsigned long flags;
	struct sk_buff *skb;
	struct rpu_cmd tmp_cmd;
	int retlen;

	if (_IOC_TYPE(cmd) != RPU_IOC_MAGIC)
		return -ENOTTY;


	if (_IOC_NR(cmd) > RPU_IOC_MAXNR)
		return -ENOTTY;

	memset(&tmp_cmd, 0, sizeof(tmp_cmd));

	retval = copy_from_user(&tmp_cmd, (char *)arg, sizeof(tmp_cmd));
	if(retval){
		retval = -EFAULT;
		printk("data copy_from_user error\n");
		return retval;
	}

	if (filp->f_flags & O_NONBLOCK)
		retval = rpmsg_trysendto(local->control_ept, &tmp_cmd, sizeof(tmp_cmd), rpdev->dst);
	else
		retval = rpmsg_sendto(local->control_ept, &tmp_cmd, sizeof(tmp_cmd), rpdev->dst);

	if(retval){
		dev_err(&rpdev->dev, "Failed to send cmd to target 0x%x.", rpdev->dst);
		return -ENODEV;
	}

	spin_lock_irqsave(&local->ioctl_queue_lock, flags);
	/* wait for data int he queue */
	if (skb_queue_empty(&local->ioctl_queue)) {
		spin_unlock_irqrestore(&local->ioctl_queue_lock, flags);
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		retval = wait_event_interruptible_timeout(local->ioctlq, !skb_queue_empty(&local->ioctl_queue), msecs_to_jiffies(5000)) ;
		if (retval == 0){
			dev_err(&local->dev, "wait_event_interruptible_timeout.\n");
			return -ERESTARTSYS;
		}
		
		if(retval < 0){
			return retval;
		}

		spin_lock_irqsave(&local->ioctl_queue_lock, flags);
	}

	skb = skb_dequeue(&local->ioctl_queue);
	if (!skb) {
		dev_err(&local->dev, "ioctl RPU_IOC_GET failed, RPMsg queue is empty.\n");
		return -EFAULT;
	}

	spin_unlock_irqrestore(&local->ioctl_queue_lock, flags);
	retlen = min_t(size_t, sizeof(struct rpu_cmd), skb->len);

	switch (cmd) {
		case RPU_IOC_SET://set data 
			break;
		case RPU_IOC_GET://get data 
			retval = copy_to_user((char *)arg, (char *)skb->data, retlen);
			if(retval){
				retval = -EFAULT;
				break;
			}
			local->ioctlq_event = 0;		// reset event
			break;
		default:
			retval = -EINVAL;
            		break;
    	}

	kfree_skb(skb);
	return retval;
}

static int rpmsg_dev_release(struct inode *inode, struct file *p_file)
{
	struct _rpmsg_eptdev *eptdev = cdev_to_eptdev(inode->i_cdev);
	struct rpmsg_device *rpdev = eptdev->rpdev;
	struct sk_buff *skb;
	int msg = TERM_SYSCALL_ID;

	spin_lock(&eptdev->queue_lock);
	eptdev->is_sk_queue_closed = true;
	spin_unlock(&eptdev->queue_lock);

	spin_lock(&eptdev->ioctl_queue_lock);
	eptdev->is_ioctl_sk_queue_closed = true;
	spin_unlock(&eptdev->ioctl_queue_lock);


	/* Delete the skb buffers */
	while(!skb_queue_empty(&eptdev->queue)) {
		skb = skb_dequeue(&eptdev->queue);
		kfree_skb(skb);
	}

	while(!skb_queue_empty(&eptdev->ioctl_queue)) {
		skb = skb_dequeue(&eptdev->ioctl_queue);
		kfree_skb(skb);
	}
	
	/*
	dev_info(&rpdev->dev, "Sending terminate message.\n");
	if (rpmsg_send(eptdev->ept,&msg,sizeof(msg))) {
		dev_err(&rpdev->dev,"Failed to send terminate message.\n");
		return -EINVAL;
	}
	*/

	/* Destroy the proxy endpoint */
	printk("rpmsg_destroy_ept [eptdev->ept] \n");
	rpmsg_destroy_ept(eptdev->ept);
	printk("rpmsg_destroy_ept [eptdev->control_ept] \n");
	rpmsg_destroy_ept(eptdev->control_ept);

	put_device(&rpdev->dev);
	return 0;
}

static const struct file_operations rpmsg_dev_fops = {
	.owner = THIS_MODULE,
	.read = rpmsg_dev_read,
	.poll = rpmsg_dev_poll,
	.write = rpmsg_dev_write,
	.open = rpmsg_dev_open,
	.unlocked_ioctl = rpmsg_dev_ioctl,
	.release = rpmsg_dev_release,
};

static void rpmsg_proxy_dev_release_device(struct device *dev)
{
	struct _rpmsg_eptdev *eptdev = dev_to_eptdev(dev);

	dev_info(dev, "Releasing rpmsg proxy dev device.\n");
	ida_simple_remove(&rpmsg_minor_ida, dev->id);
	cdev_del(&eptdev->cdev);
	/* No need to free the local dev memory eptdev.
	 * It will be freed by the system when the dev is freed
	 */
}

static int rpmsg_proxy_dev_rpmsg_drv_probe(struct rpmsg_device *rpdev)
{
	struct _rpmsg_eptdev *local;
	struct device *dev;
	int ret;

	dev_info(&rpdev->dev, "%s\n", __func__);

	local = devm_kzalloc(&rpdev->dev, sizeof(struct _rpmsg_eptdev), GFP_KERNEL);
	if (!local)
		return -ENOMEM;

	/* Initialize locks */
	spin_lock_init(&local->queue_lock);
	spin_lock_init(&local->ioctl_queue_lock);

	/* Initialize sk_buff queue */
	skb_queue_head_init(&local->queue);
	init_waitqueue_head(&local->readq);
	skb_queue_head_init(&local->ioctl_queue);
	init_waitqueue_head(&local->ioctlq);

	local->rpdev = rpdev;

	dev = &local->dev;
	device_initialize(dev);
	dev->parent = &rpdev->dev;
	dev->class = rpmsg_class;

	/* Initialize character device */
	cdev_init(&local->cdev, &rpmsg_dev_fops);
	local->cdev.owner = THIS_MODULE;

	/* Get the rpmsg char device minor id */
	ret = ida_simple_get(&rpmsg_minor_ida, 0, RPMSG_USER_DEV_MAX_MINORS, GFP_KERNEL);
	if (ret < 0) {
		dev_err(&rpdev->dev, "Not able to get minor id for rpmsg device.\n");
		goto error1;
	}
	dev->id = ret;
	dev->devt = MKDEV(MAJOR(rpmsg_dev_major), ret);

	if(0 == strcmp("rpmsg-rpu0-channel", rpdev->id.name)){
		dev_set_name(&local->dev, "rpmsg_rpu%d", 0);
	}else if(0 == strcmp("rpmsg-rpu1-channel", rpdev->id.name)){
		dev_set_name(&local->dev, "rpmsg_rpu%d", 1);
	}else{
		dev_set_name(&local->dev, "rpmsg%d", ret);
	}
	
	

	ret = cdev_add(&local->cdev, dev->devt, 1);
	if (ret) {
		dev_err(&rpdev->dev, "chardev registration failed.\n");
		goto error2;
	}

	/* Set up the release function for cleanup */
	dev->release = rpmsg_proxy_dev_release_device;

	ret = device_add(dev);
	if (ret) {
		dev_err(&rpdev->dev, "device reister failed: %d\n", ret);
		put_device(dev);
		return ret;
	}

	dev_set_drvdata(&rpdev->dev, local);

	dev_info(&rpdev->dev, "new channel: 0x%x -> 0x%x!\n", rpdev->src, rpdev->dst);

	return 0;

error2:
	ida_simple_remove(&rpmsg_minor_ida, dev->id);
	rpmsg_destroy_ept(local->ept);
	put_device(dev);

error1:
	return ret;
}

static void rpmsg_proxy_dev_rpmsg_drv_remove(struct rpmsg_device *rpdev)
{
	struct _rpmsg_eptdev *local = dev_get_drvdata(&rpdev->dev);

	dev_info(&rpdev->dev, "Removing rpmsg proxy dev.\n");

	device_del(&local->dev);
	put_device(&local->dev);
}

static struct rpmsg_device_id rpmsg_proxy_dev_drv_id_table[] = {
	{ .name = "rpmsg-openamp-demo-channel" },
	{ .name = "rpmsg-rpu0-channel" },
	{ .name = "rpmsg-rpu1-channel" },
	{},
};

static struct rpmsg_driver rpmsg_proxy_dev_drv = {
	.drv.name = "rpmsg_rpu_dev_rpmsg",
	.drv.owner = THIS_MODULE,
	.id_table = rpmsg_proxy_dev_drv_id_table,
	.probe = rpmsg_proxy_dev_rpmsg_drv_probe,
	.remove = rpmsg_proxy_dev_rpmsg_drv_remove,
};

static int __init init(void)
{
	int ret;

	/* Allocate char device for this rpmsg driver */
	ret = alloc_chrdev_region(&rpmsg_dev_major, 0, RPMSG_USER_DEV_MAX_MINORS, KBUILD_MODNAME);
	if (ret) {
		pr_err("alloc_chrdev_region failed: %d\n", ret);
		return ret;
	}

	/* Create device class for this device */
	rpmsg_class = class_create(THIS_MODULE, KBUILD_MODNAME);
	if (IS_ERR(rpmsg_class)) {
		ret = PTR_ERR(rpmsg_class);
		pr_err("class_create failed: %d\n", ret);
		goto unreg_region;
	}

	return register_rpmsg_driver(&rpmsg_proxy_dev_drv);

unreg_region:
	unregister_chrdev_region(rpmsg_dev_major, RPMSG_USER_DEV_MAX_MINORS);
	return ret;
}

static void __exit fini(void)
{
	unregister_rpmsg_driver(&rpmsg_proxy_dev_drv);
	unregister_chrdev_region(rpmsg_dev_major, RPMSG_USER_DEV_MAX_MINORS);
	class_destroy(rpmsg_class);
}

module_init(init);
module_exit(fini);


MODULE_DESCRIPTION("Sample driver to exposes rpmsg svcs to userspace via a char device");
MODULE_LICENSE("GPL v2");
