#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/device.h>
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

#define __DEBUG

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

    struct list_head read_wait_list;
    struct list_head write_wait_list;

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
    } else if (minor == 1) {
        filep->private_data = (void *) &com2_dev;
    } else {
        dprintk("Invalid inode!");
        return -ENOENT;
    }

    return 0;
}

static int uart16550_read(struct file *file, char* user_buffer,
                          size_t size, loff_t* offset)
{
    int device_port;
    struct com_dev *device = (struct com_dev*) file->private_data;
    size_t actual_size;
    char* buffer;
    int rc = 0;
    size_t unwritten_bytes = 0;
    struct wait_list_element *sleeping_task = NULL;
    unsigned long irq_status;

    device_port = (device == &com1_dev) ? COM1_BASEPORT : COM2_BASEPORT;

    dprintk("Reading from device at %x\n", device_port);

    if (kfifo_is_empty(&device->inbuffer)) {
        dprintk("No data available! Sleeping...\n");

        sleeping_task = kmalloc(sizeof(struct wait_list_element), GFP_KERNEL);

        if (sleeping_task == NULL) {
            dprintk("Could not add process to wait_list!\n");
            return -ENOMEM;
        }

        sleeping_task->task = current;
        list_add_tail(&sleeping_task->list, &device->read_wait_list);

        set_current_state(TASK_INTERRUPTIBLE);
        schedule();
    }

    actual_size = min(kfifo_len(&device->inbuffer), size);

    buffer = kmalloc(sizeof(char) * actual_size, GFP_KERNEL);

    if (buffer == NULL) {
        dprintk("Could not allocate memory for storing device data!\n");
        return -ENOMEM;
    }

    spin_lock_irqsave(&device->input_lock, irq_status);

    actual_size = kfifo_out(&device->inbuffer, buffer, actual_size);

    spin_unlock_irqrestore(&device->input_lock, irq_status);

    if ((unwritten_bytes = copy_to_user(user_buffer, buffer, actual_size)) != 0) {
        dprintk("Failed to copy read data to userspace\n");
        rc = -EFAULT;
        actual_size = actual_size - unwritten_bytes;
    }

    kfree(buffer);

    if (rc == 0) {
        rc = put_user(actual_size, offset);
    } else {
        put_user(actual_size, offset);
    }

    return rc;
}

static int uart16550_write(struct file *file, const char *user_buffer,
                           size_t size, loff_t *offset)
{
    int bytes_copied = 0;
    uint32_t device_port;

    struct com_dev* device = file->private_data;

    if (&com1_dev == device) {
        device_port = COM1_BASEPORT;
    }
    else if (&com2_dev == device) {
        device_port = COM2_BASEPORT;
    }
    else {
        return -ENOENT;
    }

    dprintk("Writing to device at %x\n", device_port);

    if (kfifo_is_full(&device->inbuffer)) {
        //TODO: put current process to sleep
    }

    /*
     * TODO: Write the code that takes the data provided by the
     *      user from userspace and stores it in the kernel
     *      device outgoing buffer.
     * TODO: Populate bytes_copied with the number of bytes
     *      that fit in the outgoing buffer.
     */

    uart16550_hw_force_interrupt_reemit(device_port);

    return bytes_copied;
}

static int uart16550_release(struct inode* inode, struct file* file) {
    dprintk("Closing device com%d...\n", iminor(inode) + 1);
    return 0;
}

irqreturn_t interrupt_handler(int irq_no, void *data)
{
    int device_status;
    uint32_t device_port;
    struct com_dev *device;
    struct wait_list_element *head;

    /*
     * TODO: Write the code that handles a hardware interrupt.
     * TODO: Populate device_port with the port of the correct device.
     */

    device_port = (irq_no == COM1_IRQ) ? COM1_BASEPORT : COM2_BASEPORT;
    device = (irq_no == COM1_IRQ) ? &com1_dev : &com2_dev;

    device_status = uart16550_hw_get_device_status(device_port);

    while (uart16550_hw_device_can_send(device_status)) {
        uint8_t byte_value = 0;
        /*
         * TODO: Populate byte_value with the next value
         *      from the kernel device outgoing buffer.
         */
        uart16550_hw_write_to_device(device_port, byte_value);
        device_status = uart16550_hw_get_device_status(device_port);
    }

    while (uart16550_hw_device_has_data(device_status)) {
        uint8_t byte_value = 0;
        byte_value = uart16550_hw_read_from_device(device_port);

        spin_lock(&device->input_lock);
        kfifo_put(&device->inbuffer, byte_value);

        while (!list_empty(&device->read_wait_list)) {
            head = list_first_entry(&device->read_wait_list,
                                    struct wait_list_element, list);
            wake_up_process(head->task);
            list_del(&head->list);
            kfree(head);
        }

        spin_unlock(&device->input_lock);

        device_status = uart16550_hw_get_device_status(device_port);
    }

    return IRQ_HANDLED;
}

static struct file_operations fops = {
    .open = uart16550_open,
    .read = uart16550_read,
    .write = uart16550_write,
    .release = uart16550_release
};

static int init_com_dev(struct com_dev *dev, int minor) {
    cdev_init(&dev->cdev, &fops);
    dev->cdev.ops = &fops;
    dev->cdev.owner = THIS_MODULE;

    spin_lock_init(&dev->input_lock);
    spin_lock_init(&dev->output_lock);

    INIT_KFIFO(dev->inbuffer);
    INIT_KFIFO(dev->outbuffer);

    INIT_LIST_HEAD(&dev->read_wait_list);
    INIT_LIST_HEAD(&dev->write_wait_list);

    return cdev_add(&dev->cdev, MKDEV(major, minor), 1);
}

static int uart16550_init(void)
{
    struct device *com1, *com2;
    int have_com1 = behavior & OPTION_COM1, have_com2 = behavior & OPTION_COM2;
    int rc_com1 = 0, rc_com2 = 0;
    int rc;

    dprintk("Loading module...\n");
    /*
     * Setup a sysfs class & device to make /dev/com1 & /dev/com2 appear.
     */
    uart16550_class = class_create(THIS_MODULE, "uart16550");

    if (have_com1) {
        dprintk("Registering COM1\n");
        /* Setup the hardware device for COM1 */
        rc_com1 = uart16550_hw_setup_device(COM1_BASEPORT, THIS_MODULE->name);

        if (rc_com1 != 0) {
            return rc_com1;
        }
        /* Create the sysfs info for /dev/com1 */
        com1 = device_create(uart16550_class, NULL, MKDEV(major, 0), NULL, "com1");
        if (IS_ERR(com1)) {
            uart16550_hw_cleanup_device(COM1_BASEPORT);
            return -1;
        }

        if (init_com_dev(&com1_dev, 0) != 0) {
            uart16550_hw_cleanup_device(COM1_BASEPORT);
            device_destroy(uart16550_class, MKDEV(major, 0));
            return rc_com1;
        }

        /* rc_com1 = request_irq(COM1_IRQ, interrupt_handler, IRQF_SHARED, */
        /*                       "com1", NULL); */

        /* if (rc_com1 != 0) { */
        /*     dprintk("Could not register interrupt handler for COM1: %d\n", */
        /*             rc_com1); */
        /*     uart16550_hw_cleanup_device(COM1_BASEPORT); */
        /*     device_destroy(uart16550_class, MKDEV(major, 0)); */
        /*     cdev_del(&com1_dev.cdev); */
        /*     return rc_com1; */
        /* } */
    }

    if (have_com2) {
        dprintk("Registering COM2\n");
        /* Setup the hardware device for COM2 */
        rc_com2 = uart16550_hw_setup_device(COM2_BASEPORT, THIS_MODULE->name);
        if (rc_com2 != 0) {
            dprintk("hw_setup_device_failed for com2! %d\n", rc_com2);
            return rc_com2;
        }
        /* Create the sysfs info for /dev/com2 */
        com2 = device_create(uart16550_class, NULL, MKDEV(major, 1), NULL, "com2");
        if (IS_ERR(com2)) {
            dprintk("Failed to create device sysfs class\n");
            uart16550_hw_cleanup_device(COM2_BASEPORT);
            return rc_com2;
        }

        if (init_com_dev(&com2_dev, 1) != 0) {
            uart16550_hw_cleanup_device(COM2_BASEPORT);
            device_destroy(uart16550_class, MKDEV(major, 1));
            return -1;
        }

        /* rc_com2 = request_irq(COM2_IRQ, interrupt_handler, IRQF_SHARED, */
        /*                       "com2", NULL); */

        /* if (rc_com2 != 0) { */
        /*     uart16550_hw_cleanup_device(COM2_BASEPORT); */
        /*     device_destroy(uart16550_class, MKDEV(major, 1)); */
        /*     cdev_del(&com2_dev.cdev); */
        /*     return rc_com2; */
        /* } */
    }

    return 0;
}

static void uart16550_cleanup(void)
{
    int have_com1 = behavior & OPTION_COM1, have_com2 = behavior & OPTION_COM2;
    /*
     * TODO: Write driver cleanup code here.
     * TODO: have_com1 & have_com2 need to be set according to the
     *      module parameters.
     */
    if (have_com1) {
        cdev_del(&com1_dev.cdev);
        /* Reset the hardware device for COM1 */
        uart16550_hw_cleanup_device(COM1_BASEPORT);
        /* Remove the sysfs info for /dev/com1 */
        device_destroy(uart16550_class, MKDEV(major, 0));

        free_irq(COM1_IRQ, NULL);
    }
    if (have_com2) {
        cdev_del(&com2_dev.cdev);
        /* Reset the hardware device for COM2 */
        uart16550_hw_cleanup_device(COM2_BASEPORT);
        /* Remove the sysfs info for /dev/com2 */
        device_destroy(uart16550_class, MKDEV(major, 1));
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
