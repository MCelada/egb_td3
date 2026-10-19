#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/serdev.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/wait.h>

#include "esp32_link.h"

#define MAX_WRITE_SIZE 4096

struct esp32_link_dev {
	struct serdev_device *serdev;
	struct cdev cdev;
	dev_t devt;
	DECLARE_KFIFO(rx_fifo, unsigned char, ESP32_LINK_FIFO_SIZE);
	wait_queue_head_t read_wait;
	struct mutex tx_lock;
	bool online;
};

static struct class *esp32_class;
static dev_t esp32_devt_base;

static size_t esp32_link_recv(struct serdev_device *serdev,
			     const unsigned char *buf, size_t count)
{
	struct esp32_link_dev *priv = serdev_device_get_drvdata(serdev);
	unsigned int copied;

	copied = kfifo_in(&priv->rx_fifo, buf, count);
	if (copied > 0)
		wake_up_interruptible(&priv->read_wait);

	if (copied < count)
		dev_warn_ratelimited(&serdev->dev,
				     "RX FIFO llena; se descartaron %zu bytes\n",
				     count - copied);

	return copied;
}

static void esp32_link_write_wakeup(struct serdev_device *serdev)
{
	serdev_device_write_wakeup(serdev);
}

static const struct serdev_device_ops esp32_link_serdev_ops = {
	.receive_buf = esp32_link_recv,
	.write_wakeup = esp32_link_write_wakeup,
};

static ssize_t esp32_link_read(struct file *file, char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct esp32_link_dev *priv = file->private_data;
	unsigned int copied;
	int ret;

	if (count == 0)
		return 0;

	while (kfifo_is_empty(&priv->rx_fifo)) {
		if (!READ_ONCE(priv->online))
			return 0;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible(priv->read_wait,
					       !kfifo_is_empty(&priv->rx_fifo) ||
					       !READ_ONCE(priv->online));
		if (ret)
			return ret;
	}

	ret = kfifo_to_user(&priv->rx_fifo, buf, count, &copied);
	return ret ? ret : copied;
}

static ssize_t esp32_link_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	struct esp32_link_dev *priv = file->private_data;
	unsigned char *kbuf;
	ssize_t written;
	int ret;

	if (count == 0)
		return 0;
	if (count > MAX_WRITE_SIZE)
		return -EMSGSIZE;
	if (!READ_ONCE(priv->online))
		return -ENODEV;

	kbuf = memdup_user(buf, count);
	if (IS_ERR(kbuf))
		return PTR_ERR(kbuf);

	ret = mutex_lock_interruptible(&priv->tx_lock);
	if (ret) {
		kfree(kbuf);
		return ret;
	}

	written = serdev_device_write(priv->serdev, kbuf, count,
				      msecs_to_jiffies(ESP32_LINK_WRITE_TIMEOUT_MS));
	mutex_unlock(&priv->tx_lock);
	kfree(kbuf);

	return written;
}

static __poll_t esp32_link_poll(struct file *file, poll_table *wait)
{
	struct esp32_link_dev *priv = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &priv->read_wait, wait);
	if (!kfifo_is_empty(&priv->rx_fifo))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (READ_ONCE(priv->online))
		mask |= EPOLLOUT | EPOLLWRNORM;
	else
		mask |= EPOLLHUP | EPOLLERR;

	return mask;
}

static int esp32_link_open(struct inode *inode, struct file *file)
{
	struct esp32_link_dev *priv = container_of(inode->i_cdev,
							  struct esp32_link_dev, cdev);

	if (!READ_ONCE(priv->online))
		return -ENODEV;
	file->private_data = priv;
	return 0;
}

static const struct file_operations esp32_link_fops = {
	.owner = THIS_MODULE,
	.open = esp32_link_open,
	.read = esp32_link_read,
	.write = esp32_link_write,
	.poll = esp32_link_poll,
	.llseek = noop_llseek,
};

static int esp32_link_probe(struct serdev_device *serdev)
{
	struct esp32_link_dev *priv;
	struct device *dev;
	u32 baudrate = ESP32_LINK_DEFAULT_BAUD;
	unsigned int actual_baudrate;
	int ret;

	priv = devm_kzalloc(&serdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->serdev = serdev;
	priv->devt = esp32_devt_base;
	priv->online = true;
	INIT_KFIFO(priv->rx_fifo);
	init_waitqueue_head(&priv->read_wait);
	mutex_init(&priv->tx_lock);
	serdev_device_set_drvdata(serdev, priv);
	serdev_device_set_client_ops(serdev, &esp32_link_serdev_ops);

	ret = serdev_device_open(serdev);
	if (ret)
		return dev_err_probe(&serdev->dev, ret, "no se pudo abrir el UART\n");

	if (serdev->dev.of_node)
		of_property_read_u32(serdev->dev.of_node, "current-speed", &baudrate);

	actual_baudrate = serdev_device_set_baudrate(serdev, baudrate);
	serdev_device_set_flow_control(serdev, false);
	ret = serdev_device_set_parity(serdev, SERDEV_PARITY_NONE);
	if (ret) {
		serdev_device_close(serdev);
		return dev_err_probe(&serdev->dev, ret, "no se pudo configurar 8N1\n");
	}

	cdev_init(&priv->cdev, &esp32_link_fops);
	priv->cdev.owner = THIS_MODULE;
	ret = cdev_add(&priv->cdev, priv->devt, 1);
	if (ret) {
		serdev_device_close(serdev);
		return dev_err_probe(&serdev->dev, ret, "no se pudo registrar el char device\n");
	}

	dev = device_create(esp32_class, &serdev->dev, priv->devt, priv,
			    ESP32_LINK_DRIVER_NAME);
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		cdev_del(&priv->cdev);
		serdev_device_close(serdev);
		return dev_err_probe(&serdev->dev, ret, "no se pudo crear /dev/%s\n",
				     ESP32_LINK_DRIVER_NAME);
	}

	dev_info(&serdev->dev, "%s listo a %u baud (%s)\n",
		 ESP32_LINK_DEVICE_NODE, actual_baudrate,
		 (actual_baudrate == baudrate) ? "8N1" : "baudrate ajustado por el controlador");
	return 0;
}

static void esp32_link_remove(struct serdev_device *serdev)
{
	struct esp32_link_dev *priv = serdev_device_get_drvdata(serdev);

	WRITE_ONCE(priv->online, false);
	wake_up_interruptible(&priv->read_wait);
	device_destroy(esp32_class, priv->devt);
	cdev_del(&priv->cdev);
	serdev_device_close(serdev);
}

static const struct of_device_id esp32_link_of_match[] = {
	{ .compatible = ESP32_LINK_COMPATIBLE },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32_link_of_match);

static struct serdev_device_driver esp32_link_driver = {
	.driver = {
		.name = ESP32_LINK_DRIVER_NAME,
		.of_match_table = esp32_link_of_match,
	},
	.probe = esp32_link_probe,
	.remove = esp32_link_remove,
};

static int __init esp32_link_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&esp32_devt_base, 0, 1,
				  ESP32_LINK_DRIVER_NAME);
	if (ret)
		return ret;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	esp32_class = class_create(ESP32_LINK_DRIVER_NAME);
#else
	esp32_class = class_create(THIS_MODULE, ESP32_LINK_DRIVER_NAME);
#endif
	if (IS_ERR(esp32_class)) {
		ret = PTR_ERR(esp32_class);
		unregister_chrdev_region(esp32_devt_base, 1);
		return ret;
	}

	ret = serdev_device_driver_register(&esp32_link_driver);
	if (ret) {
		class_destroy(esp32_class);
		unregister_chrdev_region(esp32_devt_base, 1);
	}
	return ret;
}

static void __exit esp32_link_exit(void)
{
	serdev_device_driver_unregister(&esp32_link_driver);
	class_destroy(esp32_class);
	unregister_chrdev_region(esp32_devt_base, 1);
}

module_init(esp32_link_init);
module_exit(esp32_link_exit);

MODULE_AUTHOR("Tecnicas Digitales III");
MODULE_DESCRIPTION("Driver serdev para la carga electronica ESP32-S3");
MODULE_LICENSE("GPL");
