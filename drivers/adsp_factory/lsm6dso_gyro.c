/*
 *  Copyright (C) 2012, Samsung Electronics Co. Ltd. All Rights Reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */
#include <linux/init.h>
#include <linux/module.h>
#include "adsp.h"
#define VENDOR "STM"
#define CHIP_ID "LSM6DSO"
#define ST_PASS 1
#define ST_FAIL 0
#define STARTUP_BIT_FAIL 2
#define OIS_ST_BIT_SET 3
#define G_ZRL_DELTA_FAIL 4
#define SELFTEST_REVISED 1

static ssize_t gyro_vendor_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", VENDOR);
}

static ssize_t gyro_name_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", CHIP_ID);
}

static ssize_t selftest_revised_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", SELFTEST_REVISED);
}

static ssize_t gyro_power_off(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	pr_info("[FACTORY]: %s\n", __func__);

	return snprintf(buf, PAGE_SIZE, "%d\n", 1);
}

static ssize_t gyro_power_on(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	pr_info("[FACTORY]: %s\n", __func__);

	return snprintf(buf, PAGE_SIZE, "%d\n", 1);
}

static ssize_t gyro_temp_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint8_t cnt = 0;

	adsp_unicast(NULL, 0, MSG_GYRO_TEMP, 0, MSG_TYPE_GET_RAW_DATA);

	while (!(data->ready_flag[MSG_TYPE_GET_RAW_DATA] & 1 << MSG_GYRO_TEMP)
		&& cnt++ < TIMEOUT_CNT)
		msleep(20);

	data->ready_flag[MSG_TYPE_GET_RAW_DATA] &= ~(1 << MSG_GYRO_TEMP);

	if (cnt >= TIMEOUT_CNT) {
		pr_err("[FACTORY] %s: Timeout!!!\n", __func__);
		return snprintf(buf, PAGE_SIZE, "-99\n");
	}

	pr_info("[FACTORY] %s: gyro_temp = %d\n", __func__,
		data->msg_buf[MSG_GYRO_TEMP][0]);

	return snprintf(buf, PAGE_SIZE, "%d\n",
		data->msg_buf[MSG_GYRO_TEMP][0]);
}

static ssize_t gyro_selftest_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint8_t cnt = 0;
	int st_diff_res = ST_FAIL;
	int st_zro_res = ST_FAIL;

	pr_info("[FACTORY] %s - start", __func__);
	adsp_unicast(NULL, 0, MSG_GYRO, 0, MSG_TYPE_ST_SHOW_DATA);

	while (!(data->ready_flag[MSG_TYPE_ST_SHOW_DATA] & 1 << MSG_GYRO) &&
		cnt++ < TIMEOUT_CNT)
		msleep(25);

	data->ready_flag[MSG_TYPE_ST_SHOW_DATA] &= ~(1 << MSG_GYRO);

	if (cnt >= TIMEOUT_CNT) {
		pr_err("[FACTORY] %s: Timeout!!!\n", __func__);
#ifdef CONFIG_SEC_FACTORY
		panic("force crash : sensor selftest timeout\n");
#endif
		return snprintf(buf, PAGE_SIZE,
			"0,0,0,0,0,0,0,0,0,0,0,0,%d,%d\n",
			ST_FAIL, ST_FAIL);
	}

	if (data->msg_buf[MSG_GYRO][1] != 0) {
		pr_info("[FACTORY] %s - failed(%d, %d)\n", __func__,
			data->msg_buf[MSG_GYRO][1],
			data->msg_buf[MSG_GYRO][5]);

		pr_info("[FACTORY]: %s - %d,%d,%d\n", __func__,
			data->msg_buf[MSG_GYRO][2],
			data->msg_buf[MSG_GYRO][3],
			data->msg_buf[MSG_GYRO][4]);

		if (data->msg_buf[MSG_GYRO][5] == OIS_ST_BIT_SET)
			pr_info("[FACTORY] %s - OIS_ST_BIT fail\n", __func__);
		else if (data->msg_buf[MSG_GYRO][5] == G_ZRL_DELTA_FAIL)
			pr_info("[FACTORY] %s - ZRL Delta fail\n", __func__);

		return snprintf(buf, PAGE_SIZE, "%d,%d,%d\n",
			data->msg_buf[MSG_GYRO][2],
			data->msg_buf[MSG_GYRO][3],
			data->msg_buf[MSG_GYRO][4]);
	} else {
		st_zro_res = ST_PASS;
	}

	if (!data->msg_buf[MSG_GYRO][5])
		st_diff_res = ST_PASS;
	else if (data->msg_buf[MSG_GYRO][5] == STARTUP_BIT_FAIL)
		pr_info("[FACTORY] %s - Gyro Start Up Bit fail\n", __func__);

	pr_info("[FACTORY] %s - %d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
		__func__,
		data->msg_buf[MSG_GYRO][2], data->msg_buf[MSG_GYRO][3],
		data->msg_buf[MSG_GYRO][4], data->msg_buf[MSG_GYRO][6],
		data->msg_buf[MSG_GYRO][7], data->msg_buf[MSG_GYRO][8],
		data->msg_buf[MSG_GYRO][9], data->msg_buf[MSG_GYRO][10],
		data->msg_buf[MSG_GYRO][11], data->msg_buf[MSG_GYRO][12],
		data->msg_buf[MSG_GYRO][13], data->msg_buf[MSG_GYRO][14],
		st_diff_res, st_zro_res);

	return snprintf(buf, PAGE_SIZE,
		"%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
		data->msg_buf[MSG_GYRO][2], data->msg_buf[MSG_GYRO][3],
		data->msg_buf[MSG_GYRO][4], data->msg_buf[MSG_GYRO][6],
		data->msg_buf[MSG_GYRO][7], data->msg_buf[MSG_GYRO][8],
		data->msg_buf[MSG_GYRO][9], data->msg_buf[MSG_GYRO][10],
		data->msg_buf[MSG_GYRO][11], data->msg_buf[MSG_GYRO][12],
		data->msg_buf[MSG_GYRO][13], data->msg_buf[MSG_GYRO][14],
		st_diff_res, st_zro_res);
}

static ssize_t trimmed_odr_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint8_t cnt = 0;

	adsp_unicast(NULL, 0, MSG_GYRO, 0, MSG_TYPE_GET_REGISTER);

	while (!(data->ready_flag[MSG_TYPE_GET_REGISTER] & 1 << MSG_GYRO)
		&& cnt++ < TIMEOUT_CNT)
		msleep(20);

	data->ready_flag[MSG_TYPE_GET_REGISTER] &= ~(1 << MSG_GYRO);

	if (cnt >= TIMEOUT_CNT) {
		pr_err("[FACTORY] %s: Timeout!!!\n", __func__);
		return snprintf(buf, PAGE_SIZE, "0\n");
	}

	pr_info("[FACTORY] %s: 0x63h = 0x%02x, trimmed_odr = %d Hz\n", __func__,
		data->msg_buf[MSG_GYRO][0], data->msg_buf[MSG_GYRO][1]);

	return snprintf(buf, PAGE_SIZE, "%d\n",
		data->msg_buf[MSG_GYRO][1]);
}

static DEVICE_ATTR(name, 0444, gyro_name_show, NULL);
static DEVICE_ATTR(vendor, 0444, gyro_vendor_show, NULL);
static DEVICE_ATTR(selftest, 0440, gyro_selftest_show, NULL);
static DEVICE_ATTR(power_on, 0444, gyro_power_on, NULL);
static DEVICE_ATTR(power_off, 0444, gyro_power_off, NULL);
static DEVICE_ATTR(temperature, 0440, gyro_temp_show, NULL);
static DEVICE_ATTR(selftest_revised, 0440, selftest_revised_show, NULL);
static DEVICE_ATTR(trimmed_odr, 0440, trimmed_odr_show, NULL);

static struct device_attribute *gyro_attrs[] = {
	&dev_attr_name,
	&dev_attr_vendor,
	&dev_attr_selftest,
	&dev_attr_power_on,
	&dev_attr_power_off,
	&dev_attr_temperature,
	&dev_attr_selftest_revised,
	&dev_attr_trimmed_odr,
	NULL,
};

static int __init lsm6dso_gyro_factory_init(void)
{
	adsp_factory_register(MSG_GYRO, gyro_attrs);

	pr_info("[FACTORY] %s\n", __func__);

	return 0;
}

static void __exit lsm6dso_gyro_factory_exit(void)
{
	adsp_factory_unregister(MSG_GYRO);

	pr_info("[FACTORY] %s\n", __func__);
}

module_init(lsm6dso_gyro_factory_init);
module_exit(lsm6dso_gyro_factory_exit);
