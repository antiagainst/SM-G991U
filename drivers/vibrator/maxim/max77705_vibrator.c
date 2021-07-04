/*
 * haptic motor driver for max77705_vibrator.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) "[VIB] max77705_vib: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pwm.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/i2c.h>
#include <linux/regulator/consumer.h>
#include <linux/mfd/max77705.h>
#include <linux/mfd/max77705-private.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/vibrator/sec_vibrator.h>

#define MOTOR_LRA                       (1<<7)
#define MOTOR_EN                        (1<<6)
#define EXT_PWM                         (0<<5)
#define DIVIDER_128                     (1<<1)
#define MRDBTMER_MASK                   (0x7)
#define MREN                            (1 << 3)
#define BIASEN                          (1 << 7)

/* 1s / 128(divider value) * 10(150Hz -> 1500 so multiply 10) = 78125000 ns */
#define FREQ_DIVIDER			78125000

struct max77705_vibrator_drvdata {
	struct max77705_dev *max77705;
	struct sec_vibrator_drvdata sec_vib_ddata;
	struct i2c_client *i2c;
	struct max77705_vibrator_pdata *pdata;
	struct pwm_device *pwm;
	struct regulator *regulator;
	int ratio;
	int max_duty;
	int period;
	int duty;
};

static int max77705_vib_set_ratio_with_temp(struct device *dev, int temperature)
{
	struct max77705_vibrator_drvdata *ddata = dev_get_drvdata(dev);
	struct max77705_vibrator_pdata *pdata = ddata->pdata;

	if (temperature >= pdata->high_temp_ref)
		ddata->ratio = pdata->high_temp_ratio;
	else
		ddata->ratio = pdata->normal_ratio;

	pr_info("temperature is %d, duty ratio set to %d\n", temperature, ddata->ratio);

	return 0;
}

static int max77705_vibrator_i2c_enable(struct max77705_vibrator_drvdata *ddata,
		bool en)
{
	int ret = max77705_update_reg(ddata->i2c,
			MAX77705_PMIC_REG_MCONFIG,
			en ? 0xff : 0x0, MOTOR_LRA | MOTOR_EN);
	if (ret)
		pr_err("i2c LRA and EN update error %d\n", ret);

	return ret;
}

static void max77705_vibrator_init_reg(
		struct max77705_vibrator_drvdata *ddata)
{
	int ret = 0;

	ret = max77705_update_reg(ddata->i2c,
			MAX77705_PMIC_REG_MAINCTRL1, 0xff, BIASEN);
	if (ret)
		pr_err("i2c REG_BIASEN update error %d\n", ret);

	ret = max77705_update_reg(ddata->i2c,
		MAX77705_PMIC_REG_MCONFIG, 0xff, MOTOR_LRA);
	if (ret)
		pr_err("i2c MOTOR_LPA update error %d\n", ret);

	ret = max77705_update_reg(ddata->i2c,
		MAX77705_PMIC_REG_MCONFIG, 0xff, DIVIDER_128);
	if (ret)
		pr_err("i2c DIVIDER_128 update error %d\n", ret);
}

static int max77705_vibrator_enable(struct device *dev, bool en)
{
	struct max77705_vibrator_drvdata *ddata = dev_get_drvdata(dev);
	int ret = 0;

	ret = max77705_vibrator_i2c_enable(ddata, en);

	return ret;
}

static int max77705_vib_set_intensity(struct device *dev, int intensity)
{
	struct max77705_vibrator_drvdata *ddata = dev_get_drvdata(dev);
	int duty = ddata->period >> 1;
	int ret = 0;

	if (intensity < -(MAX_INTENSITY) || MAX_INTENSITY < intensity) {
		pr_err("%s out of range %d\n", __func__, intensity);
		return -EINVAL;
	}

	if (intensity == MAX_INTENSITY)
		duty = ddata->max_duty;
	else if (intensity == -(MAX_INTENSITY))
		duty = ddata->period - ddata->max_duty;
	else if (intensity != 0)
		duty += (ddata->max_duty - duty) * intensity / MAX_INTENSITY;

	ddata->duty = duty;

	ret = pwm_config(ddata->pwm, duty, ddata->period);
	if (ret < 0) {
		pr_err("failed to config pwm %d\n", ret);
		return ret;
	}
	if (duty != ddata->period >> 1) {
		ret = pwm_enable(ddata->pwm);
		if (ret < 0)
			pr_err("failed to enable pwm %d\n", ret);
	} else {
		pwm_disable(ddata->pwm);
	}

	return ret;
}

static int max77705_vib_set_frequency(struct device *dev, int num)
{
	struct max77705_vibrator_drvdata *ddata = dev_get_drvdata(dev);

	if (num >= 0 && num < ddata->pdata->freq_nums) {
		ddata->period = FREQ_DIVIDER / ddata->pdata->freq_array[num];
		ddata->duty = ddata->max_duty =
			(ddata->period * ddata->ratio) / 100;
	} else if (num >= HAPTIC_ENGINE_FREQ_MIN &&
			num <= HAPTIC_ENGINE_FREQ_MAX) {
		ddata->period = FREQ_DIVIDER / num;
		ddata->duty = ddata->max_duty =
			(ddata->period * ddata->ratio) / 100;
	} else {
		pr_err("%s out of range %d\n", __func__, num);
		return -EINVAL;
	}

	return 0;
}

static int max77705_vib_set_overdrive(struct device *dev, bool en)
{
	struct max77705_vibrator_drvdata *ddata = dev_get_drvdata(dev);

	if (ddata->pdata->overdrive_ratio)
		ddata->ratio = en ? ddata->pdata->overdrive_ratio :
			ddata->pdata->normal_ratio;

	return 0;
}

static int max77705_vib_get_motor_type(struct device *dev, char *buf)
{
	struct max77705_vibrator_drvdata *ddata = dev_get_drvdata(dev);
	int ret = snprintf(buf, VIB_BUFSIZE, "%s\n", ddata->pdata->motor_type);

	return ret;
}

static const struct sec_vibrator_ops max77705_multi_freq_vib_ops = {
	.enable = max77705_vibrator_enable,
	.set_intensity = max77705_vib_set_intensity,
	.set_frequency = max77705_vib_set_frequency,
	.set_overdrive = max77705_vib_set_overdrive,
	.set_force_touch_intensity = max77705_vib_set_intensity,
	.get_motor_type = max77705_vib_get_motor_type,
	.set_tuning_with_temp = max77705_vib_set_ratio_with_temp,
};

static const struct sec_vibrator_ops max77705_single_freq_vib_ops = {
	.enable = max77705_vibrator_enable,
	.set_intensity = max77705_vib_set_intensity,
	.get_motor_type = max77705_vib_get_motor_type,
	.set_tuning_with_temp = max77705_vib_set_ratio_with_temp,
};

#if defined(CONFIG_OF)
static struct max77705_vibrator_pdata *of_max77705_vibrator_dt(
		struct device *dev)
{
	struct device_node *np;
	struct max77705_vibrator_pdata *pdata;
	u32 temp;
	int ret, i;

	pdata = devm_kzalloc(dev, sizeof(struct max77705_vibrator_pdata),
			GFP_KERNEL);
	if (!pdata)
		return NULL;

	np = dev->of_node;
	if (np == NULL) {
		pr_err("%s: error to get dt node\n", __func__);
		goto err_parsing_dt;
	}

	ret = of_property_read_u32(np, "haptic,multi_frequency", &temp);
	if (ret) {
		pr_info("%s: multi_frequency isn't used\n", __func__);
		pdata->freq_nums = 0;
	} else
		pdata->freq_nums = (int)temp;

	if (pdata->freq_nums) {
		pdata->freq_array = devm_kzalloc(dev,
				sizeof(u32)*pdata->freq_nums, GFP_KERNEL);
		if (!pdata->freq_array) {
			pr_err("%s: failed to allocate freq_array data\n",
					__func__);
			goto err_parsing_dt;
		}

		ret = of_property_read_u32_array(np, "haptic,frequency",
				pdata->freq_array, pdata->freq_nums);
		if (ret) {
			pr_err("%s: error to get dt node frequency\n",
					__func__);
			goto err_parsing_dt;
		}

		pdata->freq = pdata->freq_array[0];
	} else {
		ret = of_property_read_u32(np,
				"haptic,frequency", &temp);
		if (ret) {
			pr_err("%s: error to get dt node frequency\n",
					__func__);
			goto err_parsing_dt;
		} else
			pdata->freq = (int)temp;
	}

	ret = of_property_read_u32(np,
			"haptic,normal_ratio", &temp);
	if (ret) {
		pr_err("%s: error to get dt node normal_ratio\n", __func__);
		goto err_parsing_dt;
	} else
		pdata->normal_ratio = (int)temp;

	ret = of_property_read_u32(np,
			"haptic,overdrive_ratio", &temp);
	if (ret)
		pr_info("%s: overdrive_ratio isn't used\n", __func__);
	else
		pdata->overdrive_ratio = (int)temp;

	ret = of_property_read_u32(np,
			"haptic,high_temp_ref", &temp);
	if (ret)
		pr_info("%s: high temperature concept isn't used\n", __func__);
	else
		pdata->high_temp_ref = (int)temp * 10;

	ret = of_property_read_u32(np,
			"haptic,high_temp_ratio", &temp);
	if (ret)
		pr_info("%s: high_temp_ratio isn't used\n", __func__);
	else
		pdata->high_temp_ratio = (int)temp;

	pdata->pwm = devm_of_pwm_get(dev, np, NULL);
	if (IS_ERR(pdata->pwm)) {
		pr_err("%s: error to get pwms\n", __func__);
		goto err_parsing_dt;
	}

	ret = of_property_read_string(np, "haptic,motor_type",
			&pdata->motor_type);
	if (ret)
		pr_err("%s: motor_type is undefined\n", __func__);

	if (pdata->freq_nums) {
		pr_info("multi frequency: %d\n", pdata->freq_nums);
		for (i = 0; i < pdata->freq_nums; i++)
			pr_info("frequency[%d]: %d.%dHz\n", i,
					pdata->freq_array[i]/10,
					pdata->freq_array[i]%10);
	} else {
		pr_info("frequency: %d.%dHz\n", pdata->freq/10, pdata->freq%10);
	}
	pr_info("normal_ratio: %d\n", pdata->normal_ratio);
	pr_info("overdrive_ratio: %d\n", pdata->overdrive_ratio);
	pr_info("high temperature reference: %d\n", pdata->high_temp_ref);
	pr_info("high temperature ratio: %d\n", pdata->high_temp_ratio);
	pr_info("motor_type: %s\n", pdata->motor_type);

	return pdata;

err_parsing_dt:
	devm_kfree(dev, pdata);
	return NULL;
}
#endif

static int max77705_vibrator_probe(struct platform_device *pdev)
{
	struct max77705_dev *max77705 = dev_get_drvdata(pdev->dev.parent);
	struct max77705_platform_data *max77705_pdata
		= dev_get_platdata(max77705->dev);
	struct max77705_vibrator_pdata *pdata = max77705_pdata->vibrator_data;
	struct max77705_vibrator_drvdata *ddata;

#if defined(CONFIG_OF)
	if (pdata == NULL) {
		pdata = of_max77705_vibrator_dt(&pdev->dev);
		if (unlikely(!pdata)) {
			pr_err("max77705-haptic : %s not found haptic dt!\n",
					__func__);
			return -ENODEV;
		}
	}
#else
	if (unlikely(!pdata)) {
		pr_err("%s: no pdata\n", __func__);
		return -ENODEV;
	}
#endif /* CONFIG_OF */

	ddata = devm_kzalloc(&pdev->dev,
			sizeof(struct max77705_vibrator_drvdata), GFP_KERNEL);
	if (unlikely(!ddata)) {
		pr_err("%s: no ddata\n", __func__);
		return -ENODEV;
	}

	platform_set_drvdata(pdev, ddata);
	ddata->max77705 = max77705;
	ddata->i2c = max77705->i2c;
	ddata->pdata = pdata;
	ddata->period = FREQ_DIVIDER / pdata->freq;
	ddata->ratio = pdata->normal_ratio;
	ddata->max_duty = ddata->duty = ddata->period * ddata->ratio / 100;
	ddata->pwm = pdata->pwm;

	if (IS_ERR(ddata->pwm)) {
		pr_err("%s: Failed to get pwm\n", __func__);
		return -EFAULT;
	} else
		pwm_config(ddata->pwm, ddata->duty, ddata->period);

	max77705_vibrator_init_reg(ddata);
	max77705_vibrator_i2c_enable(ddata, false);

	ddata->sec_vib_ddata.dev = &pdev->dev;
	if (pdata->freq_nums)
		ddata->sec_vib_ddata.vib_ops = &max77705_multi_freq_vib_ops;
	else
		ddata->sec_vib_ddata.vib_ops = &max77705_single_freq_vib_ops;
	sec_vibrator_register(&ddata->sec_vib_ddata);

	return 0;
}

static int max77705_vibrator_remove(struct platform_device *pdev)
{
	struct max77705_vibrator_drvdata *ddata
		= platform_get_drvdata(pdev);

	sec_vibrator_unregister(&ddata->sec_vib_ddata);
	max77705_vibrator_i2c_enable(ddata, false);
	return 0;
}

static int max77705_vibrator_suspend(struct platform_device *pdev,
		pm_message_t state)
{
	struct device *dev = &pdev->dev;

	if (!IS_ERR(dev->pins->sleep_state)) {
		pr_info("%s: set sleep pinctrl\n", __func__);
		pinctrl_select_state(dev->pins->p, dev->pins->sleep_state);
	}

	return 0;
}

static int max77705_vibrator_resume(struct platform_device *pdev)
{
	struct max77705_vibrator_drvdata *ddata
		= platform_get_drvdata(pdev);

	struct device *dev = &pdev->dev;

	if (!IS_ERR(dev->pins->default_state)) {
		pr_info("%s: set default pinctrl\n", __func__);
		pinctrl_select_state(dev->pins->p, dev->pins->default_state);
	}

	max77705_vibrator_init_reg(ddata);

	return 0;
}

static void max77705_vibrator_shutdown(struct platform_device *pdev)
{
}

static struct platform_driver max77705_vibrator_driver = {
	.probe		= max77705_vibrator_probe,
	.remove		= max77705_vibrator_remove,
	.suspend	= max77705_vibrator_suspend,
	.resume		= max77705_vibrator_resume,
	.shutdown	= max77705_vibrator_shutdown,
	.driver = {
		.name	= "max77705_vibrator",
		.owner	= THIS_MODULE,
	},
};

static int __init max77705_vibrator_init(void)
{
	return platform_driver_register(&max77705_vibrator_driver);
}
module_init(max77705_vibrator_init);

static void __exit max77705_vibrator_exit(void)
{
	platform_driver_unregister(&max77705_vibrator_driver);
}
module_exit(max77705_vibrator_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("max77705 vibrator driver");
