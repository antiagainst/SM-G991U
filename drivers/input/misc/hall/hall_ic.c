/*
 *
 * Copyright 2017 SEC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/pm.h>
#include <linux/sysctl.h>
#include <linux/proc_fs.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of.h>
#include <linux/spinlock.h>
#include <linux/version.h>
#include <linux/pm_wakeup.h>
#if IS_ENABLED(CONFIG_DRV_SAMSUNG)
#include <linux/sec_class.h>
#endif
#if IS_ENABLED(CONFIG_SEC_FACTORY)
#include <linux/delay.h>
#endif

/*
 * Switch events
 */
#define SW_FLIP                 0x15  /* set = flip cover open, close*/
#define SW_CERTIFYHALL          0x1b  /* set = certify_hall attach/detach */
#define SW_HALL_LOGICAL         0x1e  /* set = logical hall ic attach/detach */

struct hall_ic_data {
	struct delayed_work dwork;
	struct wakeup_source *ws;
	struct input_dev *input;
	struct list_head list;
	int gpio;
	int irq;
	int state;
	int active_low;
	unsigned int event;
	const char *name;
};

struct hall_ic_pdata {
	struct hall_ic_data *hall;
	unsigned int nhalls;
};

struct hall_ic_drvdata {
	struct hall_ic_pdata *pdata;
	struct work_struct work;
	struct device *dev;
#if IS_ENABLED(CONFIG_DRV_SAMSUNG)
	struct device *sec_dev;
#endif
};

static LIST_HEAD(hall_ic_list);

#if IS_ENABLED(CONFIG_DRV_SAMSUNG)
struct hall_ic_drvdata *gddata;
/*
 * the sysfs just returns the level of the gpio
 */
static ssize_t hall_detect_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct hall_ic_data *hall;

	list_for_each_entry(hall, &hall_ic_list, list) {
		if (hall->event != SW_FLIP)
			continue;
		if (!!gpio_get_value_cansleep(hall->gpio))
			sprintf(buf, "OPEN\n");
		else
			sprintf(buf, "CLOSE\n");
	}
	return strlen(buf);
}
static ssize_t certify_hall_detect_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct hall_ic_data *hall;

	list_for_each_entry(hall, &hall_ic_list, list) {
		if (hall->event != SW_CERTIFYHALL)
			continue;
		if (!!gpio_get_value_cansleep(hall->gpio))
			sprintf(buf, "OPEN\n");
		else
			sprintf(buf, "CLOSE\n");
	}

	return strlen(buf);
}
static ssize_t hall_number_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct hall_ic_drvdata *ddata = dev_get_drvdata(dev);

	sprintf(buf, "%u\n", ddata->pdata->nhalls);

	return strlen(buf);
}
static DEVICE_ATTR_RO(hall_detect);
static DEVICE_ATTR_RO(certify_hall_detect);
static DEVICE_ATTR_RO(hall_number);

static struct device_attribute *hall_ic_attrs[] = {
	&dev_attr_hall_detect,
	&dev_attr_certify_hall_detect,
	NULL,
};
#endif

#if IS_ENABLED(CONFIG_SEC_FACTORY)
static void hall_ic_work(struct work_struct *work)
{
	struct hall_ic_data *hall = container_of(work,
		struct hall_ic_data, dwork.work);
	struct hall_ic_drvdata *ddata = gddata;
	int first, second, state;
	char hall_uevent[20] = {0,};
	char *hall_status[2] = {hall_uevent, NULL};

	first = gpio_get_value_cansleep(hall->gpio);
	msleep(50);
	second = gpio_get_value_cansleep(hall->gpio);
	if (first == second) {
		hall->state = first;
		state = first ^ hall->active_low;
		pr_info("%s %s\n", hall->name,
			hall->state ? "open" : "close");

		if (hall->input) {
			input_report_switch(hall->input, hall->event, state);
			input_sync(hall->input);
		}

		/* send uevent for hall ic */
		snprintf(hall_uevent, sizeof(hall_uevent), "%s=%s",
			hall->name, hall->state ? "open" : "close");
		kobject_uevent_env(&ddata->sec_dev->kobj, KOBJ_CHANGE, hall_status);
	} else
		pr_info("%s %d,%d\n", hall->name,
			first, second);
}
#else
static void hall_ic_work(struct work_struct *work)
{
	struct hall_ic_data *hall = container_of(work,
		struct hall_ic_data, dwork.work);
	int state;

	hall->state = !!gpio_get_value_cansleep(hall->gpio);
	state = hall->state ^ hall->active_low;
	pr_info("%s %s %s(%d)\n", __func__, hall->name,
		hall->state ? "open" : "close", hall->state);

	if (hall->input) {
		input_report_switch(hall->input, hall->event, state);
		input_sync(hall->input);
	}
}
#endif

static irqreturn_t hall_ic_detect(int irq, void *dev_id)
{
	struct hall_ic_data *hall = dev_id;
	int state = !!gpio_get_value_cansleep(hall->gpio);

	pr_info("%s %s(%d)\n", __func__,
		hall->name, state);
	cancel_delayed_work_sync(&hall->dwork);
#if IS_ENABLED(CONFIG_SEC_FACTORY)
	schedule_delayed_work(&hall->dwork, HZ / 20);
#else
	if (state) {
		__pm_wakeup_event(hall->ws, jiffies_to_msecs(HZ / 20));
		schedule_delayed_work(&hall->dwork, HZ  / 100);
	} else {
		__pm_relax(hall->ws);
		schedule_delayed_work(&hall->dwork, 0);
	}
#endif
	return IRQ_HANDLED;
}

static int hall_ic_open(struct input_dev *input)
{
	struct hall_ic_data *hall = input_get_drvdata(input);

	pr_info("%s: %s\n", __func__, hall->name);

	schedule_delayed_work(&hall->dwork, HZ / 2);
	input_sync(input);

	return 0;
}

static void hall_ic_close(struct input_dev *input)
{
}

static int hall_ic_input_dev_register(struct hall_ic_data *hall)
{
	struct input_dev *input;
	int ret = 0;

	input = input_allocate_device();
	if (!input) {
		pr_err("failed to allocate state\n");
		return -ENOMEM;
	}

	hall->input = input;
	input_set_capability(input, EV_SW, hall->event);
	input->name = hall->name;
	input->phys = hall->name;
	input->open = hall_ic_open;
	input->close = hall_ic_close;

	ret = input_register_device(input);
	if (ret) {
		pr_err("failed to register input device\n");
		return ret;
	}

	input_set_drvdata(input, hall);

	return 0;
}

static int hall_ic_setup_halls(struct hall_ic_drvdata *ddata)
{
	struct hall_ic_data *hall;
	int ret = 0;
#if IS_ENABLED(CONFIG_DRV_SAMSUNG)
	int i = 0;

	gddata = ddata;
	ddata->sec_dev = sec_device_create(ddata, "hall_ic");

	ret = sysfs_create_file(&ddata->sec_dev->kobj,
						&dev_attr_hall_number.attr);
#endif
	list_for_each_entry(hall, &hall_ic_list, list) {
		hall->state = gpio_get_value_cansleep(hall->gpio);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
		// 4.19 R
		wakeup_source_init(hall->ws, "hall_ic_wlock");
		// 4.19 Q
		if (!(hall->ws)) {
			hall->ws = wakeup_source_create("hall_ic_wlock");
			if (hall->ws)
				wakeup_source_add(hall->ws);
		}
#else
		hall->ws = wakeup_source_register(NULL, "hall_ic_wlock");
#endif
		INIT_DELAYED_WORK(&hall->dwork, hall_ic_work);
		ret = request_threaded_irq(hall->irq, NULL, hall_ic_detect,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				hall->name, hall);
		if (ret < 0)
			pr_err("failed to request irq %d(%d)\n",
				hall->irq, ret);

#if IS_ENABLED(CONFIG_DRV_SAMSUNG)
		if (!ddata->sec_dev)
			continue;

		for (i = 0; i < ARRAY_SIZE(hall_ic_attrs); i++) {
			if (!strncmp(hall->name, hall_ic_attrs[i]->attr.name,
					strlen(hall->name))) {
				ret = sysfs_create_file(&ddata->sec_dev->kobj,
						&hall_ic_attrs[i]->attr);
				if (ret < 0)
					pr_err("failed to create sysfr %d(%d)\n",
						hall->irq, ret);
				break;
			}
		}
#endif
	}
	return ret;
}

static struct hall_ic_pdata *hall_ic_parsing_dt(struct device *dev)
{
	struct device_node *node = dev->of_node, *pp;
	struct hall_ic_pdata *pdata;
	struct hall_ic_data *hall;
	int nhalls = 0, ret = 0, i = 0;

	if (!node)
		return ERR_PTR(-ENODEV);

	nhalls = of_get_child_count(node);
	if (nhalls == 0)
		return ERR_PTR(-ENODEV);

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	pdata->hall = devm_kzalloc(dev, nhalls * sizeof(*hall), GFP_KERNEL);
	if (!pdata->hall)
		return ERR_PTR(-ENOMEM);

	pdata->nhalls = nhalls;
	for_each_child_of_node(node, pp) {
		struct hall_ic_data *hall = &pdata->hall[i++];
		enum of_gpio_flags flags;

		hall->gpio = of_get_gpio_flags(pp, 0, &flags);
		if (hall->gpio < 0) {
			ret = hall->gpio;
			if (ret) {
				pr_err("Failed to get gpio flags %d\n", ret);
				return ERR_PTR(ret);
			}
		}
		hall->active_low = flags & OF_GPIO_ACTIVE_LOW;
		hall->irq = gpio_to_irq(hall->gpio);
		hall->name = of_get_property(pp, "name", NULL);

		pr_info("%s %s\n", __func__, hall->name);

		if (of_property_read_u32(pp, "event", &hall->event)) {
			pr_err("failed to get event: 0x%x\n", hall->event);
			return ERR_PTR(-EINVAL);
		}
		list_add(&hall->list, &hall_ic_list);
	}
	return pdata;
}

static int hall_ic_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hall_ic_pdata *pdata = dev_get_platdata(dev);
	struct hall_ic_drvdata *ddata;
	struct hall_ic_data *hall;
	int ret = 0;

	if (!pdata) {
		pdata = hall_ic_parsing_dt(dev);
		if (IS_ERR(pdata)) {
			pr_err("%s : fail to get the DT\n", __func__);
			goto fail1;
		}
	}

	ddata = devm_kzalloc(dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata) {
		pr_err("failed to allocate drvdata\n");
		ret = -ENOMEM;
		goto fail1;
	}

	ddata->pdata = pdata;
	device_init_wakeup(&pdev->dev, true);
	platform_set_drvdata(pdev, ddata);

	list_for_each_entry(hall, &hall_ic_list, list) {
		ret = hall_ic_input_dev_register(hall);
		if (ret) {
			pr_err("hall_ic_input_dev_register failed %d\n", ret);
			goto fail2;
		}

		hall->input->dev.parent = &pdev->dev;
	}

	ret = hall_ic_setup_halls(ddata);
	if (ret) {
		pr_err("failed to set up hall : %d\n", ret);
		goto fail2;
	}

	return 0;

fail2:
	platform_set_drvdata(pdev, NULL);
fail1:
	return ret;
}

static int hall_ic_remove(struct platform_device *pdev)
{
	struct hall_ic_data *hall;

	list_for_each_entry(hall, &hall_ic_list, list) {
		input_unregister_device(hall->input);
		wakeup_source_unregister(hall->ws);
	}
	device_init_wakeup(&pdev->dev, 0);
	platform_set_drvdata(pdev, NULL);
	return 0;
}

static const struct of_device_id hall_ic_dt_ids[] = {
	{ .compatible = "hall_ic", },
	{ },
};
MODULE_DEVICE_TABLE(of, hall_ic_dt_ids);

static int hall_ic_suspend(struct device *dev)
{
	struct hall_ic_data *hall;

	list_for_each_entry(hall, &hall_ic_list, list)
		enable_irq_wake(hall->irq);

	return 0;
}

static int hall_ic_resume(struct device *dev)
{
	struct hall_ic_data *hall;

	list_for_each_entry(hall, &hall_ic_list, list) {
		int state = !!gpio_get_value_cansleep(hall->gpio);

		state ^= hall->active_low;
		pr_info("%s %s %s(%d)\n", __func__, hall->name,
			hall->state ? "open" : "close", hall->state);
		disable_irq_wake(hall->irq);
		input_report_switch(hall->input, hall->event, state);
		input_sync(hall->input);
	}
	return 0;
}

static SIMPLE_DEV_PM_OPS(hall_ic_pm_ops,
	hall_ic_suspend, hall_ic_resume);

static struct platform_driver hall_ic_device_driver = {
	.probe		= hall_ic_probe,
	.remove		= hall_ic_remove,
	.driver		= {
		.name	= "hall_ic",
		.owner	= THIS_MODULE,
		.pm		= &hall_ic_pm_ops,
		.of_match_table	= hall_ic_dt_ids,
	}
};

static int __init hall_ic_init(void)
{
	return platform_driver_register(&hall_ic_device_driver);
}

static void __exit hall_ic_exit(void)
{
	platform_driver_unregister(&hall_ic_device_driver);
}

late_initcall(hall_ic_init);
module_exit(hall_ic_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Hall IC driver for SEC covers");
