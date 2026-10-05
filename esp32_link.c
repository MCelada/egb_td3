#include <linux/module.h>
#include <linux/serdev.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/kfifo.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/of.h>               /* Faltaba para el Device Tree */
#include <linux/mod_devicetable.h>  /* Faltaba para of_device_id */

#define DRIVER_NAME "esp32_link"
#define FIFO_SIZE 1024

#define ESP32_GET _IOR('E', 1, int)

struct esp32_link_dev {
    struct serdev_device *serdev;
    struct cdev cdev;
    dev_t devt;
    
    DECLARE_KFIFO(rx_fifo, unsigned char, FIFO_SIZE);
    wait_queue_head_t read_wait;
    
    struct mutex ioctl_lock;
    struct completion ioctl_comp;
    
    int last_sync_result; 
};

static struct class *esp32_class;
static dev_t esp32_devt_base;

/* CORRECCIÓN 1: Devolvemos size_t en lugar de int */
static size_t esp32_link_recv(struct serdev_device *serdev, const unsigned char *buf, size_t count)
{
    struct esp32_link_dev *priv = serdev_device_get_drvdata(serdev);
    unsigned int copied;

    copied = kfifo_in(&priv->rx_fifo, buf, count);
    
    if (copied > 0)
        wake_up_interruptible(&priv->read_wait);

    if (count > 0 && buf[0] == '{') { 
        priv->last_sync_result = 0;   
        complete(&priv->ioctl_comp);  
    }

    return (size_t)copied;
}

static const struct serdev_device_ops esp32_link_serdev_ops = {
    .receive_buf = esp32_link_recv,
    .write_wakeup = serdev_device_write_wakeup,
};

static ssize_t esp32_link_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    struct esp32_link_dev *priv = file->private_data;
    unsigned int copied;
    int ret;

    if (wait_event_interruptible(priv->read_wait, !kfifo_is_empty(&priv->rx_fifo)))
        return -ERESTARTSYS;

    ret = kfifo_to_user(&priv->rx_fifo, buf, count, &copied);
    if (ret)
        return ret;

    return copied;
}

static ssize_t esp32_link_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    struct esp32_link_dev *priv = file->private_data;
    unsigned char *kbuf;
    int ret;

    kbuf = memdup_user(buf, count);
    if (IS_ERR(kbuf))
        return PTR_ERR(kbuf);

    ret = serdev_device_write_buf(priv->serdev, kbuf, count);
    
    kfree(kbuf);
    return (ret < 0) ? ret : count;
}

static long esp32_link_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct esp32_link_dev *priv = file->private_data;
    long ret = 0;
    unsigned long timeout;
    char sync_req[] = "{\"cmd\":\"get_status\"}\n";

    switch (cmd) {
    case ESP32_GET:
        if (mutex_lock_interruptible(&priv->ioctl_lock))
            return -ERESTARTSYS;

        reinit_completion(&priv->ioctl_comp);
        serdev_device_write_buf(priv->serdev, (unsigned char *)sync_req, sizeof(sync_req) - 1);

        timeout = wait_for_completion_interruptible_timeout(&priv->ioctl_comp, msecs_to_jiffies(1000));
        
        if (timeout == 0) {
            ret = -ETIMEDOUT;
        } else if (timeout == -ERESTARTSYS) {
            ret = -ERESTARTSYS;
        } else {
            if (copy_to_user((int __user *)arg, &priv->last_sync_result, sizeof(int)))
                ret = -EFAULT;
        }

        mutex_unlock(&priv->ioctl_lock);
        break;

    default:
        ret = -ENOTTY;
    }

    return ret;
}

static int esp32_link_open(struct inode *inode, struct file *file)
{
    struct esp32_link_dev *priv = container_of(inode->i_cdev, struct esp32_link_dev, cdev);
    file->private_data = priv;
    return 0;
}

static const struct file_operations esp32_link_fops = {
    .owner          = THIS_MODULE,
    .open           = esp32_link_open,
    .read           = esp32_link_read,
    .write          = esp32_link_write,
    .unlocked_ioctl = esp32_link_ioctl,
};

static int esp32_link_probe(struct serdev_device *serdev)
{
    struct esp32_link_dev *priv;
    struct device *dev;
    int ret;

    priv = devm_kzalloc(&serdev->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    priv->serdev = serdev;
    serdev_device_set_drvdata(serdev, priv);

    INIT_KFIFO(priv->rx_fifo);
    init_waitqueue_head(&priv->read_wait);
    mutex_init(&priv->ioctl_lock);
    init_completion(&priv->ioctl_comp);

    serdev_device_set_client_ops(serdev, &esp32_link_serdev_ops);
    ret = serdev_device_open(serdev);
    if (ret)
        return ret;

    serdev_device_set_baudrate(serdev, 115200);
    serdev_device_set_flow_control(serdev, false);

    priv->devt = MKDEV(MAJOR(esp32_devt_base), 0);
    cdev_init(&priv->cdev, &esp32_link_fops);
    priv->cdev.owner = THIS_MODULE;

    ret = cdev_add(&priv->cdev, priv->devt, 1);
    if (ret) {
        serdev_device_close(serdev);
        return ret;
    }

    dev = device_create(esp32_class, &serdev->dev, priv->devt, priv, DRIVER_NAME);
    if (IS_ERR(dev)) {
        cdev_del(&priv->cdev);
        serdev_device_close(serdev);
        return PTR_ERR(dev);
    }

    dev_info(&serdev->dev, "Driver esp32_link inicializado (/dev/%s)\n", DRIVER_NAME);
    return 0;
}

static void esp32_link_remove(struct serdev_device *serdev)
{
    struct esp32_link_dev *priv = serdev_device_get_drvdata(serdev);

    device_destroy(esp32_class, priv->devt);
    cdev_del(&priv->cdev);
    serdev_device_close(serdev);
}

static const struct of_device_id esp32_link_of_match[] = {
    { .compatible = "custom,esp32-link" },
    { }
};
MODULE_DEVICE_TABLE(of, esp32_link_of_match);

static struct serdev_device_driver esp32_link_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = esp32_link_of_match,
    },
    .probe = esp32_link_probe,
    .remove = esp32_link_remove,
};

static int __init esp32_link_init(void)
{
    int ret;

    ret = alloc_chrdev_region(&esp32_devt_base, 0, 1, DRIVER_NAME);
    if (ret)
        return ret;

    /* CORRECCIÓN 2: Eliminamos THIS_MODULE, el kernel moderno ya no lo pide */
    esp32_class = class_create(DRIVER_NAME);
    if (IS_ERR(esp32_class)) {
        unregister_chrdev_region(esp32_devt_base, 1);
        return PTR_ERR(esp32_class);
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

MODULE_AUTHOR("Tu Nombre");
MODULE_DESCRIPTION("Serdev driver para carga electrónica ESP32");
MODULE_LICENSE("GPL v2");