#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/wait.h>
#include <linux/kdev_t.h>
#include <linux/spinlock.h>
#include <linux/fs.h>
#include <linux/kfifo.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include "uart16550.h"
#include "uart16550_hw.h"

MODULE_DESCRIPTION("Uart16550 driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("O.34-alpha-rc2");

//#define __DEBUG

#ifdef __DEBUG
#define dprintk(fmt, ...)     printk(KERN_DEBUG "%s:%d " fmt,           \
                                     __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define dprintk(fmt, ...)     do { } while (0)
#endif

static struct class *uart16550_class = NULL;

static int major = 42;

static int behavior = 0x3;

struct com_dev {
    DECLARE_KFIFO(inbuffer, uint8_t, FIFO_SIZE);
    DECLARE_KFIFO(outbuffer, uint8_t, FIFO_SIZE);

    struct cdev cdev;

    spinlock_t input_lock;
    spinlock_t output_lock;

    wait_queue_head_t read_wait_queue;
    wait_queue_head_t write_wait_queue;

    int base_port;
    int minor;
    int irq_no;

} com1_dev, com2_dev;

struct wait_list_element {
    struct list_head list;

    struct task_struct* task;
};

static int uart16550_open(struct inode* inode, struct file *filep) {
    int minor = iminor(inode);

    dprintk("Opening device com%d\n", minor + 1);

    if (minor == 0) {
        filep->private_data = (void *) &com1_dev;
    } else {
        filep->private_data = (void *) &com2_dev;
    }

    return 0;
}


static int uart16550_read(struct file *file, char* user_buffer,
                          size_t size, loff_t* offset)
{
    int device_port;
    struct com_dev *device = (struct com_dev*) file->private_data;
    uint8_t *buffer;
    size_t unwritten_bytes = 0;

    dprintk("READ: reading COM%d\n", device->minor + 1);

    device_port = device->base_port;

    buffer = kmalloc(sizeof(uint8_t) * size, GFP_KERNEL);

    if (buffer == NULL) {
        return -ENOMEM;
    }

    do {
        wait_event_interruptible(device->read_wait_queue,
                                 !kfifo_is_empty(&device->inbuffer));
    } while(!spin_trylock(&device->input_lock));

    size = kfifo_out(&device->inbuffer, buffer, size);

    spin_unlock(&device->input_lock);

    if ((unwritten_bytes = copy_to_user(user_buffer, buffer, size)) != 0) {
        size = size - unwritten_bytes;
    }

    kfree(buffer);

    dprintk("READ: %d bytes from COM%d\n", size,
            device->minor + 1);

    wake_up_interruptible(&device->read_wait_queue);
    uart16550_hw_force_interrupt_reemit(device_port);

    return size;
}

static int uart16550_write(struct file *file, const char *user_buffer,
                           size_t size, loff_t *offset)
{
    int bytes_copied = 0;
    uint32_t device_port;
    uint8_t *buffer;

    struct com_dev* device = file->private_data;

    if (&com1_dev == device) {
        device_port = COM1_BASEPORT;
    }
    else {
        device_port = COM2_BASEPORT;
    }

    dprintk("Writing to COM%d\n", device->minor + 1);

    buffer = kmalloc(sizeof(uint8_t) * size, GFP_KERNEL);

    if (buffer == NULL) {
        return -ENOMEM;
    }

    if (copy_from_user(buffer, user_buffer, size) != 0) {
        kfree(buffer);
        return -EFAULT;
    }

    do {
        wait_event_interruptible(device->write_wait_queue,
                                 !kfifo_is_full(&device->outbuffer));
    } while(!spin_trylock(&device->output_lock));

    bytes_copied = kfifo_in(&device->outbuffer, buffer, size);

    spin_unlock(&device->output_lock);

    kfree(buffer);

    dprintk("Written %d bytes to COM%d buffer\n", bytes_copied,
            device->minor + 1);

    wake_up_interruptible(&device->write_wait_queue);
    uart16550_hw_force_interrupt_reemit(device_port);

    return bytes_copied;
}

static long uart16550_ioctl(struct file *f, unsigned int cmd,
                            unsigned long arg)
{
    struct uart16550_line_info line;
    struct uart16550_line_info *ptr = (struct uart16550_line_info*) arg;
    struct com_dev* device = (struct com_dev*) f->private_data;
    int port = device->base_port;

    if (cmd != UART16550_IOCTL_SET_LINE) {
        return -EINVAL;
    }

    if (copy_from_user(&line, ptr, sizeof(line)) != 0) {
        return -EFAULT;
    }

    switch (line.baud) {
    case UART16550_BAUD_1200:
    case UART16550_BAUD_2400:
    case UART16550_BAUD_4800:
    case UART16550_BAUD_9600:
    case UART16550_BAUD_19200:
    case UART16550_BAUD_38400:
    case UART16550_BAUD_56000:
    case UART16550_BAUD_115200:
        break;
    default:
        return -EINVAL;
    }

    switch(line.len) {
    case UART16550_LEN_5:
    case UART16550_LEN_6:
    case UART16550_LEN_7:
    case UART16550_LEN_8:
        break;

    default:
        return -EINVAL;
    }

    switch(line.par) {
    case UART16550_PAR_NONE:
    case UART16550_PAR_ODD:
    case UART16550_PAR_EVEN:
    case UART16550_PAR_STICK:
        break;

    default:
        return -EINVAL;
    }

    switch(line.stop) {
    case UART16550_STOP_1:
    case UART16550_STOP_2:
        break;
    default:
        return -EINVAL;
    }

    uart16550_hw_set_line_parameters(port, line);
    return 0;
}

static int uart16550_release(struct inode* inode, struct file* file) {
    dprintk("Closing device com%d...\n", iminor(inode) + 1);
    return 0;
}

irqreturn_t interrupt_handler(int irq_no, void *data)
{
    int device_status;
    int device_num;
    struct com_dev *device;

    device = (irq_no == COM1_IRQ) ? &com1_dev : &com2_dev;
    device_num = device->minor + 1;

    dprintk("IH: COM%d IRQ\n", device_num);

    device_status = uart16550_hw_get_device_status(device->base_port);

    while (uart16550_hw_device_can_send(device_status)) {
        uint8_t byte_value = 0;

        if (kfifo_get(&device->outbuffer, &byte_value) == 0) {
            dprintk("IH: No more data COM%d\n", device_num);
            break;
        }

        dprintk("IH: %c to COM%d\n", byte_value, device_num);

        uart16550_hw_write_to_device(device->base_port, byte_value);
        device_status = uart16550_hw_get_device_status(device->base_port);
    }

    while (uart16550_hw_device_has_data(device_status)) {
        uint8_t byte_value = 0;

        byte_value = uart16550_hw_read_from_device(device->base_port);



        if (kfifo_put(&device->inbuffer, byte_value) != 0) {
            dprintk("No more buffer space to store data!\n");
            break;
        }

        dprintk("IH: %c from COM%d\n", byte_value, device_num);

        device_status = uart16550_hw_get_device_status(device->base_port);
    }

    wake_up_interruptible(&device->read_wait_queue);

    wake_up_interruptible(&device->write_wait_queue);

    return IRQ_HANDLED;
}

static struct file_operations fops = {
    .open = uart16550_open,
    .read = uart16550_read,
    .write = uart16550_write,
    .release = uart16550_release,
    .unlocked_ioctl = uart16550_ioctl,
};

static void init_struct(struct com_dev *dev) {
    spin_lock_init(&dev->input_lock);
    spin_lock_init(&dev->output_lock);

    INIT_KFIFO(dev->inbuffer);
    INIT_KFIFO(dev->outbuffer);

    init_waitqueue_head(&dev->read_wait_queue);
    init_waitqueue_head(&dev->write_wait_queue);
}

static int init_com_dev(struct com_dev *dev, int minor) {
    cdev_init(&dev->cdev, &fops);
    dev->cdev.ops = &fops;
    dev->cdev.owner = THIS_MODULE;

    return cdev_add(&dev->cdev, MKDEV(major, minor), 1);
}

static int uart16550_init_device(struct com_dev* device, int major, int minor,
                                 char* name, int baseport, int irq_no) {

    int rc;
    struct device *com;
    dev_t dev = MKDEV(major, minor);

    dprintk("Registering COM%d\n", minor + 1);

    device->minor = minor;
    device->irq_no = irq_no;
    device->base_port = baseport;

    /* Setup the hardware device */
    rc = register_chrdev_region(dev, 1, THIS_MODULE->name);

    if (rc != 0) {
        goto fail_register_chrdev;
    }

    init_struct(device);

    rc = request_irq(irq_no, interrupt_handler, IRQF_SHARED,
                     THIS_MODULE->name, device);

    if (rc != 0) {
        goto fail_request_irq;
    }

    /* Create the sysfs info for /dev/comX */
    com = device_create(uart16550_class, NULL, dev, NULL, name);

    if (IS_ERR(com)) {
        goto fail_device_create;
    }

    rc = uart16550_hw_setup_device(baseport, THIS_MODULE->name);

    if (rc != 0) {
        goto fail_hw_setup;
    }

    if ((rc = init_com_dev(device, minor)) != 0) {
        goto fail_init_com_dev;
    }

    return 0;

 fail_init_com_dev:
    uart16550_hw_cleanup_device(baseport);
 fail_hw_setup:
    device_destroy(uart16550_class, dev);
 fail_device_create:
    free_irq(irq_no, device);
 fail_request_irq:
    unregister_chrdev_region(dev, 1);
 fail_register_chrdev:
     return rc;
}

static int uart16550_init(void)
{
    int have_com1 = behavior & OPTION_COM1, have_com2 = behavior & OPTION_COM2;
    int rc;

    dprintk("Loading module...\n");

    switch(behavior) {
    case OPTION_BOTH:
    case OPTION_COM1:
    case OPTION_COM2:
        break;
    default:
        return -EINVAL;
    }

    if (0 > major || major > (1 << 13) - 1) {
        dprintk("Invalid major number!");
        return -EINVAL;
    }

    uart16550_class = class_create(THIS_MODULE, "uart16550");

    if (have_com1) {
        rc = uart16550_init_device(&com1_dev, major, 0, "com1",
                                   COM1_BASEPORT, COM1_IRQ);
        if (rc != 0) {
            return rc;
        }
    }

    if (have_com2) {
        rc = uart16550_init_device(&com2_dev, major, 1, "com2",
                                   COM2_BASEPORT, COM2_IRQ);

        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

static void uart16550_cleanup_device(struct com_dev* device) {
    dev_t dev = MKDEV(major, device->minor);
    int baseport = device->base_port;
    int irq_no = device->irq_no;

    dprintk("Cleanup hardware COM%d\n", device->minor + 1);
    uart16550_hw_cleanup_device(baseport);

    dprintk("Freeing IRQ\n");

    free_irq(irq_no, device);

    dprintk("Destroying device...\n");
    device_destroy(uart16550_class, dev);

    dprintk("unregister_chrdev_region");
    unregister_chrdev_region(dev, 1);

    dprintk("Deleting cdev\n");
    cdev_del(&device->cdev);
}

static void uart16550_cleanup(void)
{
    int have_com1 = behavior & OPTION_COM1, have_com2 = behavior & OPTION_COM2;

    dprintk("Unloading uart16550 module...\n");

    if (have_com1) {
        uart16550_cleanup_device(&com1_dev);
    }

    if (have_com2) {
        uart16550_cleanup_device(&com2_dev);
    }

    /*
     * Cleanup the sysfs device class.
     */
    class_unregister(uart16550_class);
    class_destroy(uart16550_class);
}

module_param(behavior, int, S_IRUGO);
module_param(major, int, S_IRUGO);


module_init(uart16550_init)
module_exit(uart16550_cleanup)
