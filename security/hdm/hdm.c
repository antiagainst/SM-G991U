/*
 * @file hdm.c
 * @brief HDM Support
 * Copyright (c) 2020, Samsung Electronics Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/mm.h>
#if defined(CONFIG_ARCH_QCOM)
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/resource.h>
#endif
#include <linux/hdm.h>
#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/fastuh.h>
#include <linux/sec_class.h>
#if defined(CONFIG_ARCH_QCOM)
#include <linux/sched/signal.h>
#include <soc/qcom/secure_buffer.h>
#include <linux/qtee_shmbridge.h>
#endif

#include "hdm_log.h"

int hdm_log_level = HDM_LOG_LEVEL;
void hdm_printk(int level, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	if (hdm_log_level < level)
		return;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	printk(KERN_INFO "%s %pV", TAG, &vaf);

	va_end(args);
}

static ssize_t store_hdm_policy(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned long mode = HDM_CMD_MAX;
	int c, p;

	if (count == 0) {
		hdm_err("%s count = 0\n", __func__);
		goto error;
	}

	if (kstrtoul(buf, 0, &mode)) {
		goto error;
	};

	if (mode > HDM_CMD_MAX) {
		hdm_err("%s command size max fail. %d\n", __func__, mode);
		goto error;
	}
	hdm_info("%s: command id: 0x%x\n", __func__, (int)mode);

	c = (int)(mode & HDM_C_BITMASK);
	p = (int)(mode & HDM_P_BITMASK);

	hdm_info("%s m:0x%x c:0x%x p:0x%x\n", __func__, (int)mode, c, p);
	switch (c) {
#if defined(CONFIG_ARCH_QCOM)
	case HDM_HYP_CALL:
		hdm_info("%s HDM_HYP_CALL\n", __func__);
		fastuh_call(FASTUH_APP_HDM, 9, 0, p, 0, 0);
		break;
	case HDM_HYP_CALLP:
		hdm_info("%s HDM_HYP_CALLP\n", __func__);
		fastuh_call(FASTUH_APP_HDM, 2, 0, p, 0, 0);
		break;
#endif
	default:
		goto error;
	}
error:
	return count;
}
static DEVICE_ATTR(hdm_policy, 0220, NULL, store_hdm_policy);

#if defined(CONFIG_ARCH_QCOM)
static uint64_t qseelog_shmbridge_handle;
static int __init __hdm_init_of(void)
{
	struct device_node *node;
	struct resource r;
	int ret;
	phys_addr_t addr;
	u64 size;

	uint32_t ns_vmids[] = {VMID_HLOS};
	uint32_t ns_vm_perms[] = {PERM_READ | PERM_WRITE};
	uint32_t ns_vm_nums = 1;

	int src_vmids[1] = {VMID_HLOS};
	int dest_vmids[1] = {VMID_CP_BITSTREAM};
	int dest_perms[1] = {PERM_READ | PERM_WRITE};

	hdm_info("%s start\n", __func__);

	node = of_find_node_by_name(NULL, "samsung,sec_hdm");
	if (!node) {
		hdm_err("failed of_find_node_by_name\n");
		return -ENODEV;
	}

	node = of_parse_phandle(node, "memory-region", 0);
	if (!node) {
		hdm_err("no memory-region specified\n");
		return -EINVAL;
	}


	ret = of_address_to_resource(node, 0, &r);
	if (ret) {
		hdm_err("failed of_address_to_resource\n");
		return ret;
	}

	addr = r.start;
	size = resource_size(&r);

	ret = qtee_shmbridge_register(addr, size, ns_vmids, ns_vm_perms,
			ns_vm_nums, PERM_READ | PERM_WRITE, &qseelog_shmbridge_handle);
	if (ret)
		hdm_err("failed to create bridge for qsee_log buffer\n");

	ret = hyp_assign_phys(addr, size, src_vmids, 1, dest_vmids, dest_perms, 1);
	if (ret) {
		hdm_err("%s: failed for %pa address of size %zx rc:%d\n",
				__func__, &addr, size, ret);
	}

	hdm_info("%s done\n", __func__);
	return 0;
}
#endif

static int __init hdm_test_init(void)
{
	struct device *dev;
#if defined(CONFIG_ARCH_QCOM)
	int err;
#endif
	dev = sec_device_create(NULL, "hdm");
	WARN_ON(!dev);
	if (IS_ERR(dev))
		hdm_err("%s Failed to create devce\n", __func__);

	if (device_create_file(dev, &dev_attr_hdm_policy) < 0)
		hdm_err("%s Failed to create device file\n", __func__);

#if defined(CONFIG_ARCH_QCOM)
	err = __hdm_init_of();
	if (err)
		return err;
#endif

	hdm_info("%s end\n", __func__);
	return 0;
}

module_init(hdm_test_init);
