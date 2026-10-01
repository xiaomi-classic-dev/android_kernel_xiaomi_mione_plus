/*
 * Broadcom Bluetooth WAKE/HOST_WAKE for the MiOne UART.
 * Based on the 2009 Google driver and MiOne commit cbd5a3af3b4c.
 * Clock preparation runs in a worker, never under the UART spinlock.
 *
 * This software is licensed under the GNU General Public License version 2.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/wakelock.h>
#include <linux/gpio.h>
#include <mach/gpio.h>
#include <mach/bcm_bt_lpm.h>

static struct {
	struct bcm_bt_lpm_platform_data *pdata;
	struct uart_port *uport;
	struct mutex port_mutex;
	spinlock_t lock;
	struct hrtimer timer;
	struct work_struct clock_work;
	struct workqueue_struct *wq;
	struct wake_lock wake_lock;
	bool ready, active, wake;
	unsigned int generation;
	unsigned long tx_events, host_events, idle_events;
	int irq;
} bt;

static void clock_work(struct work_struct *work)
{
	unsigned long flags;
	unsigned int generation;
	bool awake, changed;

	mutex_lock(&bt.port_mutex);
	if (!bt.uport)
		goto out;
	do {
		spin_lock_irqsave(&bt.lock, flags);
		generation = bt.generation;
		awake = bt.wake || gpio_get_value(bt.pdata->gpio_host_wake);
		spin_unlock_irqrestore(&bt.lock, flags);
		if (awake)
			bt.pdata->request_clock_on(bt.uport);
		else
			bt.pdata->request_clock_off(bt.uport);
		spin_lock_irqsave(&bt.lock, flags);
		changed = generation != bt.generation;
		spin_unlock_irqrestore(&bt.lock, flags);
	} while (changed);
out:
	mutex_unlock(&bt.port_mutex);
}

static enum hrtimer_restart enter_lpm(struct hrtimer *timer)
{
	unsigned long flags;
	spin_lock_irqsave(&bt.lock, flags);
	if (bt.active) {
		bt.idle_events++;
		bt.wake = false;
		bt.generation++;
		gpio_set_value(bt.pdata->gpio_wake, 0);
		queue_work(bt.wq, &bt.clock_work);
	}
	spin_unlock_irqrestore(&bt.lock, flags);
	return HRTIMER_NORESTART;
}

void bcm_bt_lpm_exit_lpm_locked(struct uart_port *uport)
{
	unsigned long flags;
	if (!bt.ready)
		return;
	spin_lock_irqsave(&bt.lock, flags);
	if (bt.active) {
		bt.tx_events++;
		bt.wake = true;
		bt.generation++;
		gpio_set_value(bt.pdata->gpio_wake, 1);
		if (gpio_get_value(bt.pdata->gpio_host_wake))
			wake_lock(&bt.wake_lock);
		else
			wake_lock_timeout(&bt.wake_lock, 2 * HZ);
		hrtimer_start(&bt.timer, ktime_set(1, 0), HRTIMER_MODE_REL);
		queue_work(bt.wq, &bt.clock_work);
	}
	spin_unlock_irqrestore(&bt.lock, flags);
}
EXPORT_SYMBOL(bcm_bt_lpm_exit_lpm_locked);

int bcm_bt_lpm_startup(struct uart_port *uport)
{
	unsigned long flags;
	if (!bt.ready)
		return -ENODEV;
	mutex_lock(&bt.port_mutex);
	bt.uport = uport;
	spin_lock_irqsave(&bt.lock, flags);
	bt.active = true;
	bt.wake = true;
	bt.generation++;
	gpio_set_value(bt.pdata->gpio_wake, 1);
	if (gpio_get_value(bt.pdata->gpio_host_wake))
		wake_lock(&bt.wake_lock);
	else
		wake_lock_timeout(&bt.wake_lock, 2 * HZ);
	spin_unlock_irqrestore(&bt.lock, flags);
	mutex_unlock(&bt.port_mutex);
	return 0;
}
EXPORT_SYMBOL(bcm_bt_lpm_startup);

void bcm_bt_lpm_shutdown(struct uart_port *uport)
{
	unsigned long flags;
	if (!bt.ready)
		return;
	mutex_lock(&bt.port_mutex);
	spin_lock_irqsave(&bt.lock, flags);
	bt.active = false;
	bt.wake = false;
	bt.uport = NULL;
	bt.generation++;
	gpio_set_value(bt.pdata->gpio_wake, 0);
	spin_unlock_irqrestore(&bt.lock, flags);
	mutex_unlock(&bt.port_mutex);
	hrtimer_cancel(&bt.timer);
	flush_workqueue(bt.wq);
	wake_unlock(&bt.wake_lock);
}
EXPORT_SYMBOL(bcm_bt_lpm_shutdown);

static irqreturn_t host_wake_thread(int irq, void *data)
{
	unsigned long flags;
	spin_lock_irqsave(&bt.lock, flags);
	if (bt.active) {
		bt.host_events++;
		bt.generation++;
		if (gpio_get_value(bt.pdata->gpio_host_wake))
			wake_lock(&bt.wake_lock);
		else
			wake_lock_timeout(&bt.wake_lock, HZ);
		queue_work(bt.wq, &bt.clock_work);
	}
	spin_unlock_irqrestore(&bt.lock, flags);
	return IRQ_HANDLED;
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	unsigned long flags;
	ssize_t n;
	spin_lock_irqsave(&bt.lock, flags);
	n = scnprintf(buf, PAGE_SIZE,
		"active=%u wake=%u host_wake=%u tx=%lu host_irq=%lu idle=%lu\n",
		bt.active, gpio_get_value(bt.pdata->gpio_wake),
		gpio_get_value(bt.pdata->gpio_host_wake),
		bt.tx_events, bt.host_events, bt.idle_events);
	spin_unlock_irqrestore(&bt.lock, flags);
	return n;
}
static DEVICE_ATTR(state, S_IRUGO, state_show, NULL);

static int bcm_bt_lpm_probe(struct platform_device *pdev)
{
	int ret;
	bt.pdata = pdev->dev.platform_data;
	if (!bt.pdata || bt.ready)
		return -EINVAL;
	mutex_init(&bt.port_mutex);
	spin_lock_init(&bt.lock);
	INIT_WORK(&bt.clock_work, clock_work);
	hrtimer_init(&bt.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	bt.timer.function = enter_lpm;
	wake_lock_init(&bt.wake_lock, WAKE_LOCK_SUSPEND, "bcm_bt_lpm");
	bt.wq = create_singlethread_workqueue("bcm_bt_lpm");
	if (!bt.wq) { ret = -ENOMEM; goto fail_wake; }
	ret = gpio_request(bt.pdata->gpio_wake, "bt_wake");
	if (ret) goto fail_wq;
	ret = gpio_request(bt.pdata->gpio_host_wake, "bt_host_wake");
	if (ret) goto fail_gpio_wake;
	ret = gpio_tlmm_config(GPIO_CFG(bt.pdata->gpio_wake, 0, GPIO_CFG_OUTPUT,
			GPIO_CFG_NO_PULL, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	if (ret) goto fail_gpio_host;
	ret = gpio_tlmm_config(GPIO_CFG(bt.pdata->gpio_host_wake, 0, GPIO_CFG_INPUT,
			GPIO_CFG_NO_PULL, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	if (ret) goto fail_gpio_host;
	ret = gpio_direction_output(bt.pdata->gpio_wake, 0);
	if (ret) goto fail_gpio_host;
	ret = gpio_direction_input(bt.pdata->gpio_host_wake);
	if (ret) goto fail_gpio_host;
	bt.irq = gpio_to_irq(bt.pdata->gpio_host_wake);
	ret = request_threaded_irq(bt.irq, NULL, host_wake_thread,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			"bt_host_wake", &bt);
	if (ret) goto fail_gpio_host;
	ret = irq_set_irq_wake(bt.irq, 1);
	if (ret) goto fail_irq;
	ret = device_create_file(&pdev->dev, &dev_attr_state);
	if (ret) goto fail_irq_wake;
	bt.ready = true;
	dev_info(&pdev->dev, "BT_WAKE GPIO%u, HOST_WAKE GPIO%u IRQ%d ready\n",
		bt.pdata->gpio_wake, bt.pdata->gpio_host_wake, bt.irq);
	return 0;
fail_irq_wake:
	irq_set_irq_wake(bt.irq, 0);
fail_irq:
	free_irq(bt.irq, &bt);
fail_gpio_host:
	gpio_free(bt.pdata->gpio_host_wake);
fail_gpio_wake:
	gpio_free(bt.pdata->gpio_wake);
fail_wq:
	destroy_workqueue(bt.wq);
fail_wake:
	wake_lock_destroy(&bt.wake_lock);
	return ret;
}

static struct platform_driver bcm_bt_lpm_driver = {
	.probe = bcm_bt_lpm_probe,
	.driver = { .name = "bcm_bt_lpm", .owner = THIS_MODULE },
};
static int __init bcm_bt_lpm_init(void)
{
	return platform_driver_register(&bcm_bt_lpm_driver);
}
module_init(bcm_bt_lpm_init);
MODULE_DESCRIPTION("MiOne Broadcom Bluetooth wake handshake");
MODULE_LICENSE("GPL");
