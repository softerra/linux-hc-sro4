/* Precise measurements of time delta between sending a trigger signal
 * to the HC-SR04 distance sensor and receiving the echo signal from
 * the sensor back. This has to be precise in the usecs range. We
 * use trigger interrupts to measure the signal, so no busy wait :)
 *
 * This supports an (in theory) unlimited number of HC-SR04 devices.
 * To add a device, do a (as root):
 *
 *	# echo 23 24 1000 > /sys/class/distance-sensor/configure
 *
 * (23 is the trigger GPIO, 24 is the echo GPIO and 1000 is a timeout in
 *  milliseconds)
 *
 * Then a directory appears with a file measure in it. To measure, do a
 *
 *	# cat /sys/class/distance-sensor/distance_23_24/measure
 *
 * You'll receive the length of the echo signal in usecs. To convert (roughly)
 * to centimeters multiply by 17150 and divide by 1e6.
 *
 * To deconfigure the device, do a
 *
 *	# echo -23 24 > /sys/class/distance-sensor/configure
 *
 * (normally not needed).
 *
 * DO NOT attach your HC-SR04's echo pin directly to the raspberry, since
 * it runs with 5V while raspberry expects 3V on the GPIO inputs.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/timekeeping.h>
#include <linux/gpio/consumer.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/interrupt.h>
#include <linux/kdev_t.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/sched.h>

struct hc_sr04 {
	int gpio_trig;
	int gpio_echo;
	int irq;
	struct timespec64 time_triggered;
	struct timespec64 time_echoed;
	int echo_received;
	int device_triggered;
	struct mutex measurement_mutex;
	wait_queue_head_t wait_for_echo;
	unsigned long timeout;
	struct list_head list;
};

static LIST_HEAD(hc_sr04_devices);
static DEFINE_MUTEX(devices_mutex);

static int setup_hc_sr04_gpio(int trig, int echo)
{
	int ret;

	ret = 0;
	if (!gpio_is_valid(trig) || !gpio_is_valid(echo)) {
		pr_err("Failed validation of GPIOs %d, %d\n", trig, echo);
		return -EINVAL;
	}

	ret = gpio_request(trig, "trig");
	if (ret < 0) {
		pr_err("GPIO %d request failed. Exiting.\n", trig);
		return ret;
	}

	ret = gpio_request(echo, "echo");
	if (ret < 0) {
		pr_err("GPIO %d request failed. Exiting.\n", echo);
		gpio_free(trig);
		return ret;
	}

	gpio_direction_output(trig, 0);
	gpio_direction_input(echo);

	pr_info("hc-sr04: acquired gpio trig=%d, echo=%d\n", trig, echo);

	return ret;
}
static irqreturn_t echo_received_irq(int irq, void *data);
static int setup_hc_sr04_irq(struct hc_sr04 *device)
{
	int ret;

	ret = 0;

	device->irq = gpio_to_irq(device->gpio_echo);
	if (device->irq < 0) {
		pr_err("Failed to retrieve IRQ number for echo GPIO. Exiting.\n");
		return device->irq;
	}

	pr_info("hc-sr04: assigned IRQ number %d\n", device->irq);
	ret = request_any_context_irq(device->irq, echo_received_irq,
		IRQF_SHARED | IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
		"hc_sr04", device);
	if (ret < 0) {
		pr_err("request_irq() failed. Exiting.\n");
	}
	return ret;
}

static struct hc_sr04 *create_hc_sr04(int trig, int echo, unsigned long timeout)
		/* must be called with devices_mutex held */
{
	struct hc_sr04 *new;
	int err;

	new = kmalloc(sizeof(*new), GFP_KERNEL);
	if (new == NULL)
		return ERR_PTR(-ENOMEM);

	new->gpio_echo = echo;
	new->gpio_trig = trig;

	err = setup_hc_sr04_gpio(new->gpio_trig, new->gpio_echo);
	if (err != 0) {
		kfree(new);
		return ERR_PTR(err);
	}

	mutex_init(&new->measurement_mutex);
	init_waitqueue_head(&new->wait_for_echo);
	new->timeout = timeout;

	err = setup_hc_sr04_irq(new);
	if (err != 0) {
		gpio_free(new->gpio_trig);
		gpio_free(new->gpio_echo);
		kfree(new);
		return ERR_PTR(err);
	}

	list_add_tail(&new->list, &hc_sr04_devices);

	return new;
}

static void destroy_hc_sr04(struct hc_sr04 *device)
{
	list_del(&device->list);
	free_irq(device->irq, device);
	gpio_free(device->gpio_echo);
	gpio_free(device->gpio_trig);
	kfree(device);
}

static irqreturn_t echo_received_irq(int irq, void *data)
{
	struct hc_sr04 *device = (struct hc_sr04 *) data;
	int val;
	struct timespec64 irq_ts;

	ktime_get_real_ts64(&irq_ts);

	if (!device->device_triggered)
		return IRQ_HANDLED;
	if (device->echo_received)
		return IRQ_HANDLED;

	val = __gpio_get_value(device->gpio_echo);
	if (val == 1) {
		device->time_triggered = irq_ts;
	} else {
		device->time_echoed = irq_ts;
		device->echo_received = 1;
		wake_up_interruptible(&device->wait_for_echo);
	}

	return IRQ_HANDLED;
}

/* devices_mutex must be held by caller, so nobody deletes the device
 * before we lock it.
 */

static int do_measurement(struct hc_sr04 *device,
			  unsigned long long *usecs_elapsed)
{
	long timeout;
	int ret;

	if (!mutex_trylock(&device->measurement_mutex)) {
		mutex_unlock(&devices_mutex);
		return -EBUSY;
	}
	mutex_unlock(&devices_mutex);

	msleep(60);
		/* wait 60 ms between measurements.
		 * now, a while true ; do cat measure ; done should work
		 */

	device->echo_received = 0;
	device->device_triggered = 0;

	gpio_set_value(device->gpio_trig, 1);
	udelay(10);
	device->device_triggered = 1;
	gpio_set_value(device->gpio_trig, 0);

	timeout = wait_event_interruptible_timeout(device->wait_for_echo,
				device->echo_received, device->timeout);

	if (timeout == 0)
		ret = -ETIMEDOUT;
	else if (timeout < 0)
		ret = timeout;
	else {
		*usecs_elapsed =
	(device->time_echoed.tv_sec - device->time_triggered.tv_sec) * 1000000 +
	(device->time_echoed.tv_nsec - device->time_triggered.tv_nsec) / 1000;
		ret = 0;
	}

	mutex_unlock(&device->measurement_mutex);

	return ret;
}

static ssize_t sysfs_do_measurement(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct hc_sr04 *sensor = dev_get_drvdata(dev);
	unsigned long long usecs_elapsed;
	int status;

	mutex_lock(&devices_mutex);
	status = do_measurement(sensor, &usecs_elapsed);

	if (status < 0)
		return status;

	return sprintf(buf, "%lld\n", usecs_elapsed);
}

DEVICE_ATTR(measure, 0444, sysfs_do_measurement, NULL);


static struct attribute *sensor_attrs[] = {
	&dev_attr_measure.attr,
	NULL,
};

static const struct attribute_group sensor_group = {
	.attrs = sensor_attrs
};

static const struct attribute_group *sensor_groups[] = {
	&sensor_group,
	NULL
};

static ssize_t configure_store(struct class *class,
				struct class_attribute *attr,
				const char *buf, size_t len);

static CLASS_ATTR_WO(configure);

static struct attribute *hc_sr04_class_attrs[] = {
	&class_attr_configure.attr,
	NULL,
};

ATTRIBUTE_GROUPS(hc_sr04_class);

static struct class hc_sr04_class = {
	.name = "distance-sensor",
	.owner = THIS_MODULE,
	.class_groups   = hc_sr04_class_groups,
};


static struct hc_sr04 *find_sensor(int trig, int echo)
{
	struct hc_sr04 *sensor;

	list_for_each_entry(sensor, &hc_sr04_devices, list) {
		if (sensor->gpio_trig == trig &&
		    sensor->gpio_echo == echo)
			return sensor;
	}
	return NULL;
}

static int match_device(struct device *dev, const void *data)
{
	return dev_get_drvdata(dev) == data;
}

static int add_sensor(int trig, int echo, unsigned long timeout)
{
	struct hc_sr04 *new_sensor;

	new_sensor = create_hc_sr04(trig, echo, timeout);
	if (IS_ERR(new_sensor)) {
		return PTR_ERR(new_sensor);
	}

	device_create_with_groups(&hc_sr04_class, NULL, MKDEV(0, 0), new_sensor,
			sensor_groups, "distance_%d_%d", trig, echo);
	return 0;
}

static int remove_sensor(struct hc_sr04 *rip_sensor)
	/* must be called with devices_mutex held. */
{
	struct device *dev;

	dev = class_find_device(&hc_sr04_class, NULL, rip_sensor, match_device);
	if (dev == NULL)
		return -ENODEV;

	mutex_lock(&rip_sensor->measurement_mutex);
			/* wait until measurement has finished */

	device_unregister(dev);
	put_device(dev);
	mutex_unlock(&rip_sensor->measurement_mutex);

	destroy_hc_sr04(rip_sensor);
	return 0;
}

static ssize_t configure_store(struct class *class,
				struct class_attribute *attr,
				const char *buf, size_t len)
{
	int add = buf[0] != '-';
	const char *s = buf;
	int trig, echo, timeout;
	struct hc_sr04 *rip_sensor;
	int err;

	if (buf[0] == '-' || buf[0] == '+')
		s++;

	if (add) {
		if (sscanf(s, "%d %d %d", &trig, &echo, &timeout) != 3)
			return -EINVAL;

		mutex_lock(&devices_mutex);
		if (find_sensor(trig, echo)) {
			mutex_unlock(&devices_mutex);
			return -EEXIST;
		}

		err = add_sensor(trig, echo, timeout);
		mutex_unlock(&devices_mutex);
		if (err < 0)
			return err;
		pr_info("hc-sr04: added device trig=%d echo=%d\n", trig, echo);
	} else {
		if (sscanf(s, "%d %d", &trig, &echo) != 2)
			return -EINVAL;

		mutex_lock(&devices_mutex);
		rip_sensor = find_sensor(trig, echo);
		if (rip_sensor == NULL) {
			mutex_unlock(&devices_mutex);
			return -ENODEV;
		}

		err = remove_sensor(rip_sensor);
		mutex_unlock(&devices_mutex);
		if (err < 0)
			return err;
		pr_info("hc-sr04: removed device trig=%d echo=%d\n", trig, echo);
	}
	return len;
}

static int __init init_hc_sr04(void)
{
	return class_register(&hc_sr04_class);
}

static void exit_hc_sr04(void)
{
	struct hc_sr04 *rip_sensor, *tmp;

	mutex_lock(&devices_mutex);
	list_for_each_entry_safe(rip_sensor, tmp, &hc_sr04_devices, list) {
		remove_sensor(rip_sensor);   /* ignore errors */
	}
	mutex_unlock(&devices_mutex);

	class_unregister(&hc_sr04_class);
}

module_init(init_hc_sr04);
module_exit(exit_hc_sr04);

MODULE_AUTHOR("Johannes Thoma");
MODULE_DESCRIPTION("Distance measurement for the HC-SR04 ultrasonic distance sensor");
MODULE_LICENSE("GPL");

