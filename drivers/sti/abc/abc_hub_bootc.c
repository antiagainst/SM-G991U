/* abc_hub_bootc.c
 *
 * Abnormal Behavior Catcher Hub Driver Sub Module(Booting Time Check)
 *
 * Copyright (C) 2017 Samsung Electronics
 *
 * Sangsu Ha <sangsu.ha@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/sti/abc_hub.h>
#include <linux/workqueue.h>

char bootc_offset_module[BOOTC_OFFSET_DATA_CNT][BOOTC_OFFSET_STR_MAX] = {"fsck"};

static int abc_hub_bootc_get_total_offset(struct sub_bootc_pdata *bootc_pdata)
{
	int total_offset = 0;
	int i;

	for (i = 0; i < BOOTC_OFFSET_DATA_CNT; i++) {
		total_offset += bootc_pdata->offset_data[i].offset;
	}

	return total_offset;
}

static void abc_hub_bootc_work_func(struct work_struct *work)
{
	struct sub_bootc_pdata *bootc_pdata = container_of(work, struct sub_bootc_pdata, bootc_work.work);
	int fixed_time_spec;

	bootc_pdata->time_spec_offset = abc_hub_bootc_get_total_offset(bootc_pdata);
	fixed_time_spec = bootc_pdata->time_spec + bootc_pdata->time_spec_offset;

	pr_info("%s: bootc_time : %d, time_spec : %d(%d + %d))\n",
		__func__, bootc_pdata->bootc_time, fixed_time_spec,
		bootc_pdata->time_spec, bootc_pdata->time_spec_offset);

	if (bootc_pdata->bootc_time < 0) {
		pr_err("%s: boot_time_parse fail(%d)\n", __func__, bootc_pdata->bootc_time);
	} else {
#if IS_ENABLED(CONFIG_SEC_ABC_MOTTO)
		motto_send_bootcheck_info(bootc_pdata->bootc_time / MSEC_PER_SEC);
#endif
		if (bootc_pdata->bootc_time > fixed_time_spec) {
			pr_info("%s: booting time is spec out\n", __func__);
			abc_hub_send_event("MODULE=bootc@ERROR=boot_time_fail");
		}
	}
}

int parse_bootc_data(struct device *dev,
		     struct abc_hub_platform_data *pdata,
		     struct device_node *np)
{
	struct device_node *bootc_np;

	bootc_np = of_find_node_by_name(np, "bootc");

#if IS_ENABLED(CONFIG_SEC_FACTORY)
	if (of_property_read_u32(bootc_np, "bootc,time_spec_fac", &pdata->bootc_pdata.time_spec)) {
		dev_err(dev, "Failed to get bootc,time_spec_fac: node not exist\n");
		return -EINVAL;
	}
	pr_info("%s: time_spec(factory binary) - %d\n", __func__, pdata->bootc_pdata.time_spec);
#elif IS_ENABLED(CONFIG_SEC_ABC_HUB_BOOTC_ENG)
	if (of_property_read_u32(bootc_np, "bootc,time_spec_eng", &pdata->bootc_pdata.time_spec)) {
		dev_err(dev, "Failed to get bootc,time_spec_eng: node not exist\n");
		return -EINVAL;
	}
	pr_info("%s: time_spec(user binary eng build) - %d\n", __func__, pdata->bootc_pdata.time_spec);
#else
	if (of_property_read_u32(bootc_np, "bootc,time_spec_user", &pdata->bootc_pdata.time_spec)) {
		dev_err(dev, "Failed to get bootc,time_spec_user: node not exist\n");
		return -EINVAL;
	}
	pr_info("%s: time_spec(user binary user build) - %d\n", __func__, pdata->bootc_pdata.time_spec);
#endif

	return 0;
}

void abc_hub_bootc_enable(struct device *dev, int enable)
{
	/* common sequence */
	struct abc_hub_info *pinfo = dev_get_drvdata(dev);

	pinfo->pdata->bootc_pdata.enabled = enable;

	/* custom sequence */
	pr_info("%s: enable(%d)\n", __func__, enable);
	if (enable == ABC_HUB_ENABLED)
		queue_delayed_work(pinfo->pdata->bootc_pdata.workqueue,
				   &pinfo->pdata->bootc_pdata.bootc_work, msecs_to_jiffies(2000));
}

int abc_hub_bootc_init(struct device *dev)
{
	struct abc_hub_info *pinfo = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < BOOTC_OFFSET_DATA_CNT; i++) {
		strcpy(pinfo->pdata->bootc_pdata.offset_data[i].module, bootc_offset_module[i]);
		pinfo->pdata->bootc_pdata.offset_data[i].offset = 0;
	}
	pinfo->pdata->bootc_pdata.bootc_time = -1;

	INIT_DELAYED_WORK(&pinfo->pdata->bootc_pdata.bootc_work, abc_hub_bootc_work_func);

	pinfo->pdata->bootc_pdata.workqueue = create_singlethread_workqueue("bootc_wq");
	if (!pinfo->pdata->bootc_pdata.workqueue) {
		pr_err("%s: fail\n", __func__);
		return -1;
	}

	pr_info("%s: success\n", __func__);
	return 0;
}

MODULE_DESCRIPTION("Samsung ABC Hub Sub Module(bootc) Driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
