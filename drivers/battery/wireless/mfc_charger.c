/*
 *  mfc_charger.c
 *  Samsung MFC IC Charger Driver
 *
 *  Copyright (C) 2016 Samsung Electronics
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include "mfc_charger.h"
#include <linux/errno.h>
#include <linux/version.h>
#include <linux/device.h>
#include <linux/pm.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/kernel.h>
#include <asm/uaccess.h>
#include <linux/sysctl.h>
#include <linux/proc_fs.h>
#include <linux/vmalloc.h>
#include <linux/ctype.h>
#include <linux/firmware.h>
#if IS_ENABLED(CONFIG_SPU_VERIFY)
#include <linux/spu-verify.h>
#endif

#define CMD_CNT 3
#define ISR_CNT 10
#define MAX_I2C_ERROR_COUNT		30

#define MAX_BUF 4095
static u8 ADT_buffer_rdata[MAX_BUF] = {0, };
static int adt_readSize;
bool is_shutdn = false;

/* Logic to call repair utility   */
#define MAX_MTP_PGM_CNT  3
int pgmCnt = 0;
bool repairEn = false;

#if defined(CONFIG_WIRELESS_CHARGER_HAL_MFC)
#define EXTERN_MFC_FUNC static
extern unsigned int mfc_chip_id_now;
#else
#define EXTERN_MFC_FUNC
#endif

extern unsigned int lpcharge;
#if defined(CONFIG_MST_V2)
extern int mfc_send_mst_cmd(int cmd, struct mfc_charger_data *charger, u8 irq_src_l, u8 irq_src_h);
#endif

static char *rx_device_type_str[] = {
	"No Dev",
	"Other Dev",
	"Gear",
	"Phone",
	"Buds",
};

static char *mfc_vout_control_mode_str[] = {
	"Set VOUT Off",
	"Set VOUT NV",
	"Set Vout Rsv",
	"Set Vout HV",
	"Set Vout CV",
	"Set Vout Call",
	"Set Vout 5V",
	"Set Vout 9V",
	"Set Vout 10V",
	"Set Vout 11V",
	"Set Vout 12V",
	"Set Vout 12.5V",
	"Set Vout 5V Step",
	"Set Vout 5.5V Step",
	"Set Vout 9V Step",
	"Set Vout 10V Step",
};

static char *rx_vout_str[] = {
	"Vout 5V",
	"Vout 5.5V",
	"Vout 6V",
	"Vout 7V",
	"Vout 8V",
	"Vout 9V",
	"Vout 10V",
	"Vout 11V",
	"Vout 12V",
	"Vout 12.5V",
};

enum mfc_test {
	MFC_TEST_SKIP_M0_RESET = 0,
	MFC_TEST_VRECT_IRQ_DELAY,
	MFC_TEST_MAX = 32,
};

static struct device_attribute mfc_attrs[] = {
	MFC_ATTR(mfc_addr),
	MFC_ATTR(mfc_size),
	MFC_ATTR(mfc_data),
	MFC_ATTR(mfc_packet),
	MFC_ATTR(mfc_flicker_test),
	MFC_ATTR(test),
};

static enum power_supply_property mfc_charger_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

EXTERN_MFC_FUNC int mfc_get_firmware_version(struct mfc_charger_data *charger, int firm_mode);
static irqreturn_t mfc_wpc_det_irq_thread(int irq, void *irq_data);
static irqreturn_t mfc_wpc_irq_thread(int irq, void *irq_data);
static int mfc_reg_multi_write_verify(struct i2c_client *client, u16 reg, const u8 *val, int size);

static void mfc_check_i2c_error(struct mfc_charger_data *charger, bool is_error)
{
	charger->i2c_error_count =
		(charger->wc_w_state && gpio_get_value(charger->pdata->wpc_det) && is_error) ?
		(charger->i2c_error_count + 1) : 0;

	if (charger->i2c_error_count > MAX_I2C_ERROR_COUNT) {
		charger->i2c_error_count = 0;
		queue_delayed_work(charger->wqueue, &charger->wpc_i2c_error_work, 0);
	}
}

EXTERN_MFC_FUNC
int mfc_reg_read(struct i2c_client *client, u16 reg, u8 *val)
{
	struct mfc_charger_data *charger = i2c_get_clientdata(client);
	int ret;
	struct i2c_msg msg[2];
	u8 wbuf[2];
	u8 rbuf[2];

	msg[0].addr = client->addr;
	msg[0].flags = client->flags & I2C_M_TEN;
	msg[0].len = 2;
	msg[0].buf = wbuf;

	wbuf[0] = (reg & 0xFF00) >> 8;
	wbuf[1] = (reg & 0xFF);

	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = 1;
	msg[1].buf = rbuf;

	mutex_lock(&charger->io_lock);
	ret = i2c_transfer(client->adapter, msg, 2);
	mfc_check_i2c_error(charger, (ret < 0));
	mutex_unlock(&charger->io_lock);
	if (ret < 0) {
		pr_err("%s: i2c read error, reg: 0x%x, ret: %d (called by %ps)\n",
			__func__, reg, ret, __builtin_return_address(0));
		return -1;
	}

	*val = rbuf[0];

	return ret;
}
#if !defined(CONFIG_WIRELESS_CHARGER_HAL_MFC)
EXPORT_SYMBOL(mfc_reg_read);
#endif

EXTERN_MFC_FUNC
int mfc_reg_multi_read(struct i2c_client *client, u16 reg, u8 *val, int size)
{
	struct mfc_charger_data *charger = i2c_get_clientdata(client);
	int ret;
	struct i2c_msg msg[2];
	u8 wbuf[2];

	msg[0].addr = client->addr;
	msg[0].flags = client->flags & I2C_M_TEN;
	msg[0].len = 2;
	msg[0].buf = wbuf;

	wbuf[0] = (reg & 0xFF00) >> 8;
	wbuf[1] = (reg & 0xFF);

	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = size;
	msg[1].buf = val;

	mutex_lock(&charger->io_lock);
	ret = i2c_transfer(client->adapter, msg, 2);
	mfc_check_i2c_error(charger, (ret < 0));
	mutex_unlock(&charger->io_lock);
	if (ret < 0) {
		pr_err("%s: i2c transfer fail", __func__);
		return -1;
	}

	return ret;
}
#if !defined(CONFIG_WIRELESS_CHARGER_HAL_MFC)
EXPORT_SYMBOL(mfc_reg_multi_read);
#endif

EXTERN_MFC_FUNC
int mfc_reg_write(struct i2c_client *client, u16 reg, u8 val)
{
	struct mfc_charger_data *charger = i2c_get_clientdata(client);
	int ret;
	unsigned char data[3] = { reg >> 8, reg & 0xff, val };

//	pr_debug("%s: reg=0x%x, val=0x%x", __func__, reg, val);

	mutex_lock(&charger->io_lock);
	ret = i2c_master_send(client, data, 3);
	mfc_check_i2c_error(charger, (ret < 3));
	mutex_unlock(&charger->io_lock);
	if (ret < 3) {
		pr_err("%s: i2c write error, reg: 0x%x, ret: %d (called by %ps)\n",
				__func__, reg, ret, __builtin_return_address(0));
		return ret < 0 ? ret : -EIO;
	}

	return 0;
}
#if !defined(CONFIG_WIRELESS_CHARGER_HAL_MFC)
EXPORT_SYMBOL(mfc_reg_write);
#endif

EXTERN_MFC_FUNC
int mfc_reg_update(struct i2c_client *client, u16 reg, u8 val, u8 mask)
{
	//val = 0b 0000 0001
	//ms = 0b 1111 1110
	struct mfc_charger_data *charger = i2c_get_clientdata(client);
	unsigned char data[3] = {reg >> 8, reg & 0xff, val};
	u8 data2;
	int ret;

	ret = mfc_reg_read(client, reg, &data2);
	if (ret >= 0) {
		u8 old_val = data2 & 0xff;
		u8 new_val = (val & mask) | (old_val & (~mask));

		data[2] = new_val;
		mutex_lock(&charger->io_lock);
		ret = i2c_master_send(client, data, 3);
		mfc_check_i2c_error(charger, (ret < 3));
		mutex_unlock(&charger->io_lock);
		if (ret < 3) {
			pr_err("%s: i2c write error, reg: 0x%x, ret: %d\n",
				__func__, reg, ret);
			return ret < 0 ? ret : -EIO;
		}
	}
	mfc_reg_read(client, reg, &data2);

	return ret;
}
#if !defined(CONFIG_WIRELESS_CHARGER_HAL_MFC)
EXPORT_SYMBOL(mfc_reg_update);
#endif

EXTERN_MFC_FUNC
int mfc_get_firmware_version(struct mfc_charger_data *charger, int firm_mode)
{
	int ver_major = -1, ver_minor = -1;
	int ret;
	u8 fw_major[2] = {0, };
	u8 fw_minor[2] = {0, };

	pr_info("%s: called by (%ps)\n", __func__, __builtin_return_address(0));
	switch (firm_mode) {
	case MFC_RX_FIRMWARE:
		ret = mfc_reg_read(charger->client, MFC_FW_MAJOR_REV_L_REG, &fw_major[0]);
		if (ret < 0)
			break;

		ret = mfc_reg_read(charger->client, MFC_FW_MAJOR_REV_H_REG, &fw_major[1]);
		if (ret < 0)
			break;

		ver_major = fw_major[0] | (fw_major[1] << 8);
		pr_info("%s: rx major firmware version 0x%x\n", __func__, ver_major);

		ret = mfc_reg_read(charger->client, MFC_FW_MINOR_REV_L_REG, &fw_minor[0]);
		if (ret < 0)
			break;

		ret = mfc_reg_read(charger->client, MFC_FW_MINOR_REV_H_REG, &fw_minor[1]);
		if (ret < 0)
			break;

		ver_minor = fw_minor[0] | (fw_minor[1] << 8);
		pr_info("%s: rx minor firmware version 0x%x\n", __func__, ver_minor);
		break;
	default:
		pr_err("%s: Wrong firmware mode\n", __func__);
		ver_major = -1;
		ver_minor = -1;
		break;
	}

#if defined(CONFIG_WIRELESS_IC_PARAM)
	if (ver_minor > 0 && charger->wireless_fw_ver_param != ver_minor) {
		pr_info("%s: fw_ver (param:0x%04X, version:0x%04X)\n",
			__func__, charger->wireless_fw_ver_param, ver_minor);
		charger->wireless_fw_ver_param = ver_minor;
		charger->wireless_param_info &= 0xFF0000FF;
		charger->wireless_param_info |= (charger->wireless_fw_ver_param & 0xFFFF) << 8;
		pr_info("%s: wireless_param_info (0x%08X)\n", __func__, charger->wireless_param_info);
	}
#endif

	return ver_minor;
}

static int mfc_get_chip_id(struct mfc_charger_data *charger)
{
	u8 chip_id = 0;
	int ret = 0;

	ret = mfc_reg_read(charger->client, MFC_CHIP_ID_L_REG, &chip_id);
	if (ret < 0)
		return -1;

	if (chip_id == MFC_CHIP_ID_S2MIW04) {
		charger->chip_id = MFC_CHIP_LSI;
		pr_info("%s: LSI CHIP(0x%x)\n", __func__, chip_id);
	} else { /* 0x20 */
		charger->chip_id = MFC_CHIP_IDT;
		pr_info("%s: IDT CHIP(0x%x)\n", __func__, chip_id);
	}
	charger->chip_id_now = chip_id;

#if defined(CONFIG_WIRELESS_IC_PARAM)
	if (chip_id > 0 && charger->wireless_chip_id_param != chip_id) {
		pr_info("%s: chip_id (param:0x%02X, chip_id:0x%02X)\n",
			__func__, charger->wireless_chip_id_param, chip_id);
		charger->wireless_chip_id_param = chip_id;
		charger->wireless_param_info &= 0x00FFFFFF;
		charger->wireless_param_info |= (charger->wireless_chip_id_param & 0xFF) << 24;
		pr_info("%s: wireless_param_info (0x%08X)\n", __func__, charger->wireless_param_info);
	}
#endif

	return charger->chip_id;
}

static int mfc_get_ic_revision(struct mfc_charger_data *charger, int read_mode)
{
	u8 temp;
	int ret = -1;

	pr_info("%s: called by (%ps)\n", __func__, __builtin_return_address(0));

	switch (read_mode) {
		case MFC_IC_REVISION:
			ret = mfc_reg_read(charger->client, MFC_CHIP_REVISION_REG, &temp);
			if (ret >= 0) {
				temp &= MFC_CHIP_REVISION_MASK;
				pr_info("%s: ic revision = %d\n", __func__, temp);
				ret = temp;
			}
			break;
		case MFC_IC_FONT:
			ret = mfc_reg_read(charger->client, MFC_CHIP_REVISION_REG, &temp);
			if (ret >= 0) {
				temp &= MFC_CHIP_FONT_MASK;
				pr_info("%s: ic font = %d\n", __func__, temp);
				ret = temp;
			}
			break;
		default :
			break;
	}
	return ret;
}

static int mfc_get_adc(struct mfc_charger_data *charger, int adc_type)
{
	int ret = 0;
	u8 data[2] = {0,};

	switch (adc_type) {
	case MFC_ADC_VOUT:
		ret = mfc_reg_read(charger->client, MFC_ADC_VOUT_L_REG, &data[0]);
		if (ret < 0)
			break;

		ret = mfc_reg_read(charger->client, MFC_ADC_VOUT_H_REG, &data[1]);
		if (ret < 0)
			break;

		ret = (data[0] | (data[1] << 8));
		break;
	case MFC_ADC_VRECT:
		ret = mfc_reg_read(charger->client, MFC_ADC_VRECT_L_REG, &data[0]);
		if (ret < 0)
			break;

		ret = mfc_reg_read(charger->client, MFC_ADC_VRECT_H_REG, &data[1]);
		if (ret < 0)
			break;

		ret = (data[0] | (data[1] << 8));
		break;
	case MFC_ADC_RX_IOUT:
		ret = mfc_reg_read(charger->client, MFC_ADC_IOUT_L_REG, &data[0]);
		if (ret < 0)
			break;

		ret = mfc_reg_read(charger->client, MFC_ADC_IOUT_H_REG, &data[1]);
		if (ret < 0)
			break;

		ret = (data[0] | (data[1] << 8));
		break;
	case MFC_ADC_DIE_TEMP:
		ret = mfc_reg_read(charger->client, MFC_ADC_DIE_TEMP_L_REG, &data[0]);
		if (ret < 0)
			break;

		ret = mfc_reg_read(charger->client, MFC_ADC_DIE_TEMP_H_REG, &data[1]);
		if (ret < 0)
			break;

		/* only 4 MSB[3:0] field is used, Celsius */
		data[1] &= 0x0f;
		ret = (data[0] | (data[1] << 8));
		break;
	case MFC_ADC_OP_FRQ: /* kHz */
		ret = mfc_reg_read(charger->client, MFC_TRX_OP_FREQ_L_REG, &data[0]);
		if (ret < 0)
			break;

		ret = mfc_reg_read(charger->client, MFC_TRX_OP_FREQ_H_REG, &data[1]);
		if (ret < 0)
			break;

		ret = (data[0] | (data[1] << 8));
		break;
	case MFC_ADC_TX_OP_FRQ:
		ret = mfc_reg_read(charger->client, MFC_TX_MAX_OP_FREQ_L_REG, &data[0]);
		if (ret < 0)
			break;

		ret = mfc_reg_read(charger->client, MFC_TX_MAX_OP_FREQ_H_REG, &data[1]);
		if (ret < 0)
			break;

		ret = (int)(60000 / (data[0] | (data[1] << 8)));
		break;
	case MFC_ADC_TX_MIN_OP_FRQ:
		ret = mfc_reg_read(charger->client, MFC_TX_MIN_OP_FREQ_L_REG, &data[0]);
		if (ret < 0)
			break;

		ret = mfc_reg_read(charger->client, MFC_TX_MIN_OP_FREQ_H_REG, &data[1]);
		if (ret < 0)
			break;

		ret = (int)(60000 / (data[0] | (data[1] << 8)));
		break;
	case MFC_ADC_PING_FRQ:
		ret = mfc_reg_read(charger->client, MFC_RX_PING_FREQ_L_REG, &data[0]);
		if (ret < 0)
			break;

		ret = mfc_reg_read(charger->client, MFC_RX_PING_FREQ_H_REG, &data[1]);
		if (ret < 0)
			break;

		ret = (data[0] | (data[1] << 8));
		break;
	case MFC_ADC_TX_VOUT:
		ret = mfc_reg_read(charger->client, MFC_ADC_VOUT_L_REG, &data[0]);
		if (ret < 0)
			break;

		ret = mfc_reg_read(charger->client, MFC_ADC_VOUT_H_REG, &data[1]);
		if (ret < 0)
			break;

		ret = (data[0] | (data[1] << 8));
		break;
	case MFC_ADC_TX_IOUT:
		ret = mfc_reg_read(charger->client, MFC_ADC_IOUT_L_REG, &data[0]);
		if (ret < 0)
			break;

		ret = mfc_reg_read(charger->client, MFC_ADC_IOUT_H_REG, &data[1]);
		if (ret < 0)
			break;

		ret = (data[0] | (data[1] << 8));
		break;
	default:
		break;
	}

	return ret;
}

#if !IS_ENABLED(CONFIG_MST_V2)
/*
 * I2C COMMANDS TO DRIVE MST PINS FOR INTERNAL PULL UP
 *
 * 1. Enable GP3 pin with Internal Pull up- write 0x40 to address
 *    0x6C24 - I2C Command :
 *     a) Address: 0x6C24
 *     b) Number of bytes: 1
 *     c) Data: 0x40
 *
 * 2. Enable GP4 pin with Internal Pull up- write 0x40 to address
 *    0x6C28 - I2C Command :
 *     a) Address: 0x6C28
 *     b) Number of bytes: 1
 *     c) Data: 0x40
 */
static void mfc_set_for_mst_removal(struct mfc_charger_data *charger)
{
	u8 data = 0, data2 = 0;
	int ret;

	mfc_reg_write(charger->client, MST_DRV1_GP3_PIN_REG, 0x40);
	mfc_reg_write(charger->client, MST_DRV0_GP4_PIN_REG, 0x40);

	ret = mfc_reg_read(charger->client, MST_DRV1_GP3_PIN_REG, &data);
	if (ret < 0)
		pr_err("%s: MST_DRV1_GP3 pin setting read fail! (%d)\n", __func__, ret);

	ret = mfc_reg_read(charger->client, MST_DRV0_GP4_PIN_REG, &data2);
	if (ret < 0)
		pr_err("%s: MST_DRV0_GP4 pin setting read fail! (%d)\n", __func__, ret);

	pr_info("%s: MST_DRV1_GP3(%d) , MST_DRV0_GP4(%d)\n", __func__, data, data2);
}
#endif

static void mfc_set_wpc_en(struct mfc_charger_data *charger, char flag, char on)
{
	union power_supply_propval value = {0, };
	int enable = 0, temp = charger->wpc_en_flag;
	int old_val = 0, new_val = 0;

	psy_do_property("battery", get,
		POWER_SUPPLY_PROP_STATUS, value);

	mutex_lock(&charger->wpc_en_lock);

	if (on) {
		if (value.intval == POWER_SUPPLY_STATUS_DISCHARGING)
			charger->wpc_en_flag |= WPC_EN_CHARGING;
		charger->wpc_en_flag |= flag;
	} else {
		charger->wpc_en_flag &= ~flag;
	}

	if (charger->wpc_en_flag & WPC_EN_FW)
		enable = 1;
	else if (!(charger->wpc_en_flag & WPC_EN_SYSFS) || !(charger->wpc_en_flag & WPC_EN_CCIC))
		enable = 0;
	else if (!(charger->wpc_en_flag & (WPC_EN_CHARGING | WPC_EN_MST | WPC_EN_TX)))
		enable = 0;
	else
		enable = 1;

	if (charger->pdata->wpc_en) {
		old_val = gpio_get_value(charger->pdata->wpc_en);
		if (enable)
			gpio_direction_output(charger->pdata->wpc_en, 0);
		else
			gpio_direction_output(charger->pdata->wpc_en, 1);
		new_val = gpio_get_value(charger->pdata->wpc_en);

		pr_info("@DIS_MFC %s: before(0x%x), after(0x%x), en(%d), val(%d->%d)\n",
			__func__, temp, charger->wpc_en_flag, enable, old_val, new_val);

		mutex_unlock(&charger->wpc_en_lock);
		if (old_val != new_val) {
			value.intval = enable;
			psy_do_property("battery", set,
				POWER_SUPPLY_EXT_PROP_WPC_EN, value);
		}
	} else {
		pr_info("@DIS_MFC %s: there`s no wpc_en\n", __func__);
		mutex_unlock(&charger->wpc_en_lock);
	}
}

static void mfc_set_vout(struct mfc_charger_data *charger, int vout)
{
	mfc_reg_write(charger->client, MFC_VOUT_SET_REG, mfc_idt_vout_val[vout]);
	msleep(100);

	pr_info("%s: vout(%s, %d) read = %d mV\n", __func__,
		rx_vout_str[vout], vout, mfc_get_adc(charger, MFC_ADC_VOUT));
	charger->pdata->vout_status = vout;
}

static int mfc_get_vout(struct mfc_charger_data *charger)
{
	u8 data;
	int ret;

	ret = mfc_reg_read(charger->client, MFC_VOUT_SET_REG, &data);
	if (ret < 0) {
		pr_err("%s: fail to read vout. (%d)\n", __func__, ret);
		return ret;
	}

	pr_info("%s: vout(0x%x)\n", __func__, data);

	return data;
}

static void mfc_uno_on(struct mfc_charger_data *charger, bool on)
{
	union power_supply_propval value = {0, };

	if (charger->wc_tx_enable && on) { /* UNO ON */
		value.intval = SEC_BAT_CHG_MODE_UNO_ONLY;
		psy_do_property("otg", set,
			POWER_SUPPLY_EXT_PROP_CHARGE_UNO_CONTROL, value);
		pr_info("%s: UNO ON\n", __func__);
	} else if (on) { /* UNO ON */
		value.intval = 1;
		psy_do_property("otg", set,
			POWER_SUPPLY_EXT_PROP_CHARGE_UNO_CONTROL, value);
		pr_info("%s: UNO ON\n", __func__);
	} else { /* UNO OFF */
		value.intval = 0;
		psy_do_property("otg", set,
			POWER_SUPPLY_EXT_PROP_CHARGE_UNO_CONTROL, value);

		if (delayed_work_pending(&charger->wpc_tx_op_freq_work)) {
			__pm_relax(charger->wpc_tx_opfq_ws);
			cancel_delayed_work(&charger->wpc_tx_op_freq_work);
		}
		pr_info("%s: UNO OFF\n", __func__);
	}
}

static void mfc_rpp_set(struct mfc_charger_data *charger)
{
	u8 data;
	int ret;

	if (charger->led_cover) {
		pr_info("%s: LED cover exists. RPP 3/4 (0x%x)\n", __func__, charger->pdata->wc_cover_rpp);
		mfc_reg_write(charger->client, MFC_RPP_SCALE_COEF_REG, charger->pdata->wc_cover_rpp);
	} else {
		pr_info("%s: LED cover not exists. RPP 1/2 (0x%x)\n", __func__, charger->pdata->wc_hv_rpp);
		mfc_reg_write(charger->client, MFC_RPP_SCALE_COEF_REG, charger->pdata->wc_hv_rpp);
	}
	msleep(5);
	ret = mfc_reg_read(charger->client, MFC_RPP_SCALE_COEF_REG, &data);
	if (ret < 0)
		pr_err("%s: fail to read RPP scaling coefficient. (%d)\n", __func__, ret);
	else
		pr_info("%s: RPP scaling coefficient(0x%x)\n", __func__, data);
}

static mfc_fod_data* mfc_get_fod_data(struct mfc_charger_data *charger)
{
	int i = 0;

	if (charger->pdata->fod_data_count <= 0)
		return NULL;

	for (i = 0; i < charger->pdata->fod_data_count; i++)
		if (charger->pdata->fod_list[i].pad_id == charger->tx_id)
			return &charger->pdata->fod_list[i];

	return (charger->pdata->fod_list[DEFAULT_PAD_ID].pad_id != 0) ?
		NULL : &charger->pdata->fod_list[DEFAULT_PAD_ID];
}

static void mfc_fod_set(struct mfc_charger_data *charger, int fod_state)
{
	mfc_fod_data* pfod_data = NULL;
	int i;

	fod_state = (charger->is_full_status) ? FOD_STATE_FULL : fod_state;
	pr_info("%s: pad_id = 0x%X, is_full_status = %d, fod_state = %d\n",
		__func__, charger->tx_id, charger->is_full_status, fod_state);

	if (charger->pdata->cable_type == SEC_WIRELESS_PAD_NONE)
		return;

	pfod_data = mfc_get_fod_data(charger);
	if (pfod_data == NULL || pfod_data->data[fod_state] == NULL) {
		pr_info("%s: skip to set fod because fod data is null\n", __func__);
		return;
	}

	for (i = 0; i < MFC_NUM_FOD_REG; i++)
		mfc_reg_write(charger->client, MFC_WPC_FOD_0A_REG + i,
			pfod_data->data[fod_state][i]);
}

static void mfc_set_tx_op_freq(struct mfc_charger_data *charger, unsigned int op_feq)
{
	u8 data[2] = {0, };

	pr_info("%s: op feq = %d KHz\n", __func__, op_feq);

	op_feq = (int)(60000 / op_feq);
	data[0] = op_feq & 0xFF;
	data[1] = (op_feq & 0xFF00) >> 8;

	mfc_reg_write(charger->client, MFC_TX_MAX_OP_FREQ_L_REG, data[0]);
	mfc_reg_write(charger->client, MFC_TX_MAX_OP_FREQ_H_REG, data[1]);

	msleep(500);
	pr_info("%s: op feq = %d KHz\n", __func__, mfc_get_adc(charger, MFC_ADC_TX_OP_FRQ));
}

static void mfc_set_tx_min_op_freq(struct mfc_charger_data *charger, unsigned int op_freq)
{
	u8 data[2] = {0,};

	pr_info("%s: op freq = %d KHz\n", __func__, op_freq);

	op_freq = (int)(60000 / op_freq);
	data[0] = op_freq & 0xFF;
	data[1] = (op_freq & 0xFF00) >> 8;
	mfc_reg_write(charger->client, MFC_TX_MIN_OP_FREQ_L_REG, data[0]);
	mfc_reg_write(charger->client, MFC_TX_MIN_OP_FREQ_H_REG, data[1]);

	msleep(500);
	pr_info("%s: op feq = %d KHz\n", __func__, mfc_get_adc(charger, MFC_ADC_TX_MIN_OP_FRQ));
}

static void mfc_set_min_duty(struct mfc_charger_data *charger, unsigned int duty)
{
	u8 data = 0;

	data = (int)((duty * 256) / 100);

	pr_info("%s: min duty = %d%%(0x%x)\n", __func__, duty, data);

	mfc_reg_write(charger->client, MFC_TX_MIN_DUTY_SETTING_REG, data);
}

static void mfc_set_tx_oc_fod(struct mfc_charger_data *charger)
{
	u8 data[2] = {0,};

	pr_info("%s: oc_fod1 = %d mA\n", __func__, charger->pdata->oc_fod1);

	data[0] = (charger->pdata->oc_fod1) & 0xff;
	data[1] = (charger->pdata->oc_fod1 & 0xff00) >> 8;

	mfc_reg_write(charger->client, MFC_TX_OC_FOD1_LIMIT_L_REG, data[0]);
	mfc_reg_write(charger->client, MFC_TX_OC_FOD1_LIMIT_H_REG, data[1]);
}

EXTERN_MFC_FUNC
void mfc_set_cmd_l_reg(struct mfc_charger_data *charger, u8 val, u8 mask)
{
	u8 temp = 0;
	int ret = 0, i = 0;

	do {
		pr_info("%s\n", __func__);
		ret = mfc_reg_update(charger->client, MFC_AP2MFC_CMD_L_REG, val, mask); // command
		if (ret >= 0) {
			msleep(10);
			ret = mfc_reg_read(charger->client, MFC_AP2MFC_CMD_L_REG, &temp); // check out set bit exists
			if (temp != 0)
				pr_info("%s: CMD is not clear yet, cnt = %d\n", __func__, i);
			if (ret < 0 || i > 3)
				break;
		}
		i++;
	} while ((temp != 0) && (i < 3));
}

static void mfc_send_eop(struct mfc_charger_data *charger, int health_mode)
{
	int i = 0;
	int ret = 0;

	pr_info("%s: health_mode(0x%x), cable_type(%d)\n",
		__func__, health_mode, charger->pdata->cable_type);

	switch (health_mode) {
	case POWER_SUPPLY_HEALTH_OVERHEAT:
	case POWER_SUPPLY_EXT_HEALTH_OVERHEATLIMIT:
	case POWER_SUPPLY_HEALTH_COLD:
		pr_info("%s: ept-ot\n", __func__);
		if (charger->pdata->cable_type == SEC_WIRELESS_PAD_PMA) {
			for (i = 0; i < CMD_CNT; i++) {
				ret = mfc_reg_write(charger->client, MFC_EPT_REG, MFC_WPC_EPT_END_OF_CHG);
				if (ret < 0)
					break;

				mfc_set_cmd_l_reg(charger, MFC_CMD_SEND_EOP_MASK, MFC_CMD_SEND_EOP_MASK);
				msleep(250);
			}
		} else {
			for (i = 0; i < CMD_CNT; i++) {
				ret = mfc_reg_write(charger->client, MFC_EPT_REG, MFC_WPC_EPT_OVER_TEMP);
				if (ret < 0)
					break;

				mfc_set_cmd_l_reg(charger, MFC_CMD_SEND_EOP_MASK, MFC_CMD_SEND_EOP_MASK);
				msleep(250);
			}
		}
		break;
	default:
		break;
	}
}

static void mfc_packet_assist(struct mfc_charger_data *charger)
{
	u8 dummy;

	mfc_reg_read(charger->client, MFC_WPC_RX_DATA_COM_REG, &dummy);
	mfc_reg_read(charger->client, MFC_WPC_RX_DATA_VALUE0_REG, &dummy);
	mfc_reg_read(charger->client, MFC_AP2MFC_CMD_L_REG, &dummy);
}

static void mfc_send_packet(struct mfc_charger_data *charger, u8 header, u8 rx_data_com, u8 data)
{
	mfc_reg_write(charger->client, MFC_WPC_PCKT_HEADER_REG, header);
	mfc_reg_write(charger->client, MFC_WPC_RX_DATA_COM_REG, rx_data_com);
	mfc_reg_write(charger->client, MFC_WPC_RX_DATA_VALUE0_REG, data);
	mfc_set_cmd_l_reg(charger, MFC_CMD_SEND_TRX_DATA_MASK, MFC_CMD_SEND_TRX_DATA_MASK);
}
/* unused function
static void mfc_send_multi_packet(struct mfc_charger_data *charger, u8 header, u8 rx_data_com, u8 *data_val, int data_size)
{
	int i;

	mfc_reg_write(charger->client, MFC_WPC_PCKT_HEADER_REG, header);
	mfc_reg_write(charger->client, MFC_WPC_RX_DATA_COM_REG, rx_data_com);

	for (i = 0; i < data_size; i++) {
		mfc_reg_write(charger->client, MFC_WPC_RX_DATA_VALUE0_REG + i, data_val[i]);
	}
	mfc_set_cmd_l_reg(charger, MFC_CMD_SEND_TRX_DATA_MASK, MFC_CMD_SEND_TRX_DATA_MASK);
}
*/

static int mfc_send_cs100(struct mfc_charger_data *charger)
{
	int i = 0;
	int ret = 0;

	for (i = 0; i < CMD_CNT; i++) {
		ret = mfc_reg_write(charger->client, MFC_BATTERY_CHG_STATUS_REG, 100); /* 100 means 100% */
		if (ret < 0) {
			ret = -1;
			break;
		}

		mfc_set_cmd_l_reg(charger, MFC_CMD_SEND_CHG_STS_MASK, MFC_CMD_SEND_CHG_STS_MASK);
		msleep(250);
		ret = 1;
	}
	return ret;
}

static void mfc_send_command(struct mfc_charger_data *charger, int cmd_mode)
{
	u8 i;

	switch (cmd_mode) {
	case MFC_AFC_CONF_5V:
		pr_info("%s: WPC/PMA set 5V\n", __func__);
		mfc_send_packet(charger, MFC_HEADER_AFC_CONF, WPC_COM_AFC_SET, RX_DATA_VAL2_5V);
		msleep(120);

		charger->vout_mode = WIRELESS_VOUT_5V;
		cancel_delayed_work(&charger->wpc_vout_mode_work);
		__pm_stay_awake(charger->wpc_vout_mode_ws);
		queue_delayed_work(charger->wqueue, &charger->wpc_vout_mode_work, 0);

		pr_info("%s: vout read = %d\n", __func__, mfc_get_adc(charger, MFC_ADC_VOUT));
		mfc_packet_assist(charger);
		break;
	case MFC_AFC_CONF_10V:
		pr_info("%s: WPC set 10V\n", __func__);
		/* trigger 10V vout work after 8sec */
		__pm_stay_awake(charger->wpc_afc_vout_ws);
#if defined(CONFIG_SEC_FACTORY)
		queue_delayed_work(charger->wqueue, &charger->wpc_afc_vout_work, msecs_to_jiffies(3000));
#else
		queue_delayed_work(charger->wqueue, &charger->wpc_afc_vout_work, msecs_to_jiffies(8000));
#endif
		break;
	case MFC_AFC_CONF_5V_TX:
		for (i = 0; i < CMD_CNT; i++) {
			pr_info("%s: set 5V to TX, cnt = %d \n", __func__, i);
			mfc_send_packet(charger, MFC_HEADER_AFC_CONF, WPC_COM_AFC_SET, RX_DATA_VAL2_5V);
			mfc_packet_assist(charger);
			msleep(100);
		}
		break;
	case MFC_AFC_CONF_10V_TX:
		for (i = 0; i < CMD_CNT; i++) {
			pr_info("%s: set 10V to TX, cnt = %d \n", __func__, i);
			mfc_send_packet(charger, MFC_HEADER_AFC_CONF, WPC_COM_AFC_SET, RX_DATA_VAL2_10V);
			mfc_packet_assist(charger);
			msleep(100);
		}
		break;
	case MFC_AFC_CONF_12V_TX:
		for (i = 0; i < CMD_CNT; i++) {
			pr_info("%s: set 12V to TX, cnt = %d \n", __func__, i);
			mfc_send_packet(charger, MFC_HEADER_AFC_CONF, WPC_COM_AFC_SET, RX_DATA_VAL2_12V);
			mfc_packet_assist(charger);
			msleep(100);
		}
		break;
	case MFC_AFC_CONF_12_5V_TX:
		for (i = 0; i < CMD_CNT; i++) {
			pr_info("%s: set 12.5V to TX, cnt = %d \n", __func__, i);
			mfc_send_packet(charger, MFC_HEADER_AFC_CONF, WPC_COM_AFC_SET, RX_DATA_VAL2_12_5V);
			mfc_packet_assist(charger);
			msleep(100);
		}
		break;
	case MFC_AFC_CONF_20V_TX:
		for (i = 0; i < CMD_CNT; i++) {
			pr_info("%s: set 20V to TX, cnt = %d \n", __func__, i);
			mfc_send_packet(charger, MFC_HEADER_AFC_CONF, WPC_COM_AFC_SET, RX_DATA_VAL2_20V);
			mfc_packet_assist(charger);
			msleep(100);
		}
		break;
	case MFC_LED_CONTROL_ON:
		pr_info("%s: led on\n", __func__);
		mfc_send_packet(charger, MFC_HEADER_AFC_CONF, WPC_COM_LED_CONTROL, WPC_LED_ON);
		msleep(100);
		break;
	case MFC_LED_CONTROL_OFF:
		pr_info("%s: led off\n", __func__);
		mfc_send_packet(charger, MFC_HEADER_AFC_CONF, WPC_COM_LED_CONTROL, WPC_LED_OFF);
		msleep(100);
		break;
	case MFC_LED_CONTROL_DIMMING:
		pr_info("%s: led dimming\n", __func__);
		mfc_send_packet(charger, MFC_HEADER_AFC_CONF, WPC_COM_LED_CONTROL, WPC_LED_DIMMING);
		msleep(100);
		break;
	case MFC_FAN_CONTROL_ON:
		pr_info("%s: fan on\n", __func__);
		mfc_send_packet(charger, MFC_HEADER_AFC_CONF, WPC_COM_COOLING_CTRL, WPC_FAN_ON);
		msleep(100);
		break;
	case MFC_FAN_CONTROL_OFF:
		pr_info("%s: fan off\n", __func__);
		mfc_send_packet(charger, MFC_HEADER_AFC_CONF, WPC_COM_COOLING_CTRL, WPC_FAN_OFF);
		msleep(100);
		break;
	case MFC_REQUEST_AFC_TX:
		pr_info("%s: request afc tx, cable(0x%x)\n", __func__, charger->pdata->cable_type);
		mfc_send_packet(charger, MFC_HEADER_AFC_CONF, WPC_COM_REQ_AFC_TX, WPC_REQUEST_AFC_TX);
		break;
	case MFC_REQUEST_TX_ID:
		pr_info("%s: request TX ID\n", __func__);
		mfc_send_packet(charger, MFC_HEADER_AFC_CONF, WPC_COM_TX_ID, WPC_REQUEST_TX_ID);
		break;
	case MFC_DISABLE_TX:
		pr_info("%s: Disable TX\n", __func__);
		mfc_send_packet(charger, MFC_HEADER_AFC_CONF, WPC_COM_DISABLE_TX, WPC_DISABLE_TX);
		break;
	case MFC_PHM_ON:
		pr_info("%s: Enter PHM\n", __func__);
		mfc_send_packet(charger, MFC_HEADER_AFC_CONF, WPC_COM_ENTER_PHM, WPC_ENTER_PHM);
		break;
	case MFC_SET_OP_FREQ:
		pr_info("%s: set tx op freq\n", __func__);
		mfc_send_packet(charger, MFC_HEADER_AFC_CONF, WPC_COM_OP_FREQ_SET, WPC_SET_OP_FREQ_120P5KHZ);
		break;
	default:
		break;
	}
}

static void mfc_send_fsk(struct mfc_charger_data *charger, u8 tx_data_com, u8 data_val)
{
	int i;

	for (i = 0; i < 3; i++) {
		mfc_reg_write(charger->client, MFC_WPC_TX_DATA_COM_REG, tx_data_com);
		mfc_reg_write(charger->client, MFC_WPC_TX_DATA_VALUE0_REG, data_val);
		mfc_set_cmd_l_reg(charger, MFC_CMD_SEND_TRX_DATA_MASK, MFC_CMD_SEND_TRX_DATA_MASK);
		msleep(250);
	}
}

static void mfc_led_control(struct mfc_charger_data *charger, int state)
{
	int i = 0;

	for (i = 0; i < CMD_CNT; i++)
		mfc_send_command(charger, state);
}

static void mfc_fan_control(struct mfc_charger_data *charger, bool on)
{
	int i = 0;

	if (on) {
		for (i = 0; i < CMD_CNT - 1; i++)
			mfc_send_command(charger, MFC_FAN_CONTROL_ON);
	} else {
		for (i = 0; i < CMD_CNT - 1; i++)
			mfc_send_command(charger, MFC_FAN_CONTROL_OFF);
	}
}

static void mfc_set_vrect_adjust(struct mfc_charger_data *charger, u8 vrect_headroom)
{
	int i = 0;

	for (i = 0; i < 6; i++) {
		mfc_reg_write(charger->client, MFC_VRECT_ADJ_REG, vrect_headroom);
		msleep(50);
	}
}

/* these pads can control fans for sleep/auto mode */
static bool is_sleep_mode_active(u8 pad_id)
{
	/* standard fw, hv pad, no multiport pad, non hv pad and dream pad */
	if (can_pad_control(pad_id)) {
		pr_info("%s: this pad can control fan for auto mode\n", __func__);
		return 1;
	} else {
		pr_info("%s: this pad cannot control fan\n", __func__);
		return 0;
	}
}

static void mfc_mis_align(struct mfc_charger_data *charger)
{
	if (charger->test & (1 << MFC_TEST_SKIP_M0_RESET))
		return;

	pr_info("%s: Reset M0\n", __func__);
	/* reset MCU of MFC IC */
	mfc_set_cmd_l_reg(charger, MFC_CMD_MCU_RESET_MASK, MFC_CMD_MCU_RESET_MASK);
}

static bool mfc_tx_function_check(struct mfc_charger_data *charger)
{
	u8 reg_f2, reg_f4;

	mfc_reg_read(charger->client, MFC_TX_RXID1_READ_REG, &reg_f2);
	mfc_reg_read(charger->client, MFC_TX_RXID3_READ_REG, &reg_f4);

	if ((reg_f2 == SS_ID) && (reg_f4 == SS_CODE))
		return true;
	else
		return false;
}

static void mfc_set_tx_ioffset(struct mfc_charger_data *charger, int ioffset)
{
	u8 ioffset_data_l = 0, ioffset_data_h = 0;

	ioffset_data_l = 0xFF & ioffset;
	ioffset_data_h = 0xFF & (ioffset >> 8);

	mfc_reg_write(charger->client, MFC_TX_IUNO_OFFSET_L_REG, ioffset_data_l);
	mfc_reg_write(charger->client, MFC_TX_IUNO_OFFSET_H_REG, ioffset_data_h);

	pr_info("@Tx_Mode %s: Tx Iout set %d(0x%x 0x%x)\n", __func__, ioffset, ioffset_data_h, ioffset_data_l);
}

static void mfc_set_tx_iout(struct mfc_charger_data *charger, unsigned int iout)
{
	u8 iout_data_l = 0, iout_data_h = 0;

	iout_data_l = 0xFF & iout;
	iout_data_h = 0xFF & (iout >> 8);

	mfc_reg_write(charger->client, MFC_TX_IUNO_LIMIT_L_REG, iout_data_l);
	mfc_reg_write(charger->client, MFC_TX_IUNO_LIMIT_H_REG, iout_data_h);

	pr_info("@Tx_Mode %s: Tx Iout set %d(0x%x 0x%x)\n", __func__, iout, iout_data_h, iout_data_l);

	if (iout == 1100)
		mfc_set_tx_ioffset(charger, 100);
	else
		mfc_set_tx_ioffset(charger, 150);
}

static void mfc_set_tx_vout(struct mfc_charger_data *charger, unsigned int vout)
{

	u8 vout_data = 0;

	vout_data = 0x32 + (vout * 5);
	mfc_reg_write(charger->client, MFC_VUNO_REG, vout_data);
	pr_info("@Tx_Mode %s: Tx Vout set %d(0x%x)\n", __func__, vout, vout_data);
}

static void mfc_print_buffer(struct mfc_charger_data *charger, u8 *buffer, int size)
{
	int start_idx = 0;

	do {
		char temp_buf[1024] = {0, };
		int size_temp = 0, str_len = 1024;
		int old_idx = start_idx;

		size_temp = ((start_idx + 0x7F) > size) ? size : (start_idx + 0x7F);
		for (; start_idx < size_temp; start_idx++) {
			snprintf(temp_buf + strlen(temp_buf), str_len, "0x%02x ", buffer[start_idx]);
			str_len = 1024 - strlen(temp_buf);
		}
		pr_info("%s: (%04d ~ %04d) %s\n", __func__, old_idx, start_idx - 1, temp_buf);
	} while (start_idx < size);
}

static int mfc_auth_adt_read(struct mfc_charger_data *charger, u8 *readData)
{
	int buffAddr = MFC_ADT_BUFFER_ADT_PARAM_REG;
	union power_supply_propval value = {0, };
	const int sendsz = 16;
	int i = 0, size = 0;
	int ret = 0;
	u8 size_temp[2] = {0, 0};

	mfc_reg_multi_read(charger->client, MFC_ADT_BUFFER_ADT_TYPE_REG, size_temp, 2);
	adt_readSize = ((size_temp[0] & 0x07) << 8) | (size_temp[1]);
	pr_info("%s %s: (size_temp = 0x%x, readSize = %d bytes)\n",
		WC_AUTH_MSG, __func__, (size_temp[0] | size_temp[1]), adt_readSize);
	if (adt_readSize <= 0) {
		pr_err("%s %s: failed to read adt data size\n", WC_AUTH_MSG, __func__);
		return -EINVAL;
	}
	size = adt_readSize;
	if ((buffAddr + adt_readSize) - 1 > MFC_ADT_BUFFER_ADT_PARAM_MAX_REG) {
		pr_err("%s %s: failed to read adt stream, too large data\n", WC_AUTH_MSG, __func__);
		return -EINVAL;
	}

	if (size <= 2) {
		pr_err("%s %s: data from mcu has invalid size\n", WC_AUTH_MSG, __func__);
		//charger->adt_transfer_status = WIRELESS_AUTH_FAIL;
		/* notify auth service to send TX PAD a request key */
		value.intval = WIRELESS_AUTH_FAIL;
		psy_do_property("wireless", set,
			POWER_SUPPLY_EXT_PROP_WIRELESS_AUTH_ADT_STATUS, value);
		return -EINVAL;
	}

	while (size > sendsz) {
		ret = mfc_reg_multi_read(charger->client, buffAddr, readData + i, sendsz);
		if (ret < 0) {
			pr_err("%s %s: failed to read adt stream (%d, buff %d)\n", WC_AUTH_MSG, __func__, ret, buffAddr);
			return ret;
		}
		i += sendsz;
		buffAddr += sendsz;
		size = size - sendsz;
		pr_info("%s %s: 0x%x %d %d\n", WC_AUTH_MSG, __func__, i, buffAddr, size);
	}

	if (size > 0) {
		ret = mfc_reg_multi_read(charger->client, buffAddr, readData + i, size);
		if (ret < 0) {
			pr_err("%s %s: failed to read adt stream (%d, buff %d)\n", WC_AUTH_MSG, __func__, ret, buffAddr);
			return ret;
		}
	}
	mfc_print_buffer(charger, readData, adt_readSize);

	value.intval = WIRELESS_AUTH_RECEIVED;
	psy_do_property("wireless", set, POWER_SUPPLY_EXT_PROP_WIRELESS_AUTH_ADT_STATUS, value);

	charger->adt_transfer_status = WIRELESS_AUTH_RECEIVED;

	pr_info("%s %s: succeeded to read ADT\n", WC_AUTH_MSG, __func__);

	return 0;
}

static int mfc_auth_adt_write(struct mfc_charger_data *charger, u8 *srcData, int srcSize)
{
	int buffAddr = MFC_ADT_BUFFER_ADT_PARAM_REG;
	u8 wdata = 0;
	u8 sBuf[144] = {0,};
	int ret;
	u8 i;

	pr_info("%s %s: start to write ADT\n", WC_AUTH_MSG, __func__);

	if ((buffAddr + srcSize) - 1 > MFC_ADT_BUFFER_ADT_PARAM_MAX_REG) {
		pr_err("%s %s: failed to write adt stream, too large data.\n", WC_AUTH_MSG, __func__);
		return -EINVAL;
	}

	wdata = 0x08; /* Authentication purpose */
	wdata = wdata | ((srcSize >> 8) & 0x07);
	mfc_reg_write(charger->client, MFC_ADT_BUFFER_ADT_TYPE_REG, wdata);

	wdata = srcSize; /* need to check */
	mfc_reg_write(charger->client, MFC_ADT_BUFFER_ADT_MSG_SIZE_REG, wdata);

	buffAddr = MFC_ADT_BUFFER_ADT_PARAM_REG;
	for (i = 0; i < srcSize; i += 128) { //write each 128byte
		int restSize = srcSize - i;

		if (restSize < 128) {
			memcpy(sBuf, srcData + i, restSize);
			ret = mfc_reg_multi_write_verify(charger->client, buffAddr, sBuf, restSize);
			if (ret < 0) {
				pr_err("%s %s: failed to write adt stream (%d, buff %d)\n", WC_AUTH_MSG, __func__, ret, buffAddr);
				return ret;
			}
			break;
		} else {
			memcpy(sBuf, srcData + i, 128);
			ret = mfc_reg_multi_write_verify(charger->client, buffAddr, sBuf, 128);
			if (ret < 0) {
				pr_err("%s %s: failed to write adt stream (%d, buff %d)\n", WC_AUTH_MSG, __func__, ret, buffAddr);
				return ret;
			}
			buffAddr += 128;
		}

		if (buffAddr > MFC_ADT_BUFFER_ADT_PARAM_MAX_REG - 128)
			break;
	}
	pr_info("%s %s: succeeded to write ADT\n", WC_AUTH_MSG, __func__);
	return 0;
}

static void mfc_auth_adt_send(struct mfc_charger_data *charger, u8 *srcData, int srcSize)
{
	u8 temp;
	int ret = 0;
	u8 irq_src[2];

	mfc_reg_read(charger->client, MFC_INT_A_L_REG, &irq_src[0]);
	mfc_reg_read(charger->client, MFC_INT_A_H_REG, &irq_src[1]);

	charger->adt_transfer_status = WIRELESS_AUTH_SENT;
	ret = mfc_auth_adt_write(charger, srcData, srcSize); /* write buff fw datas to send fw datas to tx */

	mfc_reg_read(charger->client, MFC_AP2MFC_CMD_H_REG, &temp); // check out set bit exists
	pr_info("%s %s: MFC_CMD_H_REG(0x%x)\n", WC_AUTH_MSG, __func__, temp);
	temp |= 0x02;
	pr_info("%s %s: MFC_CMD_H_REG(0x%x)\n", WC_AUTH_MSG, __func__, temp);
	mfc_reg_write(charger->client, MFC_AP2MFC_CMD_H_REG, temp);

	mfc_reg_read(charger->client, MFC_AP2MFC_CMD_H_REG, &temp); // check out set bit exists
	pr_info("%s %s: MFC_CMD_H_REG(0x%x)\n", WC_AUTH_MSG, __func__, temp);

	mfc_reg_update(charger->client, MFC_INT_A_ENABLE_H_REG,
					MFC_STAT_H_ADT_SENT_MASK, MFC_STAT_H_ADT_SENT_MASK);

	/* clear intterupt */
	mfc_reg_write(charger->client, MFC_INT_A_CLEAR_L_REG, irq_src[0]); // clear int
	mfc_reg_write(charger->client, MFC_INT_A_CLEAR_H_REG, irq_src[1]); // clear int
	mfc_set_cmd_l_reg(charger, MFC_CMD_CLEAR_INT_MASK, MFC_CMD_CLEAR_INT_MASK); // command
}

/* uno on/off control function */
static void mfc_set_tx_power(struct mfc_charger_data *charger, bool on)
{
	union power_supply_propval value = {0, };

	charger->wc_tx_enable = on;

	if (on) {
		pr_info("@Tx_Mode %s: Turn On TX Power\n", __func__);
		mfc_uno_on(charger, true);
		msleep(200);

		charger->pdata->otp_firmware_ver = mfc_get_firmware_version(charger, MFC_RX_FIRMWARE);

		cancel_delayed_work(&charger->wpc_tx_op_freq_work);
		__pm_stay_awake(charger->wpc_tx_opfq_ws);
		queue_delayed_work(charger->wqueue, &charger->wpc_tx_op_freq_work, msecs_to_jiffies(5000));
		charger->wc_tx_ac_missing = false;
		charger->wc_rx_fod = false;
		pr_info("%s: tx op freq = %dKhz\n", __func__, mfc_get_adc(charger, MFC_ADC_TX_OP_FRQ));
		mfc_set_min_duty(charger, MIN_DUTY_SETTING_30_DATA);
	} else {
		pr_info("@Tx_Mode %s: Turn Off TX Power, and reset UNO config\n", __func__);

		mfc_uno_on(charger, false);

		value.intval = WC_TX_VOUT_5_0V;
		psy_do_property("otg", set,
			POWER_SUPPLY_EXT_PROP_WIRELESS_TX_VOUT, value);

		charger->wc_rx_connected = false;
		charger->gear_start_time = 0;
#if defined(CONFIG_TX_GEAR_PHM_VOUT_CTRL) || defined(CONFIG_TX_GEAR_AOV)
		charger->tx_gear_phm = 0;
#endif

		cancel_delayed_work(&charger->wpc_rx_type_det_work);
		cancel_delayed_work(&charger->wpc_rx_connection_work);
		cancel_delayed_work(&charger->wpc_tx_isr_work);
		cancel_delayed_work(&charger->wpc_tx_phm_work);
#if defined(CONFIG_TX_GEAR_AOV)
		cancel_delayed_work(&charger->wpc_tx_aov_phm_work);
		__pm_relax(charger->wpc_tx_aov_phm_ws);
#endif
		alarm_cancel(&charger->phm_alarm);
		__pm_relax(charger->wpc_tx_phm_ws);
		__pm_relax(charger->wpc_rx_connection_ws);
		__pm_relax(charger->wpc_rx_det_ws);
		__pm_relax(charger->wpc_tx_ws);

		charger->tx_status = SEC_TX_OFF;
	}
}

/* determine rx connection status with tx sharing mode */
static void mfc_wpc_rx_connection_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_rx_connection_work.work);
	union power_supply_propval value = {0, };

	if (!charger->wc_tx_enable) {
		__pm_relax(charger->wpc_rx_connection_ws);
		return;
	}

	if (charger->wc_rx_connected) {
		pr_info("@Tx_Mode %s: Rx(%s) connected\n", __func__, mfc_tx_function_check(charger) ? "Samsung" : "Other");

		cancel_delayed_work(&charger->wpc_rx_type_det_work);
		__pm_stay_awake(charger->wpc_rx_det_ws);
		queue_delayed_work(charger->wqueue, &charger->wpc_rx_type_det_work, msecs_to_jiffies(1000));
	} else {
		pr_info("@Tx_Mode %s: Rx disconnected\n", __func__);

		charger->wc_rx_fod = false;
		charger->gear_start_time = 0;
		charger->wc_rx_type = NO_DEV;
#if defined(CONFIG_TX_GEAR_PHM_VOUT_CTRL) || defined(CONFIG_TX_GEAR_AOV)
		charger->tx_gear_phm = 0;
#endif

		if (delayed_work_pending(&charger->wpc_rx_type_det_work)) {
			__pm_relax(charger->wpc_rx_det_ws);
			cancel_delayed_work(&charger->wpc_rx_type_det_work);
		}

		if (delayed_work_pending(&charger->wpc_tx_op_freq_work)) {
			__pm_relax(charger->wpc_tx_opfq_ws);
			cancel_delayed_work(&charger->wpc_tx_op_freq_work);
		}
	}

	/* set rx connection condition */
	value.intval = charger->wc_rx_connected;
	psy_do_property("wireless", set, POWER_SUPPLY_EXT_PROP_WIRELESS_RX_CONNECTED, value);

	if (charger->wc_tx_ac_missing && !charger->wc_rx_connected) {
		pr_info("@Tx_Mode %s: tx_ac_missing: tx off\n", __func__);
		value.intval = 0;
		psy_do_property("battery", set, POWER_SUPPLY_EXT_PROP_WIRELESS_TX_ENABLE, value);

		__pm_stay_awake(charger->wpc_tx_ac_missing_ws);
		queue_delayed_work(charger->wqueue, &charger->wpc_tx_ac_missing_work, msecs_to_jiffies(600));

		charger->wc_tx_ac_missing = false;
	}
	__pm_relax(charger->wpc_rx_connection_ws);
}

/*
 * determine rx connection status with tx sharing mode,
 * this TX device has MFC_INTA_H_TRX_DATA_RECEIVED_MASK irq and RX device is connected HV wired cable
 */
static void mfc_tx_handle_rx_packet(struct mfc_charger_data *charger)
{
	u8 cmd_data = 0, val1_data = 0, val2_data = 0;
	union power_supply_propval value = {0, };

	mfc_reg_read(charger->client, MFC_WPC_TRX_DATA2_COM_REG, &cmd_data);
	mfc_reg_read(charger->client, MFC_WPC_TRX_DATA2_VALUE1_REG, &val1_data);
	mfc_reg_read(charger->client, MFC_WPC_TRX_DATA2_VALUE2_REG, &val2_data);
	charger->pdata->trx_data_cmd = cmd_data;
	charger->pdata->trx_data_val = val1_data;

	pr_info("@Tx_Mode %s: CMD : 0x%x, DATA : 0x%x, DATA2 : 0x%x\n", __func__, cmd_data, val1_data, val2_data);

	/* When RX device has got a AFC TA, this TX device should turn off TX power sharing(uno) */
	if (cmd_data == MFC_HEADER_AFC_CONF) {	/* 0x28 */
		switch (val1_data) {
		case WPC_COM_DISABLE_TX:	/* 0x19 Turn off UNO of TX */
			if (val2_data == RX_DATA_VAL2_WATCH_MISALIGN) {
				pr_info("@Tx_Mode %s: Gear send TX off packet by Misalign or darkzone issue\n", __func__);
				value.intval = BATT_TX_EVENT_WIRELESS_TX_MISALIGN;
				psy_do_property("wireless", set, POWER_SUPPLY_EXT_PROP_WIRELESS_TX_ERR, value);
			} else if (val2_data == RX_DATA_VAL2_TA_CONNECT_DURING_WC) {
				pr_info("@Tx_Mode %s: this TX device should turn off TX power sharing(uno) since RX device has a AFC TA\n", __func__);
				value.intval = BATT_TX_EVENT_WIRELESS_RX_CHG_SWITCH;
				psy_do_property("wireless", set, POWER_SUPPLY_EXT_PROP_WIRELESS_TX_ERR, value);
			}
			break;
		case WPC_COM_ENTER_PHM:		/* 0x18 GEAR entered PHM */
			if (val2_data == RX_DATA_VAL2_ENABLE) {
				if (charger->wc_rx_type == SS_GEAR) {
					/* start 3min alarm timer */
					pr_info("%s: Start PHM alarm by 3min\n", __func__);
					alarm_start(&charger->phm_alarm,
					ktime_add(ktime_get_boottime(), ktime_set(180, 0)));

#if defined(CONFIG_TX_GEAR_AOV)
					charger->tx_gear_phm = 1;
					value.intval = 1;
					psy_do_property("wireless", set,
						POWER_SUPPLY_EXT_PROP_GEAR_PHM_EVENT, value);
#endif
				} else {
					pr_info("%s: TX entered PHM but no 3min timer\n", __func__);
				}
			}
			break;
		case WPC_COM_RX_ID:		/* 0x0E Received RX ID */
			if (val2_data >= RX_DATA_VAL2_RXID_ACC_BUDS && val2_data <= RX_DATA_VAL2_RXID_ACC_BUDS_MAX) {	/* Buds */
				charger->wc_rx_type = SS_BUDS;
				pr_info("@Tx_Mode %s: Buds connected\n", __func__);

				value.intval = charger->wc_rx_type;
				psy_do_property("wireless", set,
						POWER_SUPPLY_EXT_PROP_WIRELESS_RX_TYPE, value);
			}
			break;
		default:
			pr_info("@Tx_Mode %s: unsupport : 0x%x", __func__, val1_data);
			break;
		}
	} else if (cmd_data == MFC_HEADER_END_CHARGE_STATUS) {	/* 0x05 Charge Stop */
		if (val1_data == MFC_ECS_CS100) {	/* CS 100 */
			pr_info("%s: CS100 Received, TX power off\n", __func__);
			value.intval = BATT_TX_EVENT_WIRELESS_RX_CS100;
			psy_do_property("wireless", set, POWER_SUPPLY_EXT_PROP_WIRELESS_TX_ERR, value);
		}
	} else if (cmd_data == MFC_HEADER_END_POWER_TRANSFER) {	/* 0x02 AFC_TX */
		if (val1_data == MFC_EPT_CODE_OVER_TEMPERATURE) {
			pr_info("%s: EPT-OT Received, TX power off\n", __func__);
			value.intval = BATT_TX_EVENT_WIRELESS_RX_UNSAFE_TEMP;
			psy_do_property("wireless", set, POWER_SUPPLY_EXT_PROP_WIRELESS_TX_ERR, value);
		}
	}
}

static int datacmp(const char *cs, const char *ct, int count)
{
	unsigned char c1, c2;

	while (count) {
		c1 = *cs++;
		c2 = *ct++;
		if (c1 != c2) {
			pr_err("%s: cnt %d\n", __func__, count);
			return c1 < c2 ? -1 : 1;
		}
		count--;
	}
	return 0;
}

static int mfc_reg_multi_write_verify(struct i2c_client *client, u16 reg, const u8 *val, int size)
{
	int ret = 0;
	const int sendsz = 16;
	int cnt = 0;
	int retry_cnt = 0;
	unsigned char data[sendsz + 2];
	u8 rdata[sendsz + 2];

//	dev_dbg(&client->dev, "%s: size: 0x%x\n", __func__, size);
	while (size > sendsz) {
		data[0] = (reg + cnt) >> 8;
		data[1] = (reg + cnt) & 0xFF;
		memcpy(data + 2, val + cnt, sendsz);
//		dev_dbg(&client->dev, "%s: addr: 0x%x, cnt: 0x%x\n", __func__, reg + cnt, cnt);
		ret = i2c_master_send(client, data, sendsz + 2);
		if (ret < sendsz + 2) {
			pr_err("%s: i2c write error, reg: 0x%x\n", __func__, reg);
			return ret < 0 ? ret : -EIO;
		}
		if (mfc_reg_multi_read(client, reg + cnt, rdata, sendsz) < 0) {
			pr_err("%s: read failed(%d)\n", __func__, reg + cnt);
			return -1;
		}
		if (datacmp(val + cnt, rdata, sendsz)) {
			pr_err("%s: data is not matched. retry(%d)\n", __func__, retry_cnt);
			retry_cnt++;
			if (retry_cnt > 4) {
				pr_err("%s: data is not matched. write failed\n", __func__);
				retry_cnt = 0;
				return -1;
			}
			continue;
		}
//		pr_debug("%s: data is matched!\n", __func__);
		cnt += sendsz;
		size -= sendsz;
		retry_cnt = 0;
	}
	while (size > 0) {
		data[0] = (reg + cnt) >> 8;
		data[1] = (reg + cnt) & 0xFF;
		memcpy(data + 2, val + cnt, size);
//		dev_dbg(&client->dev, "%s: addr: 0x%x, cnt: 0x%x, size: 0x%x\n", __func__, reg + cnt, cnt, size);
		ret = i2c_master_send(client, data, size + 2);
		if (ret < size + 2) {
			pr_err("%s: i2c write error, reg: 0x%x\n", __func__, reg);
			return ret < 0 ? ret : -EIO;
		}
		if (mfc_reg_multi_read(client, reg + cnt, rdata, size) < 0) {
			pr_err("%s: read failed(%d)\n", __func__, reg + cnt);
			return -1;
		}
		if (datacmp(val + cnt, rdata, size)) {
			pr_err("%s: data is not matched. retry(%d)\n", __func__, retry_cnt);
			retry_cnt++;
			if (retry_cnt > 4) {
				pr_err("%s: data is not matched. write failed\n", __func__);
				retry_cnt = 0;
				return -1;
			}
			continue;
		}
		size = 0;
		pr_err("%s: data is matched!\n", __func__);
	}
	return ret;
}

EXTERN_MFC_FUNC
int mfc_reg_multi_write(struct i2c_client *client, u16 reg, const u8 *val, int size)
{
	struct mfc_charger_data *charger = i2c_get_clientdata(client);
	int ret = 0;
	const int sendsz = 16;
	unsigned char data[sendsz + 2];
	int cnt = 0;

	pr_err("%s: size: 0x%x\n", __func__, size);
	while (size > sendsz) {
		data[0] = (reg + cnt) >> 8;
		data[1] = (reg + cnt) & 0xff;
		memcpy(data + 2, val + cnt, sendsz);
		mutex_lock(&charger->io_lock);
		ret = i2c_master_send(client, data, sendsz + 2);
		mutex_unlock(&charger->io_lock);
		if (ret < sendsz + 2) {
			pr_err("%s: i2c write error, reg: 0x%x\n", __func__, reg);
			return ret < 0 ? ret : -EIO;
		}
		cnt = cnt + sendsz;
		size = size - sendsz;
	}
	if (size > 0) {
		data[0] = (reg + cnt) >> 8;
		data[1] = (reg + cnt) & 0xff;
		memcpy(data + 2, val + cnt, size);
		mutex_lock(&charger->io_lock);
		ret = i2c_master_send(client, data, size + 2);
		mutex_unlock(&charger->io_lock);
		if (ret < size + 2) {
			dev_err(&client->dev, "%s: i2c write error, reg: 0x%x\n", __func__, reg);
			return ret < 0 ? ret : -EIO;
		}
	}

	return ret;
}

static int LoadOTPLoaderInRAM(struct mfc_charger_data *charger, u16 addr)
{
	int i, size;
	u8 data[1024];

	if (mfc_reg_multi_write_verify(charger->client, addr, MTPBootloader9320, sizeof(MTPBootloader9320)) < 0) {
		pr_err("%s: fail", __func__);
	}
	size = sizeof(MTPBootloader9320);
	i = 0;
	while(size > 0) {
		if (mfc_reg_multi_read(charger->client, addr + i, data + i, 16) < 0) {
			pr_err("%s: read failed(%d)", __func__, addr + i);
			return MFC_FWUP_ERR_ADDR_READ_FAIL;
		}
		i += 16;
		size -= 16;
	}
	size = sizeof(MTPBootloader9320);
	if (datacmp(data, MTPBootloader9320, size)) {
		pr_err("%s: data is not matched\n", __func__);
		return MFC_FWUP_ERR_DATA_NOT_MATCH;
	}
	return MFC_FWUP_ERR_SUCCEEDED;
}

static int mfc_firmware_verify(struct mfc_charger_data *charger)
{
	int ret = 0;
	const u16 sendsz = 16;
	size_t i = 0;
	int block_len = 0;
	int block_addr = 0;
	u8 rdata[sendsz + 2];

/* I2C WR to prepare boot-loader write */

	if (mfc_reg_write(charger->client, 0x3000, 0x5a) < 0) {
		pr_err("%s: key error\n", __func__);
		return 0;
	}

	if (mfc_reg_write(charger->client, 0x3040, 0x11) < 0) {
		pr_err("%s: halt M0, OTP_I2C_EN set error\n", __func__);
		return 0;
	}

	dev_err(&charger->client->dev, "%s: request_firmware\n", __func__);
	ret = request_firmware(&charger->firm_data_bin, MFC_FLASH_FW_HEX_PATH,
		&charger->client->dev);
	if (ret < 0) {
		dev_err(&charger->client->dev, "%s: failed to request firmware %s(%d)\n",
				__func__, MFC_FLASH_FW_HEX_PATH, ret);
		return 0;
	}
	ret = 1;
	__pm_stay_awake(charger->wpc_update_ws);
	for (i = 0; i < charger->firm_data_bin->size; i += sendsz) {
		block_len = (i + sendsz) > charger->firm_data_bin->size ? charger->firm_data_bin->size - i : sendsz;
		block_addr = 0x8000 + i;

		if (mfc_reg_multi_read(charger->client, block_addr, rdata, block_len) < 0) {
			pr_err("%s: read failed\n", __func__);
			ret = 0;
			break;
		}
		if (datacmp(charger->firm_data_bin->data + i, rdata, block_len)) {
			pr_err("%s: verify data is not matched. block_len(%d), block_addr(%d)\n",
				__func__, block_len, block_addr);
			ret = -1;
			break;
		}
	}
	release_firmware(charger->firm_data_bin);

	__pm_relax(charger->wpc_update_ws);
	return ret;
}

bool WriteWordToMtp(struct mfc_charger_data *charger, u16 StartAddr, u32 data)
{
	int j, cnt;
	u8 sBuf[16] = {0,};
	u16 CheckSum = StartAddr;
	u16 CodeLength = 4;
	//*(u32*)&sBuf[8] = data;
	sBuf[8] = (u8)(data >> 0);
	sBuf[9] = (u8)(data >> 8);
	sBuf[10] = (u8)(data >> 16);
	sBuf[11] = (u8)(data >> 24);

	pr_info("%s: changed sBuf codes\n", __func__);
	for (j = 3; j >= 0; j--)
		CheckSum += sBuf[j + 8];	// add the non zero values
	CheckSum += CodeLength;			// finish calculation of the check sum
	//*(u16*)&sBuf[2] = StartAddr;
	//*(u16*)&sBuf[4] = CodeLength;
	//*(u16*)&sBuf[6] = CheckSum;
	sBuf[2] = (u8)(StartAddr >> 0);
	sBuf[3] = (u8)(StartAddr >> 8);
	sBuf[4] = (u8)(CodeLength >> 0);
	sBuf[5] = (u8)(CodeLength >> 8);
	sBuf[6] = (u8)(CheckSum >> 0);
	sBuf[7] = (u8)(CheckSum >> 8);

	if (mfc_reg_multi_write(charger->client, 0x400, sBuf, 4 + 8) < 0)
	{
		pr_err("ERROR: on writing to OTP buffer");
		return false;
	}

	sBuf[0] = 0x01;
	if (mfc_reg_write(charger->client, 0x400, sBuf[0]) < 0)
	{
		pr_err("ERROR: on OTP buffer validation");
		return false;
	}

	cnt = 0;
	do {
		msleep(20);
		if (mfc_reg_read(charger->client, 0x400, sBuf) < 0)
		{
			pr_err("ERROR: on reading OTP buffer status(%d)", cnt);
			return false;
		}

		if (cnt > 1000) {
			pr_err("ERROR: time out on buffer program to OTP");
			break;
		}
		cnt++;
	} while ((sBuf[0]&1) != 0);

	if (sBuf[0] != 2) // not OK
	{
		pr_err("ERROR: buffer write to OTP returned status %d", sBuf[0]);
		return false;
	}
	return true;
}

int mfc_check_idt_info_page(struct mfc_charger_data *charger)
{
	u8 i = 0;
	u8 rdata = 0;
	u16 BaseAddr = 0x4000;
	u16 infoPageAddr = BaseAddr + 0x8000;	// Compensate for 0x8000 offset when reading MTP
	u8 numBytesToCheck = 8;

	/*
	3.1.	Configure clocks and timing
		3.1.1.	Write 0x00 to address 0x3004
		3.1.2.	Write 0x09 to address 0x3008
		3.1.3.	Write 0x05 to address 0x300c
		3.1.4.	Write 0x1D to address 0x300d
	*/
	if (mfc_reg_write(charger->client, 0x3004, 0x00) < 0) {
		pr_err("%s: Configure clocks and timing error (1)\n", __func__);
		return MFC_FWUP_ERR_CLK_TIMING_ERR1;
	}
	if (mfc_reg_write(charger->client, 0x3008, 0x09) < 0) {
		pr_err("%s: Configure clocks and timing error (2)\n", __func__);
		return MFC_FWUP_ERR_CLK_TIMING_ERR2;
	}
	if (mfc_reg_write(charger->client, 0x300c, 0x05) < 0) {
		pr_err("%s: Configure clocks and timing error (3)\n", __func__);
		return MFC_FWUP_ERR_CLK_TIMING_ERR3;
	}
	if (mfc_reg_write(charger->client, 0x300d, 0x1D) < 0) {
		pr_err("%s: Configure clocks and timing error (4)\n", __func__);
		return MFC_FWUP_ERR_CLK_TIMING_ERR4;
	}

	/*
	3.2.	Pause the processor and enable MTP access via I2C
		3.2.1.	Write 0x11 to address 0x3040
	*/
	if (mfc_reg_write(charger->client, 0x3040, 0x11) < 0) {
		pr_err("%s: Pause the processor and enable MTP access via I2C error\n", __func__);
		return MFC_FWUP_ERR_INFO_PAGE_EMPTY;
	}

	/*
	3.3.	Begin reading MTP
		3.3.1.	Read 8 bytes starting from 0xc000 and if all bytes == 0xFF then exit
	*/
	for (i = 0; i < numBytesToCheck; i++)
	{
		if (mfc_reg_read(charger->client, infoPageAddr, &rdata) < 0)
			return MFC_FWUP_ERR_INFO_PAGE_EMPTY;

		if (rdata != 0xFF) {
			pr_info("%s: Info page is programmed, f/w update allowed\n", __func__);
			return MFC_FWUP_ERR_SUCCEEDED;
		}
		// All bytes have data other than 0xFF
		if (i == (numBytesToCheck - 1)) {
			pr_info("%s: Info page is empty\n", __func__);
			return MFC_FWUP_ERR_INFO_PAGE_EMPTY;
		}
	}
	return MFC_FWUP_ERR_INFO_PAGE_EMPTY;
}

/* Load MTP utility in RAM */
static int LoadMTPVerifierInRAM(struct mfc_charger_data *charger, u16 addr)
{
	int i, size;
	void* buf = NULL;
	int numOfByteToRead; // set the number of bytes to read

	buf = kzalloc(sizeof(MTPVerifier9320), GFP_KERNEL);
	if (!buf) {
		pr_err("%s: kzalloc failed\n", __func__);
		return 0;
	}
	size = sizeof(MTPVerifier9320);

	if (mfc_reg_multi_write_verify(charger->client, addr, MTPVerifier9320, size) < 0) {
		pr_err("%s: fail", __func__);
	}
	i = 0;
	while(size > 0) {
		/* It reads 16 bytes in normal cases,
		when remaining size is less than 16, set it to the small size.
		This is to deal with the last 4 bytes in updated MTPVerifier9320[] */
		numOfByteToRead = size >= 16 ? 16 : size;
		if (mfc_reg_multi_read(charger->client, addr + i, buf + i, numOfByteToRead) < 0) {
			pr_err("%s: read failed(%d)", __func__, addr + i);
			kfree(buf);
			return MFC_VERIFY_ERR_ADDR_READ_FAIL;
		}
		i += 16;
		size -= 16;
	}

	size = sizeof(MTPVerifier9320); // 'size' is changed, get MTPVerifier9320 size again
	if (datacmp(buf, MTPVerifier9320, size)) {
		pr_err("%s: data is not matched\n", __func__);
		kfree(buf);
		return MFC_VERIFY_ERR_DATA_NOT_MATCH;
	}

	kfree(buf);
	return MFC_FWUP_ERR_SUCCEEDED;
}

/*Setup MTP to verify */
static int VerifyMTPwRAM_IDT(struct mfc_charger_data *charger, unsigned short start_addr,
							unsigned short size, unsigned short checksum)
{
	u8 sBuf[8];
	u16 mtp_verify_cnt = 0;
	int ret;

	if (charger->pdata->wpc_en >= 0)
		gpio_direction_output(charger->pdata->wpc_en, 1);
	mfc_uno_on(charger, false);
	msleep(1000);
	mfc_uno_on(charger, true);
	if (charger->pdata->wpc_en >= 0)
		gpio_direction_output(charger->pdata->wpc_en, 0);
	msleep(300);

	pr_info("%s %s: start\n", MFC_FW_MSG, __func__);

	if (mfc_reg_write(charger->client, 0x3000, 0x5a) < 0) {
		pr_err("%s %s: write key error\n", MFC_FW_MSG, __func__);
		return MFC_VERIFY_ERR_WRITE_KEY_ERR;		// write key
	}

	msleep(10);
	if (mfc_reg_write(charger->client, 0x3040, 0x10) < 0) {
		pr_err("%s %s: halt M0 error\n", MFC_FW_MSG, __func__);
		return MFC_VERIFY_ERR_HALT_M0_ERR;		// halt M0
	}

	msleep(10);
	ret = LoadMTPVerifierInRAM(charger, 0x800);
	if (ret != MFC_FWUP_ERR_SUCCEEDED){
		pr_err("%s %s: LoadMTPVerifierInRAM error\n", MFC_FW_MSG, __func__);
		return ret;		// make sure load address and 1KB size are OK
	}

	msleep(10);
	// Clear MTP program status byte
	if (mfc_reg_write(charger->client, 0x0400, 0x00) < 0) {
			pr_err("%s %s: clear MTP verifier status byte error\n", MFC_FW_MSG, __func__);
		return MFC_VERIFY_ERR_CLR_MTP_STATUS_BYTE;
	}

	msleep(10);
	if (mfc_reg_write(charger->client, 0x3048, 0xd0) < 0) {
		pr_err("%s %s: map RAM to MTP error\n", MFC_FW_MSG, __func__);
		return MFC_VERIFY_ERR_MAP_RAM_TO_OTP_ERR;		// map RAM to MTP
	}

	// Check Key lock state
	mfc_reg_write(charger->client, 0x3040, 0x80); //M0 RESET : P9320 will not acknowledge for this transaction !!
	msleep(100);

	if (mfc_reg_write(charger->client, 0x3000, 0x5A) < 0) {	//unlock system registers
		pr_err("%s %s: unlock system registers error\n", MFC_FW_MSG, __func__);
		return MFC_VERIFY_ERR_UNLOCK_SYS_REG_ERR;		// unlock system registers
	}

	if (mfc_reg_write(charger->client, 0x300D, 0x04) < 0) {	//set LDO clock to 2MHz
		pr_err("%s %s: set LDO clock to 2MHz error\n", MFC_FW_MSG, __func__);
		return MFC_VERIFY_ERR_LDO_CLK_2MHZ_ERR;		// set LDO to 2MHz
	}

	if (mfc_reg_write(charger->client, 0x3010, 0x6C) < 0) {	//set LDO output voltage as 5.5V
		pr_err("%s %s: set LDO output voltage as 5.5V error\n", MFC_FW_MSG, __func__);
		return MFC_VERIFY_ERR_LDO_OUTPUT_5_5V_ERR;		// set LDO voltage to 5.5V
	}

	if (mfc_reg_write(charger->client, 0x301C, 0x09) < 0) {	//enable LDO
		pr_err("%s %s: enable LDO error\n", MFC_FW_MSG, __func__);
		return MFC_VERIFY_ERR_ENABLE_LDO_ERR;		// enable LDO
	}

	sBuf[0] = 0;
	sBuf[1] = 0;
	sBuf[2] = (unsigned char)(start_addr >> 0);
	sBuf[3] = (unsigned char)(start_addr >> 8);
	sBuf[4] = (unsigned char)(size >> 0);
	sBuf[5] = (unsigned char)(size >> 8);
	sBuf[6] = (unsigned char)(checksum >> 0);
	sBuf[7] = (unsigned char)(checksum >> 8);

	msleep(10);
	if (mfc_reg_multi_write(charger->client, 0x400, sBuf, sizeof(sBuf)) < 0)
	{
		pr_err("%s ERROR: on writing to MTP verification buffer", MFC_FW_MSG);
		return MFC_VERIFY_ERR_WRITING_TO_MTP_VERIFY_BUFFER;
	}

	// Start MTP verification
	msleep(10);
	if (mfc_reg_write(charger->client, 0x400, 0x11) < 0) {
		pr_err("%s %s: Start MTP verification error\n", MFC_FW_MSG, __func__);
		return MFC_VERIFY_ERR_START_MTP_VERIFY_ERR;		// map RAM to MTP
	}

	do
	{
		msleep(20);
		if (mfc_reg_read(charger->client, 0x401, sBuf) < 0)
		{
			pr_err("%s ERROR: on reading MTP verification status(%d)", MFC_FW_MSG, mtp_verify_cnt);
			return MFC_VERIFY_ERR_READING_MTP_VERIFY_STATUS;
		}
		if (mtp_verify_cnt > 1000) {
			pr_err("%s ERROR: time out on buffer verification program to MTP", MFC_FW_MSG);
			break;
		}
		mtp_verify_cnt++;
	} while ((sBuf[0] & 1) != 0);

	msleep(10);
	if (mfc_reg_read(charger->client, 0x401, sBuf) < 0)
	{
		pr_err("%s ERROR: on reading MTP verification pass/fail", MFC_FW_MSG);
		return false;
	}

	if (sBuf[0] != 2) // not OK
	{
		if (sBuf[0] == 1) {
			ret = MFC_VERIFY_ERR_CRC_BUSY;
			pr_err("%s: CRC BUSY ERROR\n", MFC_FW_MSG);
		} else if (sBuf[0] == 4) {
			ret = MFC_VERIFY_ERR_CRC_ERROR;
			pr_err("%s: CRC ERROR\n", MFC_FW_MSG);
		} else {
			ret = MFC_VERIFY_ERR_UNKNOWN_ERR;
			pr_err("%s UNKNOWN ERR\n", MFC_FW_MSG);
		}
		pr_err("%s ERROR: buffer write to OTP returned status %d in sector 0x%x ",
			MFC_FW_MSG, sBuf[0], mtp_verify_cnt);
		return ret;
	}

	msleep(10);

	pr_err("MTP verification finished in");
	return MFC_FWUP_ERR_SUCCEEDED;
}

/* Load MTP Repair Utility in RAM */
static int LoadMTPRepairInRAM(struct mfc_charger_data *charger, u16 addr)
{
	int i, size;
	void* buf = NULL;
	int numOfByteToRead;	// set the number of bytes to read

	buf = kzalloc(sizeof(MTPRepair9320), GFP_KERNEL);
	if (!buf) {
		pr_err("%s: kzalloc failed\n", __func__);
		return 0;
	}
	size = sizeof(MTPRepair9320);

	if (mfc_reg_multi_write_verify(charger->client, addr, MTPRepair9320, size) < 0) {
		pr_err("%s: fail", __func__);
	}
	i = 0;
	while (size > 0) {
		/* It reads 16 bytes in normal cases, when remaining
		   size is less than 16, set it to the small
		   size. This is to deal with the last 4 bytes
		   in updated MTPVerifier9320[] */
		numOfByteToRead = size >= 16 ? 16 : size;

		if (mfc_reg_multi_read(charger->client, addr + i, buf + i, numOfByteToRead) < 0) {
			pr_err("%s: read failed(%d)", __func__, addr + i);
			kfree(buf);
			return 0;
		}
		i += 16;
		size -= 16;
	}

	size = sizeof(MTPRepair9320);	// 'size' is changed, get MTPVerifier9320 size again
	if (datacmp(buf, MTPRepair9320, size)) {
		pr_err("%s: data is not matched\n", __func__);
		kfree(buf);
		return 0;
	}

	kfree(buf);
	return 1;
}


/*Setup MTP to verify */
static int RepairMTPwRAM_IDT(struct mfc_charger_data *charger)
{
	u16 mtp_repair_cnt = 0;
	u8 status;

	if (mfc_reg_write(charger->client, 0x3040, 0x10) < 0) {
		pr_err("%s: halt M0 error\n", __func__);
		return MFC_REPAIR_ERR_HALT_M0_ERR;		// halt M0
	}

	msleep(10);
	if (!LoadMTPRepairInRAM(charger, 0x800)) {
		pr_err("%s: LoadMTPRepairInRAM error\n", __func__);
		return MFC_REPAIR_ERR_MTP_REPAIR_IN_RAM;
	}

	msleep(10);
	// Clear MTP program status byte
	if (mfc_reg_write(charger->client, 0x0400, 0x00) < 0) {
		pr_err("%s: clear MTP repair program status byte error\n", __func__);
		return MFC_REPAIR_ERR_CLR_MTP_STATUS_BYTE;
	}

	// Check Key lock state
	mfc_reg_write(charger->client, 0x3040, 0x80); //M0 RESET : P9320 will not acknowledge for this transaction !!
	msleep(100);

	// Start MTP repair
	msleep(10);
	if (mfc_reg_write(charger->client, 0x400, 0x1) < 0) {
		pr_err("%s: Start MTP Repair error\n", __func__);
		return MFC_REPAIR_ERR_START_MTP_REPAIR_ERR;		// map RAM to MTP
	}
	do
	{
		msleep(20);
		if (mfc_reg_read(charger->client, 0x400, &status) < 0)
		{
			pr_err("ERROR: on reading MTP repair program status(%d)", mtp_repair_cnt);
			return MFC_REPAIR_ERR_READING_MTP_REPAIR_STATUS;
		}
		if (mtp_repair_cnt > 1000) {
			pr_err("ERROR: time out on MTP repair program");
			break;
		}
		mtp_repair_cnt++;
	} while ((status & 1) != 0); // bit 0 will be cleared once the program is finished

	msleep(10);
	if (mfc_reg_read(charger->client, 0x400, &status) < 0)
	{
		pr_err("ERROR: on reading MTP repair status pass/fail");
		return MFC_REPAIR_ERR_READING_MTP_REPAIR_PASS_FAIL;
	}

	if (status != 2) // not OK
	{
		if (status == 64)
			pr_err("MTP REPAIR ERROR\n");
		else
			pr_err("UNKNOWN ERR\n");
		pr_err("ERROR: buffer write to MTP returned status %d in sector 0x%x ", status, mtp_repair_cnt);
		return MFC_REPAIR_ERR_BUFFER_WRITE_IN_SECTOR;
	}

	msleep(10);

	pr_err("MTP Repair finished");
	return MFC_FWUP_ERR_SUCCEEDED;
}

static int PgmOTPwRAM_IDT(struct mfc_charger_data *charger, unsigned short OtpAddr,
					  const u8 * srcData, int srcOffs, int size, bool repairEn)
{
	int i, cnt, ret;
	u8 fw_major[2] = {0,};
	u8 fw_minor[2] = {0,};
	u16 BaseAddr = 0, Offset = 0;
	u32 fw_ver = 0;

	pr_info("%s %s: start\n", MFC_FW_MSG, __func__);

	msleep(10);
	if (mfc_reg_write(charger->client, 0x3000, 0x5a) < 0) {
		pr_err("%s: write key error\n", __func__);
		return MFC_FWUP_ERR_WRITE_KEY_ERR;		// write key
	}

	ret = mfc_check_idt_info_page(charger);
	if (ret != MFC_FWUP_ERR_SUCCEEDED) {
		pr_info("%s: f/w update NOT allowed, info page is empty!!!\n", __func__);
		return ret;
	}

	msleep(10);
	if (mfc_reg_write(charger->client, 0x3040, 0x10) < 0) {
		pr_err("%s: halt M0 error\n", __func__);
		return MFC_FWUP_ERR_HALT_M0_ERR;		// halt M0
	}
	msleep(10);
	ret = LoadOTPLoaderInRAM(charger, 0x0800);
	if (ret != MFC_FWUP_ERR_SUCCEEDED){
		pr_err("%s: LoadOTPLoaderInRAM error\n", __func__);
		return ret;		// make sure load address and 1KB size are OK
	}
	msleep(10);

	// Clear MTP program status byte
	if (mfc_reg_write(charger->client, 0x0400, 0x00) < 0) {
			pr_err("%s: clear MTP programming status byte error\n", __func__);
		return MFC_FWUP_ERR_CLR_MTP_STATUS_BYTE;
	}

	if (mfc_reg_write(charger->client, 0x3048, 0xD0) < 0) {
		pr_err("%s: map RAM to OTP error\n", __func__);
		return MFC_FWUP_ERR_MAP_RAM_TO_OTP_ERR;		// map RAM to OTP
	}

	// Check Key lock state
	mfc_reg_write(charger->client, 0x3040, 0x80); //M0 RESET : P9320 will not acknowledge for this transaction !!
	msleep(100);

	Offset = MFC_FW_BIN_VERSION_ADDR % 128;
	BaseAddr = MFC_FW_BIN_VERSION_ADDR - Offset;
	pr_info("%s: %x, %x\n", __func__, BaseAddr, Offset);

	// Limit programming size to 16KB
	size = ((size > MTP_MAX_PROGRAM_SIZE)? MTP_MAX_PROGRAM_SIZE : size);
	pr_info("%s: start to write f/w bin to mtp\n", __func__);
	for (i = 0; i < size; i += 128)	// program pages of 128 bytes
	{
		u8 sBuf[144] = {0,};	// align size in 16 bytes boundary. may not be important for SS
		u16 StartAddr = (u16)i;
		u16 CheckSum = StartAddr;
		u16 CodeLength = 128;
		int j;
		memcpy(sBuf + 8, srcData + i + srcOffs, 128);

		if (i == BaseAddr) { // the FW rev address for rev 6 is 0x167C. 0x1280 is the half page base address.
			//*(u32*)&sBuf[8 + 0x28] = 0;
			fw_major[0] = sBuf[Offset + 8];
			fw_major[1] = sBuf[Offset + 8 + 1];
			fw_minor[0] = sBuf[Offset + 8 + 2];
			fw_minor[1] = sBuf[Offset + 8 + 3];
			sBuf[Offset + 8] = 0;
			sBuf[Offset + 8 + 1] = 0;
			sBuf[Offset + 8 + 2] = 0;
			sBuf[Offset + 8 + 3] = 0;
		}

		j = size - i;	// calculate how many bytes need to be programmed in the current run and round up to 16

		if (j < 128)
		{
			j = ((j + 15) / 16) * 16;
			CodeLength = (u16)j;
		}
		else
		{
			j = 128;
		}
		j -= 1;	// compensate for index

		for (; j >= 0; j--)
			CheckSum += sBuf[j + 8];	// add the non zero values
		CheckSum += CodeLength;		// finish calculation of the check sum
		memcpy(sBuf + 2, &StartAddr, 2);
		memcpy(sBuf + 4, &CodeLength, 2);
		memcpy(sBuf + 6, &CheckSum, 2);

		// FOR REFERENCE HERE IS THE DATA STRUCTURE
		//typedef struct {
		//		   u16 Status;
		//		   u16 StartAddr;
		//		   u16 CodeLength;
		//		   u16 DataChksum;
		//		   u8  DataBuf[128];
		//}  P9320PgmStrType;	 // the structure is located at address 0x400

		// TODO sBuf[0] = 0x00; // done during initialization.

		if (mfc_reg_multi_write(charger->client, 0x400, sBuf, ((CodeLength + 8 + 15) / 16) * 16) < 0)
		{	// TODO the write size is aligned to 16 bytes. SS may not need to do this.
			pr_err("ERROR: on writing to OTP buffer");
			return MFC_FWUP_ERR_WRITING_TO_OTP_BUFFER;
		}
		sBuf[0] = 0x01;	// TODO write 0x11 if Vrect is powered from 5V
			//write 0x31 if Vrect is powered from 8.2V
			//write 0x01 if Vrect is 5V and there is a problem

		if (mfc_reg_write(charger->client, 0x400, sBuf[0]) < 0)
		{
			pr_err("ERROR: on OTP buffer validation");
			return MFC_FWUP_ERR_OTF_BUFFER_VALIDATION;
		}
		cnt = 0;
		do
		{
			msleep(20);
			if (mfc_reg_read(charger->client, 0x400, sBuf) < 0)
			{
				pr_err("ERROR: on reading OTP buffer status(%d)", cnt);
				return MFC_FWUP_ERR_READING_OTP_BUFFER_STATUS;
			}
			if (cnt > 1000) {
				pr_err("ERROR: time out on buffer program to OTP");
				break;
			}
			cnt++;
		} while ((sBuf[0] & 1) != 0);

		if (sBuf[0] != 2) // not OK
		{
			if (sBuf[0] == 4) {
				pr_err("WRITE ERR\n");
				ret = MFC_FWUP_ERR_MTP_WRITE_ERR;
			} else if (sBuf[0] == 8) {
				pr_err("CHECKSUM ERR\n");
				ret = MFC_FWUP_ERR_PKT_CHECKSUM_ERR;
			} else {
				pr_err("UNKNOWN ERR\n");
				ret = MFC_FWUP_ERR_UNKNOWN_ERR;
			}
			pr_err("ERROR: buffer write to OTP returned status %d in sector 0x%x", sBuf[0], i);
			return ret;
		}
	}

	if (charger->fw_cmd == SEC_WIRELESS_RX_SDCARD_MODE ||
		charger->fw_cmd == SEC_WIRELESS_RX_SPU_MODE) {
		fw_ver = ((fw_minor[1] << 24) | (fw_minor[0] << 16) |
			(fw_major[1] << 8) | (fw_major[0] << 0));
		pr_info("%s: write current f/w rev (0x%x) in sdcard\n", __func__, fw_ver);
		if (!WriteWordToMtp(charger, MFC_FW_BIN_VERSION_ADDR, fw_ver)) {
			pr_err("ERROR: on writing FW rev to MTP\n");
			return MFC_FWUP_ERR_WRITING_FW_VERION;
		}
	} else {
		pr_info("%s: write current f/w rev (0x%x) in binary\n", __func__, MFC_FW_BIN_FULL_VERSION);
		if (!WriteWordToMtp(charger, MFC_FW_BIN_VERSION_ADDR, MFC_FW_BIN_FULL_VERSION)) {
			pr_err("ERROR: on writing FW rev to MTP\n");
			return MFC_FWUP_ERR_WRITING_FW_VERION;
		}
	}

	// run repair utility if repair is enabled
	if (repairEn) {
		ret = RepairMTPwRAM_IDT(charger);
		if (ret != MFC_FWUP_ERR_SUCCEEDED) {
			pr_err("ERROR: on repairing MTP\n");
			return ret;
		}
	}

	if (repairEn)
		pr_err("MTP Programming with repair finished in");
	else
		pr_err("MTP Programming finished in");

	pr_info("%s-------------------------------------------------\n", __func__);
	return MFC_FWUP_ERR_SUCCEEDED;
}

/* SKB : need to check. so strange. */
static void mfc_reset_rx_power(struct mfc_charger_data *charger, u8 rx_power)
{
#if defined(CONFIG_SEC_FACTORY)
	if (delayed_work_pending(&charger->wpc_rx_power_work))
		cancel_delayed_work(&charger->wpc_rx_power_work);
#endif

	if (charger->adt_transfer_status == WIRELESS_AUTH_PASS) {
		union power_supply_propval value = {0, };

		switch (rx_power) {
		case TX_RX_POWER_7_5W:
			value.intval = SEC_WIRELESS_RX_POWER_7_5W;
			break;
		case TX_RX_POWER_12W:
			value.intval = SEC_WIRELESS_RX_POWER_12W;
			break;
		case TX_RX_POWER_15W:
			value.intval = SEC_WIRELESS_RX_POWER_15W;
			break;
		case TX_RX_POWER_17_5W:
			value.intval = SEC_WIRELESS_RX_POWER_17_5W;
			break;
		case TX_RX_POWER_20W:
			value.intval = SEC_WIRELESS_RX_POWER_20W;
			break;
		default:
			value.intval = SEC_WIRELESS_RX_POWER_12W;
			pr_info("%s %s: undefine rx power reset\n", WC_AUTH_MSG, __func__);
			break;
		}
		psy_do_property("wireless", set,
			POWER_SUPPLY_EXT_PROP_WIRELESS_RX_POWER, value);
	} else {
		pr_info("%s %s: undefine rx power scenario, It is auth failed case how dose it get rx power?\n",
				WC_AUTH_MSG, __func__);
	}
}

static void mfc_wpc_rx_power_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_rx_power_work.work);
	union power_supply_propval value;

	pr_info("%s: rx power = %d, This W/A is only for Factory\n", __func__, charger->max_power_by_txid);

	value.intval = charger->max_power_by_txid;
	psy_do_property("wireless", set, POWER_SUPPLY_EXT_PROP_WIRELESS_RX_POWER, value);
}

static void mfc_wpc_afc_vout_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_afc_vout_work.work);
	union power_supply_propval value = {0, };

	pr_info("%s: start, current cable(%d)\n", __func__, charger->pdata->cable_type);

	/* change cable type */
	if (charger->pdata->cable_type == SEC_WIRELESS_PAD_PREPARE_HV) {
		if (charger->is_abnormal_pad) {
			pr_info("%s: skip voltgate set to pad, abnormal 3rd party pad\n", __func__);
			charger->pdata->cable_type = value.intval = SEC_WIRELESS_PAD_WPC;
		} else {
			charger->pdata->cable_type = value.intval = SEC_WIRELESS_PAD_WPC_HV;
		}
	} else {
		value.intval = charger->pdata->cable_type;
	}

	psy_do_property("wireless", set, POWER_SUPPLY_PROP_ONLINE, value);

	if (charger->is_abnormal_pad) {
		charger->pad_vout = PAD_VOUT_5V;
		goto skip_set_afc_vout;
	}

	/* check high temperature battery protection mode, value.intval == battery->thermal_zone */
	psy_do_property("battery", get, POWER_SUPPLY_EXT_PROP_THERMAL_ZONE, value);

	if (value.intval > BAT_THERMAL_NORMAL) {
		pr_info("%s: skip voltage set to pad, thermal_zone is WARM or HOT(%d)\n", __func__, value.intval);
		goto skip_set_afc_vout;
	}

	/* check is_otg_on */
	if (charger->is_otg_on) {
		pr_info("%s: skip voltage set to pad, is_otg_on(%d)\n", __func__, charger->is_otg_on);
		goto skip_set_afc_vout;
	}

	/* check is_full_status, tx_id */
	if (charger->is_full_status &&
			(charger->tx_id != TX_ID_DREAM_STAND && charger->tx_id != TX_ID_DREAM_DOWN)) {
		pr_info("%s: skip voltage set to pad, is_full_status(%d), tx_id(%d)\n",
				__func__, charger->is_full_status, charger->tx_id);
		goto skip_set_afc_vout;
	}

	// need to fix here to get vout setting
	if (charger->pdata->cable_type == SEC_WIRELESS_PAD_WPC_HV_20)
		mfc_send_command(charger, charger->vrect_by_txid);
	else
		mfc_send_command(charger, MFC_AFC_CONF_10V_TX);

	charger->is_afc_tx = true;
	pr_info("%s: is_afc_tx = %d vout read = %d\n",
		__func__, charger->is_afc_tx, mfc_get_adc(charger, MFC_ADC_VOUT));

	if (!charger->is_full_status &&
		charger->vout_mode != WIRELESS_VOUT_5V &&
		charger->vout_mode != WIRELESS_VOUT_5V_STEP &&
		charger->vout_mode != WIRELESS_VOUT_5_5V_STEP) {
		// need to fix here
		if (charger->pdata->cable_type == SEC_WIRELESS_PAD_WPC_HV_20)
			charger->vout_mode = charger->vout_by_txid;
		else
			charger->vout_mode = WIRELESS_VOUT_10V;

		cancel_delayed_work(&charger->wpc_vout_mode_work);
		__pm_stay_awake(charger->wpc_vout_mode_ws);
		queue_delayed_work(charger->wqueue, &charger->wpc_vout_mode_work, 0);
	}

skip_set_afc_vout:
	__pm_relax(charger->wpc_afc_vout_ws);
}

static void mfc_wpc_fw_update_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_fw_update_work.work);
	union power_supply_propval value = {0, };
	int ret = 0;
	int i = 0;
	bool is_changed = false;
	char fwtime[8] = {32, 32, 32, 32, 32, 32, 32, 32};
	char fwdate[8] = {32, 32, 32, 32, 32, 32, 32, 32};
	u8 data = 32; /* ascii space */
	int bin_fw_ver;
	const char *fw_path = MFC_FLASH_FW_HEX_PATH;

	pr_info("%s: firmware update mode is = %d\n", __func__, charger->fw_cmd);

	if (gpio_get_value(charger->pdata->wpc_en)) {
		pr_info("%s: wpc_en disabled\n", __func__);
		goto end_of_fw_work;
	}
	mfc_set_wpc_en(charger, WPC_EN_FW, true); /* keep the wpc_en low during fw update */

	switch (charger->fw_cmd) {
	case SEC_WIRELESS_RX_SPU_MODE:
	case SEC_WIRELESS_RX_SPU_VERIFY_MODE:
	case SEC_WIRELESS_RX_SDCARD_MODE:
	case SEC_WIRELESS_RX_INIT:
	case SEC_WIRELESS_RX_BUILT_IN_MODE:
		pgmCnt = 0;
		repairEn = false;
		mfc_uno_on(charger, true);
		msleep(200);
		if (mfc_get_chip_id(charger) < 0 ||
			charger->chip_id != MFC_CHIP_IDT) {
			pr_info("%s: current IC's chip_id(0x%x) is not matching to driver's chip_id(0x%x)\n",
				__func__, charger->chip_id, MFC_CHIP_IDT);
			break;
		}
		if (charger->fw_cmd == SEC_WIRELESS_RX_INIT) {
			charger->pdata->otp_firmware_ver = mfc_get_firmware_version(charger, MFC_RX_FIRMWARE);
#if defined(CONFIG_WIRELESS_IC_PARAM)
			if (charger->wireless_fw_mode_param == SEC_WIRELESS_RX_SPU_MODE &&
				charger->pdata->otp_firmware_ver > MFC_FW_BIN_VERSION) {
				pr_info("%s: current version (0x%x) is higher than BIN_VERSION by SPU(0x%x)\n",
					__func__, charger->pdata->otp_firmware_ver, MFC_FW_BIN_VERSION);
				break;
			}
#endif
			if (charger->pdata->otp_firmware_ver == MFC_FW_BIN_VERSION) {
				pr_info("%s: current version (0x%x) is same to BIN_VERSION (0x%x)\n",
					__func__, charger->pdata->otp_firmware_ver, MFC_FW_BIN_VERSION);
				break;
			}
		}
		charger->pdata->otp_firmware_result = MFC_FWUP_ERR_RUNNING;
		is_changed = true;

		pr_info("%s: request_firmware\n", __func__);

		if (charger->fw_cmd == SEC_WIRELESS_RX_SPU_MODE ||
			charger->fw_cmd == SEC_WIRELESS_RX_SPU_VERIFY_MODE)
			fw_path = MFC_FW_SPU_BIN_PATH;
		else if (charger->fw_cmd == SEC_WIRELESS_RX_SDCARD_MODE)
			fw_path = MFC_FW_SDCARD_BIN_PATH;
		else
			fw_path = MFC_FLASH_FW_HEX_PATH;

		ret = request_firmware(&charger->firm_data_bin, fw_path, &charger->client->dev);
		if (ret < 0) {
			pr_err("%s: failed to request firmware %s(%d)\n", __func__, fw_path, ret);
			charger->pdata->otp_firmware_result = MFC_FWUP_ERR_REQUEST_FW_BIN;
			goto fw_err;
		}

#if IS_ENABLED(CONFIG_SPU_VERIFY)
		if (charger->fw_cmd == SEC_WIRELESS_RX_SPU_MODE ||
			charger->fw_cmd == SEC_WIRELESS_RX_SPU_VERIFY_MODE) {
			if (spu_firmware_signature_verify("MFC", charger->firm_data_bin->data, charger->firm_data_bin->size) ==
				((int)charger->firm_data_bin->size - SPU_METADATA_SIZE(MFC))) {
				pr_err("%s: spu_firmware_signature_verify success\n", __func__);
				if (charger->fw_cmd == SEC_WIRELESS_RX_SPU_VERIFY_MODE) {
					charger->pdata->otp_firmware_result = MFC_FW_RESULT_PASS;
					goto fw_err;
				}
			} else {
				pr_err("%s: spu_firmware_signature_verify failed\n", __func__);
				goto fw_err;
			}
		}
#endif

		if ((charger->fw_cmd == SEC_WIRELESS_RX_INIT)
			|| (charger->fw_cmd == SEC_WIRELESS_RX_BUILT_IN_MODE)) {
			bin_fw_ver = fw_ver_in_bin(charger->firm_data_bin->data, MFC_FW_VER_OFFSET);
			if (bin_fw_ver != MFC_FW_BIN_VERSION) {
				pr_err("%s: failed to mismatch fw. bin_fw_ver(0x%x) , defined_fw_ver(0x%x)\n",
					__func__, bin_fw_ver, MFC_FW_BIN_VERSION);
				charger->pdata->otp_firmware_result = MFC_FWUP_ERR_DATA_NOT_MATCH;
				goto fw_err;
			}
		}

		disable_irq(charger->pdata->irq_wpc_int);
		disable_irq(charger->pdata->irq_wpc_det);
		__pm_stay_awake(charger->wpc_update_ws);
		pr_info("%s: data size = %ld, chip_id(%d)\n", __func__, charger->firm_data_bin->size, charger->chip_id);

		do {
			if (repairEn == true) {
				if (charger->pdata->wpc_en >= 0)
					gpio_direction_output(charger->pdata->wpc_en, 1);
				mfc_uno_on(charger, false);
				msleep(1000);
				mfc_uno_on(charger, true);
				if (charger->pdata->wpc_en >= 0)
					gpio_direction_output(charger->pdata->wpc_en, 0);
				msleep(300);
			}
			ret = PgmOTPwRAM_IDT(charger, 0, charger->firm_data_bin->data,
						0, charger->firm_data_bin->size, repairEn);
			if ((charger->fw_cmd == SEC_WIRELESS_RX_INIT
				|| charger->fw_cmd == SEC_WIRELESS_RX_BUILT_IN_MODE)
				&& (ret == MFC_FWUP_ERR_SUCCEEDED)) { // pass, let's begin verification
				ret = VerifyMTPwRAM_IDT(charger, MTP_VERIFY_ADDR,
				MTP_VERIFY_SIZE, MTP_VERIFY_CHKSUM);
			}
			// if download fails, enable repair
			//repairEn = !ret;
			if (ret != MFC_FWUP_ERR_SUCCEEDED)
				repairEn = true;
			else
				repairEn = false;

			pgmCnt++;

			pr_info("%s %s: repairEn(%d), pgmCnt(%d), ret(%d)\n",
					MFC_FW_MSG, __func__, repairEn, pgmCnt, ret);
		} while ((ret != MFC_FWUP_ERR_SUCCEEDED) && (pgmCnt < MAX_MTP_PGM_CNT));

		release_firmware(charger->firm_data_bin);

		for (i = 0; i < 8; i++) {
			if (mfc_reg_read(charger->client, MFC_FW_DATA_CODE_0+i, &data) > 0)
				fwdate[i] = (char)data;
		}
		for (i = 0; i < 8; i++) {
			if (mfc_reg_read(charger->client, MFC_FW_TIMER_CODE_0+i, &data) > 0)
				fwtime[i] = (char)data;
		}
		pr_info("%s: %d%d%d%d%d%d%d%d, %d%d%d%d%d%d%d%d\n", __func__,
			fwdate[0], fwdate[1], fwdate[2], fwdate[3], fwdate[4], fwdate[5], fwdate[6], fwdate[7],
			fwtime[0], fwtime[1], fwtime[2], fwtime[3], fwtime[4], fwtime[5], fwtime[6], fwtime[7]);

		charger->pdata->otp_firmware_ver = mfc_get_firmware_version(charger, MFC_RX_FIRMWARE);
		charger->pdata->wc_ic_rev = mfc_get_ic_revision(charger, MFC_IC_REVISION);

		for (i = 0; i < 8; i++) {
			if (mfc_reg_read(charger->client, MFC_FW_DATA_CODE_0+i, &data) > 0)
				fwdate[i] = (char)data;
		}
		for (i = 0; i < 8; i++) {
			if (mfc_reg_read(charger->client, MFC_FW_TIMER_CODE_0+i, &data) > 0)
				fwtime[i] = (char)data;
		}
		pr_info("%s: %d%d%d%d%d%d%d%d, %d%d%d%d%d%d%d%d\n", __func__,
			fwdate[0], fwdate[1], fwdate[2], fwdate[3], fwdate[4], fwdate[5], fwdate[6], fwdate[7],
			fwtime[0], fwtime[1], fwtime[2], fwtime[3], fwtime[4], fwtime[5], fwtime[6], fwtime[7]);

		enable_irq(charger->pdata->irq_wpc_int);
		enable_irq(charger->pdata->irq_wpc_det);
		__pm_relax(charger->wpc_update_ws);
		break;
	case SEC_WIRELESS_TX_ON_MODE:
		charger->pdata->cable_type = SEC_WIRELESS_PAD_TX;
		break;
	case SEC_WIRELESS_TX_OFF_MODE:
		/* need to check */
		break;
	default:
		break;
	}

	msleep(200);
	mfc_uno_on(charger, false);
	pr_info("%s---------------------------------------------------------------\n", __func__);

	if (is_changed) {
		if (ret == MFC_FWUP_ERR_SUCCEEDED) {
			charger->pdata->otp_firmware_result = MFC_FW_RESULT_PASS;
#if defined(CONFIG_WIRELESS_IC_PARAM)
			charger->wireless_fw_mode_param = charger->fw_cmd & 0xF;
			pr_info("%s: succeed. fw_mode(0x%01X)\n",
				__func__, charger->wireless_fw_mode_param);
			charger->wireless_param_info &= 0xFFFFFF0F;
			charger->wireless_param_info |= (charger->wireless_fw_mode_param & 0xF) << 4;
			pr_info("%s: wireless_param_info (0x%08X)\n", __func__, charger->wireless_param_info);
#endif
		} else {
			charger->pdata->otp_firmware_result = ret;
		}
	}
	value.intval = false;
	psy_do_property("battery", set, POWER_SUPPLY_EXT_PROP_MFC_FW_UPDATE, value);

	mfc_set_wpc_en(charger, WPC_EN_FW, false);

	return;
fw_err:
	mfc_uno_on(charger, false);
	mfc_set_wpc_en(charger, WPC_EN_FW, false);
end_of_fw_work:
	value.intval = false;
	psy_do_property("battery", set, POWER_SUPPLY_EXT_PROP_MFC_FW_UPDATE, value);
}

/*#if !defined(CONFIG_SEC_FACTORY)
static void mfc_wpc_fw_booting_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_fw_booting_work.work);
	union power_supply_propval value = {0, };
	int fw_version;

	value.intval =
		SEC_FUELGAUGE_CAPACITY_TYPE_SCALE;
	psy_do_property(charger->pdata->fuelgauge_name, get,
		POWER_SUPPLY_PROP_CAPACITY, value);
	pr_info("%s: battery capacity (%d)\n", __func__, value.intval);

	if (value.intval >= 10) {
		mfc_uno_on(charger, true);
		msleep(200);
		fw_version = mfc_get_firmware_version(charger, MFC_RX_FIRMWARE);
		pr_info("%s: fw version (0x%x)\n", __func__, fw_version);
		if (fw_version != MFC_FW_BIN_VERSION) {
			charger->fw_cmd = SEC_WIRELESS_RX_BUILT_IN_MODE;
			queue_delayed_work(charger->wqueue, &charger->wpc_fw_update_work, 0);
		} else {
			mfc_uno_on(charger, false);
		}
	}
}
#endif*/

static int mfc_chg_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct mfc_charger_data *charger = power_supply_get_drvdata(psy);
	enum power_supply_ext_property ext_psp = (enum power_supply_ext_property) psp;
//	union power_supply_propval value;

	switch ((int)psp) {
	case POWER_SUPPLY_PROP_STATUS:
		pr_info("%s: charger->pdata->cs100_status %d\n", __func__, charger->pdata->cs100_status);
		val->intval = charger->pdata->cs100_status;
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
	case POWER_SUPPLY_PROP_HEALTH:
		return -ENODATA;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		if (charger->pdata->is_charging)
			val->intval = mfc_get_vout(charger);
		else
			val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		return -ENODATA;
	case POWER_SUPPLY_PROP_ONLINE:
		pr_info("%s: cable_type =%d\n", __func__, charger->pdata->cable_type);
		val->intval = charger->pdata->cable_type;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		pr_info("%s: POWER_SUPPLY_PROP_MANUFACTURER, intval(0x%x), called by(%ps)\n",
				__func__, val->intval, __builtin_return_address(0));
		if (val->intval == SEC_WIRELESS_OTP_FIRM_RESULT) {
			pr_info("%s: otp firmware result = %d\n", __func__, charger->pdata->otp_firmware_result);
			val->intval = charger->pdata->otp_firmware_result;
		} else if (val->intval == SEC_WIRELESS_IC_REVISION) {
			pr_info("%s: check ic revision\n", __func__);
			val->intval = mfc_get_ic_revision(charger, MFC_IC_REVISION);
		} else if (val->intval == SEC_WIRELESS_IC_CHIP_ID) {
			pr_info("%s: check ic chip_id(0x%02X)\n", __func__, charger->chip_id);
			val->intval = charger->chip_id;
		} else if (val->intval == SEC_WIRELESS_OTP_FIRM_VER_BIN) {
			/* update latest kernl f/w version */
			val->intval = MFC_FW_BIN_VERSION;
		} else if (val->intval == SEC_WIRELESS_OTP_FIRM_VER) {
			val->intval = mfc_get_firmware_version(charger, MFC_RX_FIRMWARE);
			pr_info("%s: check f/w revision (0x%x)\n", __func__, val->intval);
			if (val->intval < 0 && charger->pdata->otp_firmware_ver > 0)
				val->intval = charger->pdata->otp_firmware_ver;
		} else if (val->intval == SEC_WIRELESS_TX_FIRM_RESULT) {
			val->intval = charger->pdata->tx_firmware_result;
		} else if (val->intval == SEC_WIRELESS_TX_FIRM_VER) {
			val->intval = charger->pdata->tx_firmware_ver;
		} else if (val->intval == SEC_TX_FIRMWARE) {
			val->intval = charger->tx_status;
		} else if (val->intval == SEC_WIRELESS_OTP_FIRM_VERIFY) {
			pr_info("%s: IDT FIRM_VERIFY\n", __func__);
			msleep(10);
			val->intval = mfc_firmware_verify(charger);
		} else {
			val->intval = -ENODATA;
			pr_err("%s: wrong mode\n", __func__);
		}
		break;
	case POWER_SUPPLY_PROP_ENERGY_NOW: /* vout */
		if (charger->pdata->is_charging) {
			val->intval = mfc_get_adc(charger, MFC_ADC_VOUT);
			pr_info("%s: wc vout (%d)\n", __func__, val->intval);
		} else {
			val->intval = 0;
		}
		break;
	case POWER_SUPPLY_PROP_ENERGY_AVG: /* vrect */
		if (charger->pdata->is_charging)
			val->intval = mfc_get_adc(charger, MFC_ADC_VRECT);
		else
			val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
		val->intval = charger->vrect_by_txid;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = mfc_get_adc(charger, val->intval);
		break;
	case POWER_SUPPLY_PROP_CAPACITY_ALERT_MIN:
		break;
	case POWER_SUPPLY_EXT_PROP_MIN ... POWER_SUPPLY_EXT_PROP_MAX:
		switch (ext_psp) {
		case POWER_SUPPLY_EXT_PROP_WIRELESS_OP_FREQ:
			val->intval = mfc_get_adc(charger, MFC_ADC_OP_FRQ);
			pr_info("%s: Operating FQ %dkHz\n", __func__, val->intval);
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_TRX_CMD:
			val->intval = charger->pdata->trx_data_cmd;
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_TRX_VAL:
			val->intval = charger->pdata->trx_data_val;
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_TX_ID:
			val->intval = charger->tx_id;
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_TX_ID_CNT:
			val->intval = charger->tx_id_cnt;
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_RX_CONNECTED:
			val->intval = charger->wc_rx_connected;
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_RX_TYPE:
			val->intval = charger->wc_rx_type;
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_TX_UNO_VIN:
			val->intval = mfc_get_adc(charger, MFC_ADC_TX_VOUT);
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_TX_UNO_IIN:
			val->intval = mfc_get_adc(charger, MFC_ADC_TX_IOUT);
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_RX_POWER:
			val->intval = charger->current_rx_power;
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_AUTH_ADT_STATUS:
			val->intval = charger->adt_transfer_status;
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_AUTH_ADT_DATA:
			{
				//int i = 0;
				//u8 *p_data;

				if (charger->adt_transfer_status == WIRELESS_AUTH_RECEIVED) {
					pr_info("%s %s: MFC_ADT_RECEIVED (%d)\n",
							WC_AUTH_MSG, __func__, charger->adt_transfer_status);
					val->strval = (u8 *)ADT_buffer_rdata;
					//p_data = ADT_buffer_rdata;
					//for (i = 0; i < adt_readSize; i++)
					//	pr_info("%s: auth read data = %x", __func__, p_data[i]);
					//pr_info("\n", __func__);
				} else {
					pr_info("%s: data hasn't been received yet\n", __func__);
					return -ENODATA;
				}
			}
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_AUTH_ADT_SIZE:
			val->intval = adt_readSize;
			pr_info("%s %s: MFC_ADT_RECEIVED (%d), DATA SIZE(%d)\n",
					WC_AUTH_MSG, __func__, charger->adt_transfer_status, val->intval);
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_RX_VOUT:
			val->intval = charger->vout_mode;
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_INITIAL_WC_CHECK:
			val->intval = charger->initial_wc_check;
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_CHECK_FW_VER:
#if defined(CONFIG_WIRELESS_IC_PARAM)
			pr_info("%s: fw_ver (param:0x%04X, build:0x%04X)\n",
				__func__, charger->wireless_fw_ver_param, MFC_FW_BIN_VERSION);
			if (charger->wireless_fw_ver_param == MFC_FW_BIN_VERSION)
				val->intval = 1;
			else
				val->intval = 0;
#else
			val->intval = 0;
#endif
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_MST_PWR_EN:
			if (gpio_is_valid(charger->pdata->mst_pwr_en)) {
				val->intval = gpio_get_value(charger->pdata->mst_pwr_en);
			} else {
				pr_info("%s: invalid gpio(mst_pwr_en)\n", __func__);
				val->intval = 0;
			}
			break;
		case POWER_SUPPLY_EXT_PROP_PAD_VOLT_CTRL:
			val->intval = charger->is_afc_tx;
			break;
		case POWER_SUPPLY_EXT_PROP_WPC_EN:
			val->intval = gpio_get_value(charger->pdata->wpc_en);
			break;
		case POWER_SUPPLY_EXT_PROP_MONITOR_WORK:
			if (gpio_get_value(charger->pdata->wpc_en))
				pr_info("@DIS_MFC %s: charger->wpc_en_flag(0x%x)\n", __func__, charger->wpc_en_flag);
			break;
#if defined(CONFIG_TX_GEAR_PHM_VOUT_CTRL) || defined(CONFIG_TX_GEAR_AOV)
		case POWER_SUPPLY_EXT_PROP_GEAR_PHM_EVENT:
			val->intval = charger->tx_gear_phm;
			break;
#endif
		case POWER_SUPPLY_EXT_PROP_CHARGE_OTG_CONTROL:
		case POWER_SUPPLY_EXT_PROP_CHARGE_POWERED_OTG_CONTROL:
			return -ENODATA;
		case POWER_SUPPLY_EXT_PROP_INPUT_VOLTAGE_REGULATION:
			val->intval = charger->pdata->vout_status;
			break;
#if defined(CONFIG_WIRELESS_IC_PARAM)
		case POWER_SUPPLY_EXT_PROP_WIRELESS_PARAM_INFO:
			val->intval = charger->wireless_param_info;
			break;
#endif
		default:
			return -ENODATA;
		}
		break;
	default:
		return -ENODATA;
	}
	return 0;
}

static void mfc_wpc_vout_mode_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_vout_mode_work.work);
	int vout_step = charger->pdata->vout_status;
	int vout = MFC_VOUT_10V;
	union power_supply_propval value;

	if (is_shutdn) {
		pr_err("%s: Escape by shtudown\n", __func__);
		return;
	}
	pr_info("%s: start - vout_mode(%s), vout_status(%s)\n",
		__func__, mfc_vout_control_mode_str[charger->vout_mode], rx_vout_str[charger->pdata->vout_status]);
	switch (charger->vout_mode) {
	case WIRELESS_VOUT_5V:
		mfc_set_vout(charger, MFC_VOUT_5V);
		break;
	case WIRELESS_VOUT_9V:
		mfc_set_vout(charger, MFC_VOUT_9V);
		break;
	case WIRELESS_VOUT_10V:
		mfc_set_vout(charger, MFC_VOUT_10V);
		/* reset AICL */
		psy_do_property("wireless", set, POWER_SUPPLY_PROP_CURRENT_MAX, value);
		break;
	case WIRELESS_VOUT_11V:
		mfc_set_vout(charger, MFC_VOUT_11V);
		/* reset AICL */
		psy_do_property("wireless", set, POWER_SUPPLY_PROP_CURRENT_MAX, value);
		break;
	case WIRELESS_VOUT_12V:
		mfc_set_vout(charger, MFC_VOUT_12V);
		/* reset AICL */
		psy_do_property("wireless", set, POWER_SUPPLY_PROP_CURRENT_MAX, value);
		break;
	case WIRELESS_VOUT_12_5V:
		mfc_set_vout(charger, MFC_VOUT_12_5V);
		/* reset AICL */
		psy_do_property("wireless", set, POWER_SUPPLY_PROP_CURRENT_MAX, value);
		break;
	case WIRELESS_VOUT_5V_STEP:
		vout_step--;
		if (vout_step >= MFC_VOUT_5V) {
			mfc_set_vout(charger, vout_step);
			cancel_delayed_work(&charger->wpc_vout_mode_work);
			queue_delayed_work(charger->wqueue, &charger->wpc_vout_mode_work, msecs_to_jiffies(250));
			return;
		}
		break;
	case WIRELESS_VOUT_5_5V_STEP:
		vout_step--;
		if (vout_step >= MFC_VOUT_5_5V) {
			if (charger->pdata->wpc_vout_ctrl_lcd_on) {
				psy_do_property("battery", get,	POWER_SUPPLY_EXT_PROP_PAD_VOLT_CTRL, value);
				if (value.intval && charger->is_afc_tx &&
					(charger->tx_id != TX_ID_UNKNOWN) &&
					(charger->tx_id != TX_ID_DREAM_DOWN &&
					charger->tx_id != TX_ID_DREAM_STAND)) {
					if (vout_step == charger->flicker_vout_threshold) {
						mfc_set_vout(charger, vout_step);
						cancel_delayed_work(&charger->wpc_vout_mode_work);
						queue_delayed_work(charger->wqueue,
							&charger->wpc_vout_mode_work, msecs_to_jiffies(charger->flicker_delay));
						return;
					} else if (vout_step < charger->flicker_vout_threshold) {
						mfc_send_command(charger, MFC_AFC_CONF_5V_TX);
						pr_info("%s: set TX 5V because LCD ON\n", __func__);
						charger->is_afc_tx = false;
						charger->pad_ctrl_by_lcd = true;
					}
				}
			}
			mfc_set_vout(charger, vout_step);
			cancel_delayed_work(&charger->wpc_vout_mode_work);
			queue_delayed_work(charger->wqueue,
				&charger->wpc_vout_mode_work, msecs_to_jiffies(250));
			return;
		}
		break;
	case WIRELESS_VOUT_9V_STEP:
		vout = MFC_VOUT_9V;
	case WIRELESS_VOUT_10V_STEP:
		vout_step++;
		if (vout_step <= vout) {
			mfc_set_vout(charger, vout_step);
			cancel_delayed_work(&charger->wpc_vout_mode_work);
			queue_delayed_work(charger->wqueue,
				&charger->wpc_vout_mode_work, msecs_to_jiffies(250));
			return;
		}
		break;
	case WIRELESS_VOUT_CC_CV_VOUT:
		mfc_set_vout(charger, MFC_VOUT_5_5V);
		break;
	default:
		break;
	}
#if !defined(CONFIG_SEC_FACTORY)
	if ((is_hv_wireless_pad_type(charger->pdata->cable_type)) &&
			(charger->pdata->vout_status <= MFC_VOUT_5_5V &&
			(charger->is_full_status || charger->sleep_mode))) {
		mfc_send_command(charger, MFC_AFC_CONF_5V_TX);
		pr_info("%s: set TX 5V after cs100\n", __func__);
		charger->is_afc_tx = false;
	}
#endif
	pr_info("%s: finish - vout_mode(%s), vout_status(%s)\n",
		__func__, mfc_vout_control_mode_str[charger->vout_mode], rx_vout_str[charger->pdata->vout_status]);
	__pm_relax(charger->wpc_vout_mode_ws);
}

static void mfc_wpc_i2c_error_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_i2c_error_work.work);

	if (charger->wc_w_state &&
		gpio_get_value(charger->pdata->wpc_det)) {
		union power_supply_propval value;

		psy_do_property("battery", set,
			POWER_SUPPLY_EXT_PROP_WC_CONTROL, value);
	}
}

/* SKB : Need to check */
static void mfc_set_tx_fod_with_gear(struct mfc_charger_data *charger)
{
	struct timespec ts = {0, };
	u8 data[4] = {0, };

	pr_info("%s: FOD1 %dmA, FOD2 %dmA\n", __func__, charger->pdata->gear_fod1, charger->pdata->gear_fod2);

	data[0] = charger->pdata->gear_fod1 & 0xFF;
	data[1] = (charger->pdata->gear_fod1 & 0xFF00) >> 8;
	data[2] = charger->pdata->gear_fod2 & 0xFF;
	data[3] = (charger->pdata->gear_fod2 & 0xFF00) >> 8;

	mfc_reg_write(charger->client, MFC_TX_OC_FOD1_LIMIT_L_REG, data[0]);
	mfc_reg_write(charger->client, MFC_TX_OC_FOD1_LIMIT_H_REG, data[1]);
	mfc_reg_write(charger->client, MFC_TX_OC_FOD2_LIMIT_L_REG, data[2]);
	mfc_reg_write(charger->client, MFC_TX_OC_FOD2_LIMIT_H_REG, data[3]);

	mfc_reg_read(charger->client, MFC_TX_OC_FOD1_LIMIT_L_REG, &data[0]);
	mfc_reg_read(charger->client, MFC_TX_OC_FOD1_LIMIT_H_REG, &data[1]);
	mfc_reg_read(charger->client, MFC_TX_OC_FOD2_LIMIT_L_REG, &data[2]);
	mfc_reg_read(charger->client, MFC_TX_OC_FOD2_LIMIT_H_REG, &data[3]);
	pr_info("%s: FOD1 (0x%x 0x%x) FOD2 (0x%x 0x%x)\n", __func__, data[0], data[1], data[2], data[3]);

	if (charger->gear_start_time == 0) {
		ts = ktime_to_timespec(ktime_get_boottime());
		charger->gear_start_time = ts.tv_sec;
	}
}

static void mfc_set_tx_ping_freq_with_gear(struct mfc_charger_data *charger)
{
	pr_info("@Tx_Mode %s\n", __func__);

	mfc_reg_write(charger->client, MFC_TX_PING_FREQ_L_REG, charger->pdata->gear_ping_freq);
}

static void mfc_set_tx_fod_threshold_with_phone(struct mfc_charger_data *charger)
{
	pr_info("@Tx_Mode %s\n", __func__);

	mfc_reg_write(charger->client, MFC_WPC_FOD_THRESHOLD_REG, charger->pdata->phone_fod_threshold);
}

static void mfc_wpc_rx_type_det_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_rx_type_det_work.work);
	u8 reg_data;
	u8 data[2] = {0,};
	int ret = 0, prmc_id = 0;
	union power_supply_propval value;

	if (!charger->wc_tx_enable) {
		__pm_relax(charger->wpc_rx_det_ws);
		return;
	}

	mfc_reg_read(charger->client, MFC_STARTUP_EPT_COUNTER, &reg_data);

	ret = mfc_reg_read(charger->client, MFC_PRMC_ID_L_REG, &data[0]);
	ret = mfc_reg_read(charger->client, MFC_PRMC_ID_H_REG, &data[1]);

	prmc_id = data[0] | (data[1] << 8);

	pr_info("@Tx_Mode %s: prmc_id 0x%x\n", __func__, prmc_id);

	if (prmc_id == SS_ID && reg_data >= 1) {
		pr_info("@Tx_Mode %s: Samsung Gear Connected\n", __func__);
		charger->wc_rx_type = SS_GEAR;
		mfc_set_tx_fod_with_gear(charger);
		mfc_set_tx_ping_freq_with_gear(charger);

		if (charger->pdata->gear_min_op_freq_delay > 0) {
			mfc_set_tx_min_op_freq(charger, charger->pdata->gear_min_op_freq);
			cancel_delayed_work(&charger->wpc_tx_min_op_freq_work);
			__pm_stay_awake(charger->wpc_tx_min_opfq_ws);
			queue_delayed_work(charger->wqueue, &charger->wpc_tx_min_op_freq_work,
				msecs_to_jiffies(charger->pdata->gear_min_op_freq_delay));
		}
	} else if (prmc_id == SS_ID) {
		pr_info("@Tx_Mode %s: Samsung Phone Connected\n", __func__);
		charger->wc_rx_type = SS_PHONE;
		mfc_set_tx_fod_threshold_with_phone(charger);
	} else {
		pr_info("@Tx_Mode %s: Unknown device connected\n", __func__);
		charger->wc_rx_type = OTHER_DEV;
		mfc_set_tx_fod_threshold_with_phone(charger);
	}
	value.intval = charger->wc_rx_type;
	psy_do_property("wireless", set, POWER_SUPPLY_EXT_PROP_WIRELESS_RX_TYPE, value);

	__pm_relax(charger->wpc_rx_det_ws);
}

static void mfc_tx_op_freq_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_tx_op_freq_work.work);

	/* recover tx op freq */
	mfc_set_min_duty(charger, MIN_DUTY_SETTING_20_DATA);
	pr_info("%s: tx op freq = %dKhz\n", __func__, mfc_get_adc(charger, MFC_ADC_TX_OP_FRQ));

	__pm_relax(charger->wpc_tx_opfq_ws);

}

static void mfc_tx_min_op_freq_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_tx_min_op_freq_work.work);

	mfc_set_tx_min_op_freq(charger, TX_MIN_OP_FREQ_DEFAULT);

	__pm_relax(charger->wpc_tx_min_opfq_ws);
}

static void mfc_check_tx_gear_time(struct mfc_charger_data *charger)
{
	struct timespec ts = {0, };
	unsigned long gear_charging_time;

	ts = ktime_to_timespec(ktime_get_boottime());

	if (ts.tv_sec >= charger->gear_start_time)
		gear_charging_time = ts.tv_sec - charger->gear_start_time;
	else
		gear_charging_time = 0xFFFFFFFF - charger->gear_start_time + ts.tv_sec;

	if ((gear_charging_time >= 90) && charger->gear_start_time) {
		pr_info("@Tx_Mode %s: set OC FOD1 %d mA for Gear, gear_charging_time(%ld)\n",
			__func__, charger->pdata->oc_fod1, gear_charging_time);
		mfc_set_tx_oc_fod(charger);

		charger->gear_start_time = 0;
	}
}

#if defined(CONFIG_TX_GEAR_AOV)
static void mfc_tx_aov_phm_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_tx_aov_phm_work.work);
	union power_supply_propval value = {0, };

	psy_do_property("wireless", get,
		POWER_SUPPLY_EXT_PROP_TX_SWITCH_MODE, value);

	if ((value.intval == TX_SWITCH_CHG_ONLY) && !charger->tx_gear_phm) {
		mfc_set_cmd_l_reg(charger, MFC_CMD_TOGGLE_PHM_MASK, MFC_CMD_TOGGLE_PHM_MASK);
		charger->tx_gear_phm = 1;

		value.intval = 0;
		psy_do_property("wireless", set,
			POWER_SUPPLY_EXT_PROP_TX_AOV_BUCK_CTRL, value);

		/* start aov phm alarm timer */
		pr_info("@Tx_Mode %s: Start AOV PHM alarm by %d seconds\n",
			__func__, charger->pdata->gear_aov_phm_alarm);
		alarm_start(&charger->phm_alarm, ktime_add(ktime_get_boottime(),
			ktime_set(charger->pdata->gear_aov_phm_alarm, 0)));
	}

	__pm_relax(charger->wpc_tx_aov_phm_ws);
}
#endif

static void mfc_tx_phm_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_tx_phm_work.work);

#if defined(CONFIG_TX_GEAR_PHM_VOUT_CTRL) || defined(CONFIG_TX_GEAR_AOV)
	union power_supply_propval value = {0, };
	u8 data = 0;
#endif

	/* noirq resume of devices complete after 200 msecs */
	if (charger->is_suspend)
		msleep(200);

	pr_info("@Tx_Mode %s\n", __func__);
#if !defined(CONFIG_TX_GEAR_PHM_VOUT_CTRL) && !defined(CONFIG_TX_GEAR_AOV)
	mfc_set_cmd_l_reg(charger, MFC_CMD_TOGGLE_PHM_MASK, MFC_CMD_TOGGLE_PHM_MASK);
#endif
#if defined(CONFIG_TX_GEAR_AOV)
	psy_do_property("wireless", get,
		POWER_SUPPLY_EXT_PROP_TX_SWITCH_MODE, value);

	if (value.intval == TX_SWITCH_CHG_ONLY) {
		value.intval = TX_SWITCH_UNO_FOR_GEAR;
		psy_do_property("wireless", set,
			POWER_SUPPLY_EXT_PROP_TX_SWITCH_MODE, value);
	} else {
#endif
#if defined(CONFIG_TX_GEAR_PHM_VOUT_CTRL) || defined(CONFIG_TX_GEAR_AOV)
		mfc_set_cmd_l_reg(charger, MFC_CMD_TOGGLE_PHM_MASK, MFC_CMD_TOGGLE_PHM_MASK);

		if (charger->tx_gear_phm) {
			charger->tx_gear_phm = 0;
			value.intval = 0;
			psy_do_property("wireless", set,
				POWER_SUPPLY_EXT_PROP_GEAR_PHM_EVENT, value);
		}

		msleep(400);
		mfc_reg_update(charger->client, MFC_INT_A_ENABLE_H_REG,
			MFC_STAT_H_TX_CON_DISCON_MASK, MFC_STAT_H_TX_CON_DISCON_MASK);
		mfc_reg_read(charger->client, MFC_INT_A_ENABLE_H_REG, &data);
		pr_info("@Tx_Mode %s MFC_INT_A_ENABLE_H_REG(0x25)= 0x%x\n", __func__, data);
		mfc_reg_read(charger->client, MFC_STATUS_H_REG, &data);
		pr_info("@Tx_Mode %s MFC_STATUS_H_REG(0x21)= 0x%x\n", __func__, data);
		/* determine rx connection status with tx sharing mode */
		if (!(data & MFC_STAT_H_TX_CON_DISCON_MASK)) {
			if (!charger->wc_rx_connected) {
				pr_info("@Tx_Mode %s Ignore IRQ!! already Rx disconnected!\n", __func__);
			} else {
				charger->wc_rx_connected = false;
				__pm_stay_awake(charger->wpc_rx_connection_ws);
				queue_delayed_work(charger->wqueue,
					&charger->wpc_rx_connection_work, 0);
			}
		}
#endif
#if defined(CONFIG_TX_GEAR_AOV)
	}
#endif
	__pm_relax(charger->wpc_tx_phm_ws);
}

static void mfc_cs100_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_cs100_work.work);

	pr_info("%s: full_status(%d) , tx_id(0x%x)\n",
		__func__, charger->is_full_status, charger->tx_id);
	/* set fake FOD values before send cs100, need to tune */
	if (is_wireless_pad_type(charger->pdata->cable_type))
		mfc_fod_set(charger, FOD_STATE_FULL);

	charger->pdata->cs100_status = mfc_send_cs100(charger);

#if !defined(CONFIG_SEC_FACTORY)
	if (!charger->is_full_status) {
		charger->is_full_status = 1;
		/* 1st chg done ~ 2nd chg done : CMA+CMB (samsung pad only) */
		if (charger->tx_id) {
			mfc_reg_write(charger->client, MFC_RX_COMM_MOD_AFC_FET_REG, CMA_ENABLE_CMB_ENABLE_DATA);
			mfc_reg_write(charger->client, MFC_RX_COMM_MOD_FET_REG, CMA_ENABLE_CMB_ENABLE_DATA);
		}

		if (is_hv_wireless_pad_type(charger->pdata->cable_type) &&
			(charger->tx_id != TX_ID_DREAM_STAND &&
			charger->tx_id != TX_ID_DREAM_DOWN)) {
			charger->vout_mode = WIRELESS_VOUT_5_5V_STEP;
			cancel_delayed_work(&charger->wpc_vout_mode_work);
			__pm_stay_awake(charger->wpc_vout_mode_ws);
			queue_delayed_work(charger->wqueue, &charger->wpc_vout_mode_work, msecs_to_jiffies(250));
		}
	}
#endif
	__pm_relax(charger->wpc_cs100_ws);
}

static void mfc_tx_ac_missing_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_tx_ac_missing_work.work);
	union power_supply_propval value = {0, };

	if (!charger->wc_rx_connected) {
		pr_info("@Tx_Mode %s: retry\n", __func__);
		value.intval = BATT_TX_EVENT_WIRELESS_TX_AC_MISSING;
		psy_do_property("wireless", set, POWER_SUPPLY_EXT_PROP_WIRELESS_TX_ERR, value);
	}

	__pm_relax(charger->wpc_tx_ac_missing_ws);
}

#if defined(CONFIG_UPDATE_BATTERY_DATA)
static int mfc_chg_parse_dt(struct device *dev, mfc_charger_platform_data_t *pdata);
#endif
static int mfc_chg_set_property(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct mfc_charger_data *charger = power_supply_get_drvdata(psy);
	enum power_supply_ext_property ext_psp = (enum power_supply_ext_property) psp;

	switch ((int)psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (val->intval == POWER_SUPPLY_STATUS_FULL) {
			pr_info("%s: full status\n", __func__);
			__pm_stay_awake(charger->wpc_cs100_ws);
			if (charger->tx_id)
				queue_delayed_work(charger->wqueue, &charger->wpc_cs100_work, msecs_to_jiffies(0));
			else
				queue_delayed_work(charger->wqueue, &charger->wpc_cs100_work, msecs_to_jiffies(15000));
		} else if (val->intval == POWER_SUPPLY_STATUS_NOT_CHARGING) {
			mfc_mis_align(charger);
		} else if (val->intval == POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE) {
			pr_info("%s: CV status. tx_id(0x%x)\n", __func__, charger->tx_id);
			mfc_fod_set(charger, FOD_STATE_CV);
			if (charger->tx_id) {
				mfc_reg_write(charger->client, MFC_RX_COMM_MOD_AFC_FET_REG, CMA_ENABLE_CMB_ENABLE_DATA);
				mfc_reg_write(charger->client, MFC_RX_COMM_MOD_FET_REG, CMA_ENABLE_CMB_ENABLE_DATA);
			}
		}
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
	{
		union power_supply_propval value;
		value.intval = charger->pdata->cable_type;
		psy_do_property("wireless", set, POWER_SUPPLY_PROP_ONLINE, value);

		if (charger->is_otg_on) {
			psy_do_property("wireless", set,
				POWER_SUPPLY_EXT_PROP_CHARGE_OTG_CONTROL, value);
		}
	}
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		if (val->intval == POWER_SUPPLY_HEALTH_OVERHEAT || val->intval == POWER_SUPPLY_EXT_HEALTH_OVERHEATLIMIT ||
				val->intval == POWER_SUPPLY_HEALTH_COLD)
			mfc_send_eop(charger, val->intval);
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		pr_info("%s: ept-internal fault\n", __func__);
		mfc_reg_write(charger->client, MFC_EPT_REG, MFC_WPC_EPT_INT_FAULT);
		mfc_set_cmd_l_reg(charger, MFC_CMD_SEND_EOP_MASK, MFC_CMD_SEND_EOP_MASK);
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		if (val->intval) {
			charger->is_mst_on = MST_MODE_2;
			pr_info("%s: set MST mode 2\n", __func__);
		} else {
#if defined(CONFIG_MST_V2)
			// it will send MST driver a message.
			mfc_send_mst_cmd(0, charger, 0, 0);
#endif
			pr_info("%s: set MST mode off\n", __func__);
			charger->is_mst_on = MST_MODE_0;
		}
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		charger->pdata->siop_level = val->intval;
		if (charger->pdata->siop_level == 100) {
			pr_info("%s: vrect headroom set ROOM 2, siop = %d\n", __func__, charger->pdata->siop_level);
			mfc_set_vrect_adjust(charger, MFC_HEADROOM_2);
		} else if (charger->pdata->siop_level < 100) {
			pr_info("%s: vrect headroom set ROOM 0, siop = %d\n", __func__, charger->pdata->siop_level);
			mfc_set_vrect_adjust(charger, MFC_HEADROOM_0);
		}
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		charger->pdata->otp_firmware_result = val->intval;
		pr_info("%s: otp_firmware result initialize (%d)\n", __func__,
			charger->pdata->otp_firmware_result);
		break;
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		charger->pdata->capacity = val->intval;
		if (charger->wc_tx_enable) {
			pr_info("@Tx_Mode %s: FW Ver(%x) TX_VOUT(%dmV) TX_IOUT(%dmA) FRQ(%dKHz) TX_FRQ(%dKHZ), %s connected\n",
				__func__,
				charger->pdata->otp_firmware_ver,		/* FW Ver */
				mfc_get_adc(charger, MFC_ADC_TX_VOUT),		/* TX_VOUT */
				mfc_get_adc(charger, MFC_ADC_TX_IOUT),		/* TX_IOUT */
				mfc_get_adc(charger, MFC_ADC_OP_FRQ),		/* FRQ */
				mfc_get_adc(charger, MFC_ADC_TX_OP_FRQ),	/* TX_FRQ */
				rx_device_type_str[charger->wc_rx_type]);
		} else if (charger->pdata->cable_type != SEC_WIRELESS_PAD_NONE) {
	//		int i = 0;
			u8 fod[12] = {0, };
			int vout = mfc_get_adc(charger, MFC_ADC_VOUT);

			pr_info("%s: FW Ver(%x) RX_VOUT(%dmV) RX_VRECT(%dmV) RX_IOUT(%dmA) FRQ(%dKHz) IC Rev(0x%x) cable(%d)\n",
				__func__,
				charger->pdata->otp_firmware_ver,	/* FW Ver */
				vout,					/* RX_VOUT */
				mfc_get_adc(charger, MFC_ADC_VRECT),	/* RX_VRECT */
				mfc_get_adc(charger, MFC_ADC_RX_IOUT),	/* RX_IOUT */
				mfc_get_adc(charger, MFC_ADC_OP_FRQ),	/* FREQ */
				charger->pdata->wc_ic_rev,		/* IC Rev */
				charger->pdata->cable_type);

			mfc_reg_multi_read(charger->client, MFC_WPC_FOD_0A_REG, fod, 12);
			pr_info("%s: FOD(%d %d %d %d %d %d %d %d %d %d %d %d)\n", __func__,
				fod[0], fod[1], fod[2], fod[3], fod[4], fod[5],
				fod[6], fod[7], fod[8], fod[9], fod[10], fod[11]);
		}
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		charger->pdata->capacity = val->intval;
		if (charger->wc_rx_type == SS_GEAR)
			mfc_check_tx_gear_time(charger);
		break;
	case POWER_SUPPLY_PROP_CHARGE_EMPTY:
	{
		int vout = 0, vrect = 0;
		u8 is_vout_on = 0;
		int i = 0;

		mfc_reg_read(charger->client, MFC_STATUS_L_REG, &is_vout_on);
		is_vout_on = is_vout_on >> 7;
		vout = mfc_get_adc(charger, MFC_ADC_VOUT);
		vrect = mfc_get_adc(charger, MFC_ADC_VRECT);
		pr_info("%s: SET MFC LDO (%s), Current VOUT STAT (%d), RX_VOUT = %dmV, RX_VRECT = %dmV\n",
				__func__, (val->intval == MFC_LDO_ON ? "ON" : "OFF"), is_vout_on, vout, vrect);
		if ((val->intval == MFC_LDO_ON) && !is_vout_on) { /* LDO ON */
			pr_info("%s: MFC LDO ON toggle ------------ cable_work\n", __func__);
			for (i = 0; i < 2; i++) {
				mfc_reg_read(charger->client, MFC_STATUS_L_REG, &is_vout_on);
				is_vout_on = is_vout_on >> 7;
				if (!is_vout_on)
					mfc_set_cmd_l_reg(charger, MFC_CMD_TOGGLE_LDO_MASK, MFC_CMD_TOGGLE_LDO_MASK);
				else
					break;
				msleep(500);
				mfc_reg_read(charger->client, MFC_STATUS_L_REG, &is_vout_on);
				is_vout_on = is_vout_on >> 7;
				if (is_vout_on) {
					pr_info("%s: cnt = %d, LDO is ON -> MFC LDO STAT(%d)\n", __func__, i, is_vout_on);
					break;
				}
				msleep(1000);
				vout = mfc_get_adc(charger, MFC_ADC_VOUT);
				vrect = mfc_get_adc(charger, MFC_ADC_VRECT);
				pr_info("%s: cnt = %d, LDO Should ON -> MFC LDO STAT(%d), RX_VOUT = %dmV, RX_VRECT = %dmV\n",
						__func__, i, is_vout_on, vout, vrect);
			}
			charger->wc_ldo_status = MFC_LDO_ON;
		} else if ((val->intval == MFC_LDO_OFF) && is_vout_on) { /* LDO OFF */
			pr_info("%s: MFC LDO OFF toggle ------------ cable_work\n", __func__);
			mfc_set_cmd_l_reg(charger, MFC_CMD_TOGGLE_LDO_MASK, MFC_CMD_TOGGLE_LDO_MASK);
			msleep(400);
			mfc_reg_read(charger->client, MFC_STATUS_L_REG, &is_vout_on);
			is_vout_on = is_vout_on >> 7;
			vout = mfc_get_adc(charger, MFC_ADC_VOUT);
			vrect = mfc_get_adc(charger, MFC_ADC_VRECT);
			pr_info("%s: LDO Should OFF -> MFC LDO STAT(%d), RX_VOUT = %dmV, RX_VRECT = %dmV\n",
					__func__, is_vout_on, vout, vrect);
			charger->wc_ldo_status = MFC_LDO_OFF;
		}
	}
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		return -ENODATA;
	case POWER_SUPPLY_EXT_PROP_MIN ... POWER_SUPPLY_EXT_PROP_MAX:
		switch (ext_psp) {
		case POWER_SUPPLY_EXT_PROP_WC_CONTROL:
			if (val->intval == 0) {
				mfc_send_packet(charger, MFC_HEADER_AFC_CONF, WPC_COM_PAD_LED, WPC_PAD_LED_ON);
				pr_info("%s: keep PAD LED ON for LED cover\n", __func__);
				msleep(150);
			}
			break;
		case POWER_SUPPLY_EXT_PROP_WC_EPT_UNKNOWN:
			if (val->intval == 1) {
				pr_info("%s: EPT-Unknown\n", __func__);
				mfc_reg_write(charger->client, MFC_EPT_REG, MFC_WPC_EPT_UNKNOWN);
				mfc_set_cmd_l_reg(charger, MFC_CMD_SEND_EOP_MASK, MFC_CMD_SEND_EOP_MASK);
			}
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_SWITCH:
			/* It is a RX device , send a packet to TX device to stop power sharing.
				TX device will have MFC_INTA_H_TRX_DATA_RECEIVED_MASK irq */
			if (charger->pdata->cable_type == SEC_WIRELESS_PAD_TX) {
				if (val->intval) {
					pr_info("%s: It is a RX device , send a packet to TX device to stop power sharing\n",
							__func__);
					mfc_send_command(charger, MFC_DISABLE_TX);
				}
			}
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_SEND_FSK:
			/* send fsk packet for rx device aicl reset */
			if (val->intval && (charger->wc_rx_type != SS_GEAR)) {
				pr_info("@Tx_mode %s: Send FSK packet for Rx device aicl reset\n", __func__);
				mfc_send_fsk(charger, WPC_TX_COM_WPS, WPS_AICL_RESET);
			} else if (val->intval && (charger->wc_rx_type == SS_GEAR)) {
				pr_info("@Tx_mode %s: Send FSK packet for Rx wakeup\n", __func__);
				mfc_send_fsk(charger, WPC_TX_COM_TX_ID, TX_ID_UNO_TX);
			}
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_TX_ENABLE:
			/* on/off tx power */
			mfc_set_tx_power(charger, val->intval);
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_RX_CONNECTED:
			charger->wc_rx_connected = val->intval;
			__pm_stay_awake(charger->wpc_rx_connection_ws);
			queue_delayed_work(charger->wqueue, &charger->wpc_rx_connection_work, 0);
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_AUTH_ADT_STATUS: /* it has only PASS and FAIL */
#if !defined(CONFIG_SEC_FACTORY)
			if ((charger->pdata->cable_type != SEC_WIRELESS_PAD_NONE) &&
				!lpcharge &&
				(charger->adt_transfer_status != WIRELESS_AUTH_WAIT)) {
				if (charger->adt_transfer_status != val->intval) {
					charger->adt_transfer_status = val->intval;

					if (charger->adt_transfer_status == WIRELESS_AUTH_PASS) {
						/* this cable type will call a proper RX power having done vout_work */
						//charger->pdata->cable_type = SEC_WIRELESS_PAD_WPC_HV_20_LIMIT;
						charger->pdata->cable_type = SEC_WIRELESS_PAD_WPC_HV_20;
						pr_info("%s %s: PASS! type = %d\n", WC_AUTH_MSG,
								__func__, charger->pdata->cable_type);
					} else if (charger->adt_transfer_status == WIRELESS_AUTH_FAIL) {
						/* when auth is failed, cable type is HV wireless */
						charger->pdata->cable_type = SEC_WIRELESS_PAD_WPC_HV;
						pr_info("%s %s: FAIL\n", WC_AUTH_MSG, __func__);
					} else {
						pr_info("%s %s: undefined PASS/FAIL result\n", WC_AUTH_MSG, __func__);
					}
					__pm_stay_awake(charger->wpc_afc_vout_ws);
					queue_delayed_work(charger->wqueue, &charger->wpc_afc_vout_work, msecs_to_jiffies(0));
				} else {
					pr_info("%s %s: skip a same PASS/FAIL result\n", WC_AUTH_MSG, __func__);
				}
			} else {
				pr_info("%s %s: auth service sent wrong cmd\n", WC_AUTH_MSG, __func__);
			}
#endif
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_AUTH_ADT_DATA: /* data from auth service will be sent */
			if (charger->pdata->cable_type != SEC_WIRELESS_PAD_NONE && !lpcharge) {
				u8 *p_data;
				int i;
				p_data = (u8 *)val->strval;

				for (i = 0; i < adt_readSize; i++)
					pr_info("%s %s: p_data[%d] = %x\n", WC_AUTH_MSG, __func__, i, p_data[i]);
				mfc_auth_adt_send(charger, p_data, adt_readSize);
			}
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_AUTH_ADT_SIZE:
			if (charger->pdata->cable_type != SEC_WIRELESS_PAD_NONE)
				adt_readSize = val->intval;
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_RX_TYPE:
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_TX_VOUT:
			mfc_set_tx_vout(charger, val->intval);
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_TX_IOUT:
			mfc_set_tx_iout(charger, val->intval);
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_TIMER_ON:
			pr_info("%s %s: TX receiver detecting timer enable(%d)\n", WC_AUTH_MSG, __func__, val->intval);
			if (charger->wc_tx_enable) {
				if (val->intval) {
					pr_info("%s %s: enable TX OFF timer (90sec)", WC_AUTH_MSG, __func__);
					mfc_reg_update(charger->client, MFC_INT_A_ENABLE_H_REG,
							MFC_INTA_H_TX_MODE_RX_NOT_DET_MASK, MFC_INTA_H_TX_MODE_RX_NOT_DET_MASK);
				} else {
					pr_info("%s %s: disable TX OFF timer (90sec)", WC_AUTH_MSG, __func__);
					mfc_reg_update(charger->client, MFC_INT_A_ENABLE_H_REG,
							0, MFC_INTA_H_TX_MODE_RX_NOT_DET_MASK);
				}
			} else {
				pr_info("%s %s: Don't need to set TX 90sec timer, on TX OFF state\n", WC_AUTH_MSG, __func__);
			}
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_MIN_DUTY:
			cancel_delayed_work(&charger->wpc_tx_op_freq_work);
			if (val->intval == MIN_DUTY_SETTING_50_DATA)
				mfc_set_tx_op_freq(charger, TX_OP_FREQ_110KHZ_DATA);
			else
				mfc_set_tx_op_freq(charger, TX_OP_FREQ_DEFAULT_148KHZ_DATA);
			mfc_set_min_duty(charger, val->intval);
			break;
		case POWER_SUPPLY_EXT_PROP_CALL_EVENT:
			if (val->intval & BATT_EXT_EVENT_CALL) {
				charger->device_event |= BATT_EXT_EVENT_CALL;

				/* call in is after wireless connection */
				if (charger->pdata->cable_type == SEC_WIRELESS_PAD_WPC_PACK ||
					charger->pdata->cable_type == SEC_WIRELESS_PAD_WPC_PACK_HV ||
					charger->pdata->cable_type == SEC_WIRELESS_PAD_TX) {
					union power_supply_propval value2;
					pr_info("%s %s: enter PHM \n", WC_TX_MSG, __func__);
					/* notify "wireless" PHM status */
					value2.intval = 1;
					psy_do_property("wireless", set, POWER_SUPPLY_EXT_PROP_CALL_EVENT, value2);
					mfc_send_command(charger, MFC_PHM_ON);
					msleep(250);
					mfc_send_command(charger, MFC_PHM_ON);
				}
			} else if (val->intval == BATT_EXT_EVENT_NONE) {
				charger->device_event &= ~BATT_EXT_EVENT_CALL;
			}
			break;
#if defined(CONFIG_TX_GEAR_AOV)
		case POWER_SUPPLY_EXT_PROP_FORCED_PHM_CTRL:
			if (val->intval == 1) {
				if (charger->tx_gear_phm) {
					pr_info("@Tx_Mode %s: already PHM(%d). cancel old PHM alarm\n",
						__func__, charger->tx_gear_phm);
					cancel_delayed_work(&charger->wpc_tx_phm_work);
					__pm_relax(charger->wpc_tx_phm_ws);
					alarm_cancel(&charger->phm_alarm);
				} else {
					mfc_set_cmd_l_reg(charger, MFC_CMD_TOGGLE_PHM_MASK, MFC_CMD_TOGGLE_PHM_MASK);
					charger->tx_gear_phm = 1;
					pr_info("@Tx_Mode %s: enter forced PHM(%d)\n",
						__func__, charger->tx_gear_phm);
				}
				/* start aov phm alarm timer */
				pr_info("@Tx_Mode %s: Start AOV PHM alarm by %d seconds\n",
					__func__, charger->pdata->gear_aov_phm_alarm);
				alarm_start(&charger->phm_alarm, ktime_add(ktime_get_boottime(),
					ktime_set(charger->pdata->gear_aov_phm_alarm, 0)));
			} else {
				if (charger->tx_gear_phm) {
					mfc_set_cmd_l_reg(charger, MFC_CMD_TOGGLE_PHM_MASK, MFC_CMD_TOGGLE_PHM_MASK);
					charger->tx_gear_phm = 0;
					pr_info("@Tx_Mode %s: escape forced PHM(%d). cancel PHM alarm\n",
						__func__, charger->tx_gear_phm);
					cancel_delayed_work(&charger->wpc_tx_phm_work);
					__pm_relax(charger->wpc_tx_phm_ws);
					alarm_cancel(&charger->phm_alarm);
				} else {
					pr_info("@Tx_Mode %s: already not PHM(%d)\n",
						__func__, charger->tx_gear_phm);
				}
				mfc_set_tx_fod_with_gear(charger);
			}
			break;
#endif
		case POWER_SUPPLY_EXT_PROP_SLEEP_MODE:
			charger->sleep_mode = val->intval;
			break;
		case POWER_SUPPLY_EXT_PROP_PAD_VOLT_CTRL:
			if (charger->pdata->wpc_vout_ctrl_lcd_on) {
				if (delayed_work_pending(&charger->wpc_vout_mode_work)) {
					pr_info("%s: Already vout change. skip pad control\n", __func__);
					return 0;
				}
				if (val->intval && charger->is_afc_tx &&
					(charger->tx_id != TX_ID_UNKNOWN) &&
					(charger->tx_id != TX_ID_DREAM_DOWN &&
					charger->tx_id != TX_ID_DREAM_STAND)) {
					mfc_send_command(charger, MFC_AFC_CONF_5V_TX);
					pr_info("%s: set TX 5V because LCD ON\n", __func__);
					charger->is_afc_tx = false;
					charger->pad_ctrl_by_lcd = true;
				} else if (!val->intval && !charger->is_afc_tx && charger->pad_ctrl_by_lcd){
					if (charger->pdata->cable_type == SEC_WIRELESS_PAD_WPC_HV_20)
						mfc_send_command(charger, charger->vrect_by_txid);
					else
						mfc_send_command(charger, MFC_AFC_CONF_10V_TX);
					charger->is_afc_tx = true;
					charger->pad_ctrl_by_lcd = false;
					pr_info("%s: need to set afc tx because LCD OFF\n", __func__);
				}
			}
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_VOUT:
			{
				int i = 0;

				for (i = 0; i < charger->pdata->len_wc20_list; i++)
					charger->pdata->wireless20_vout_list[i] = val->intval;

				pr_info("%s: vout(%d) len(%d)\n", __func__, val->intval, i - 1);
			}
			break;
		case POWER_SUPPLY_EXT_PROP_WIRELESS_2ND_DONE:
			mfc_reg_write(charger->client, MFC_RX_COMM_MOD_AFC_FET_REG, CMA_ENABLE_CMB_DISABLE_DATA);
			mfc_reg_write(charger->client, MFC_RX_COMM_MOD_FET_REG, CMA_ENABLE_CMB_DISABLE_DATA);
			pr_info("%s: 2nd wireless charging done! CMA only\n", __func__);
			break;
		case POWER_SUPPLY_EXT_PROP_WPC_EN:
			mfc_set_wpc_en(charger, val->strval[0], val->strval[1]);
			break;
		case POWER_SUPPLY_EXT_PROP_WPC_EN_MST:
			if (val->intval)
				mfc_set_wpc_en(charger, WPC_EN_MST, true);
			else
				mfc_set_wpc_en(charger, WPC_EN_MST, false);
			break;
		case POWER_SUPPLY_EXT_PROP_CHARGE_OTG_CONTROL:
			if (val->intval) {
				pr_info("%s: mfc otg on\n", __func__);
				charger->is_otg_on = true;
			} else {
				pr_info("%s: mfc otg off\n", __func__);
				charger->is_otg_on = false;
			}
			break;
		case POWER_SUPPLY_EXT_PROP_CHARGE_POWERED_OTG_CONTROL:
			charger->fw_cmd = val->intval;
			queue_delayed_work(charger->wqueue, &charger->wpc_fw_update_work, 0);
			pr_info("%s: rx result = %d, tx result = %d\n", __func__,
				charger->pdata->otp_firmware_result, charger->pdata->tx_firmware_result);
			break;
		case POWER_SUPPLY_EXT_PROP_INPUT_VOLTAGE_REGULATION:
			if (val->intval == WIRELESS_VOUT_NORMAL_VOLTAGE) {
				int i = 0;
				pr_info("%s: Wireless Vout forced set to 5V. + PAD CMD\n", __func__);
				for (i = 0; i < CMD_CNT; i++) {
					mfc_send_command(charger, MFC_AFC_CONF_5V);
					msleep(250);
				}
			} else if (val->intval == WIRELESS_VOUT_HIGH_VOLTAGE) {
				int i = 0;
				pr_info("%s: Wireless Vout forced set to 10V. + PAD CMD\n", __func__);
				for (i = 0; i < CMD_CNT; i++) {
					mfc_send_command(charger, MFC_AFC_CONF_10V);
					msleep(250);
				}
			} else if (val->intval == WIRELESS_VOUT_CC_CV_VOUT) {
				charger->vout_mode = val->intval;
				cancel_delayed_work(&charger->wpc_vout_mode_work);
				__pm_stay_awake(charger->wpc_vout_mode_ws);
				queue_delayed_work(charger->wqueue, &charger->wpc_vout_mode_work, 0);
			} else if (val->intval == WIRELESS_VOUT_5V ||
					val->intval == WIRELESS_VOUT_5V_STEP ||
					val->intval == WIRELESS_VOUT_5_5V_STEP) {
				int def_delay = 0;
				charger->vout_mode = val->intval;
				if (is_hv_wireless_pad_type(charger->pdata->cable_type)) {
					def_delay = 250;
				}
				cancel_delayed_work(&charger->wpc_vout_mode_work);
				__pm_stay_awake(charger->wpc_vout_mode_ws);
				queue_delayed_work(charger->wqueue, &charger->wpc_vout_mode_work, msecs_to_jiffies(def_delay));
			} else if (val->intval == WIRELESS_VOUT_9V ||
				val->intval == WIRELESS_VOUT_10V ||
				val->intval == WIRELESS_VOUT_11V ||
				val->intval == WIRELESS_VOUT_12V ||
				val->intval == WIRELESS_VOUT_12_5V ||
				val->intval == WIRELESS_VOUT_9V_STEP ||
				val->intval == WIRELESS_VOUT_10V_STEP) {
				if (!charger->is_full_status && !is_shutdn) {
					if (!charger->is_afc_tx) {
						pr_info("%s: need to set afc tx before vout control\n", __func__);

						if (charger->pdata->cable_type == SEC_WIRELESS_PAD_WPC_HV_20)
							mfc_send_command(charger, charger->vrect_by_txid);
						else
							mfc_send_command(charger, MFC_AFC_CONF_10V_TX);
						charger->is_afc_tx = true;

						if (charger->pad_ctrl_by_lcd)
							charger->pad_ctrl_by_lcd = false;

						pr_info("%s: is_afc_tx = %d vout read = %d \n",
							__func__, charger->is_afc_tx, mfc_get_adc(charger, MFC_ADC_VOUT));
					}
					charger->vout_mode = val->intval;
					cancel_delayed_work(&charger->wpc_vout_mode_work);
					__pm_stay_awake(charger->wpc_vout_mode_ws);
					queue_delayed_work(charger->wqueue,
						&charger->wpc_vout_mode_work, msecs_to_jiffies(250));
				} else {
					pr_info("%s: block to set high vout level(vs=%s) because full status(%d), shutdn(%d)\n",
						__func__, rx_vout_str[charger->pdata->vout_status],
						charger->is_full_status, is_shutdn);
				}
			} else if (val->intval == WIRELESS_PAD_FAN_OFF) {
				pr_info("%s: fan off\n", __func__);
				mfc_fan_control(charger, 0);
			} else if (val->intval == WIRELESS_PAD_FAN_ON) {
				pr_info("%s: fan on\n", __func__);
				mfc_fan_control(charger, 1);
			} else if (val->intval == WIRELESS_PAD_LED_OFF) {
				pr_info("%s: led off\n", __func__);
				mfc_led_control(charger, MFC_LED_CONTROL_OFF);
			} else if (val->intval == WIRELESS_PAD_LED_ON) {
				pr_info("%s: led on\n", __func__);
				mfc_led_control(charger, MFC_LED_CONTROL_ON);
			} else if (val->intval == WIRELESS_PAD_LED_DIMMING) {
				pr_info("%s: led dimming\n", __func__);
			} else if (val->intval == WIRELESS_VRECT_ADJ_ON) {
				pr_info("%s: vrect adjust to have big headroom(default value)\n", __func__);
				mfc_set_vrect_adjust(charger, MFC_HEADROOM_1);
			} else if (val->intval == WIRELESS_VRECT_ADJ_OFF) {
				pr_info("%s: vrect adjust to have small headroom\n", __func__);
				mfc_set_vrect_adjust(charger, MFC_HEADROOM_0);
			} else if (val->intval == WIRELESS_VRECT_ADJ_ROOM_0) {
				pr_info("%s: vrect adjust to have headroom 0(0mV)\n", __func__);
				mfc_set_vrect_adjust(charger, MFC_HEADROOM_0);
			} else if (val->intval == WIRELESS_VRECT_ADJ_ROOM_1) {
				pr_info("%s: vrect adjust to have headroom 1(277mV)\n", __func__);
				mfc_set_vrect_adjust(charger, MFC_HEADROOM_1);
			} else if (val->intval == WIRELESS_VRECT_ADJ_ROOM_2) {
				pr_info("%s: vrect adjust to have headroom 2(497mV)\n", __func__);
				mfc_set_vrect_adjust(charger, MFC_HEADROOM_2);
			} else if (val->intval == WIRELESS_VRECT_ADJ_ROOM_3) {
				pr_info("%s: vrect adjust to have headroom 3(650mV)\n", __func__);
				mfc_set_vrect_adjust(charger, MFC_HEADROOM_3);
			} else if (val->intval == WIRELESS_VRECT_ADJ_ROOM_4) {
				pr_info("%s: vrect adjust to have headroom 4(30mV)\n", __func__);
				mfc_set_vrect_adjust(charger, MFC_HEADROOM_4);
			} else if (val->intval == WIRELESS_VRECT_ADJ_ROOM_5) {
				pr_info("%s: vrect adjust to have headroom 5(82mV)\n", __func__);
				mfc_set_vrect_adjust(charger, MFC_HEADROOM_5);
			} else if (val->intval == WIRELESS_CLAMP_ENABLE) {
				pr_info("%s: enable clamp1, clamp2 for WPC modulation\n", __func__);
			} else if (val->intval == WIRELESS_SLEEP_MODE_ENABLE) {
				if (is_sleep_mode_active(charger->tx_id)) {
					pr_info("%s: sleep_mode enable\n", __func__);
					msleep(500);
					pr_info("%s: led dimming\n", __func__);
					mfc_led_control(charger, MFC_LED_CONTROL_DIMMING);
					msleep(500);
					pr_info("%s: fan off\n", __func__);
					mfc_fan_control(charger, 0);
				} else {
					pr_info("%s: sleep_mode inactive\n", __func__);
				}
			} else if (val->intval == WIRELESS_SLEEP_MODE_DISABLE) {
				if (is_sleep_mode_active(charger->tx_id)) {
					pr_info("%s: sleep_mode disable\n", __func__);
					msleep(500);
					pr_info("%s: led on\n", __func__);
					mfc_led_control(charger, MFC_LED_CONTROL_ON);
					msleep(500);
					pr_info("%s: fan on\n", __func__);
					mfc_fan_control(charger, 1);
				} else {
					pr_info("%s: sleep_mode inactive\n", __func__);
				}
			} else {
				pr_info("%s: Unknown Command(%d)\n", __func__, val->intval);
			}
			break;
#if defined(CONFIG_UPDATE_BATTERY_DATA)
		case POWER_SUPPLY_EXT_PROP_POWER_DESIGN:
			mfc_chg_parse_dt(charger->dev, charger->pdata);
			break;
#endif
		case POWER_SUPPLY_EXT_PROP_FILTER_CFG:
			charger->led_cover = val->intval;
			pr_info("%s: LED_COVER(%d)\n", __func__, charger->led_cover);
			break;

		default:
			return -ENODATA;
		}
		break;
	default:
		return -ENODATA;
	}

	return 0;
}

static void mfc_wpc_opfq_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_opfq_work.work);

	u16 op_fq;
	u8 pad_mode;
	union power_supply_propval value;

	mfc_reg_read(charger->client, MFC_SYS_OP_MODE_REG, &pad_mode);
	if ((pad_mode == PAD_MODE_WPC_BASIC) ||	(pad_mode == PAD_MODE_WPC_ADV)) {
		op_fq = mfc_get_adc(charger, MFC_ADC_OP_FRQ);
		pr_info("%s: Operating FQ %dkHz(0x%x)\n", __func__, op_fq, op_fq);
		if (op_fq > 230) { /* wpc threshold 230kHz */
			pr_info("%s: Reset M0\n", __func__);
			mfc_reg_write(charger->client, 0x3040, 0x80); /*restart M0 */

			charger->pdata->opfq_cnt++;
			if (charger->pdata->opfq_cnt <= CMD_CNT) {
				queue_delayed_work(charger->wqueue, &charger->wpc_opfq_work, msecs_to_jiffies(10000));
				return;
			}
		}
	} else if ((pad_mode == PAD_MODE_PMA_SR1) || (pad_mode == PAD_MODE_PMA_SR1E)) {
			charger->pdata->cable_type = value.intval = SEC_WIRELESS_PAD_PMA;
			psy_do_property("wireless", set, POWER_SUPPLY_PROP_ONLINE, value);
	}
	charger->pdata->opfq_cnt = 0;
	__pm_relax(charger->wpc_opfq_ws);

}

static u32 mfc_get_wireless20_vout_by_txid(struct mfc_charger_data *charger, u8 txid)
{
	u32 vout = 0, i = 0;

	if (txid < TX_ID_AUTH_PAD || txid > TX_ID_AUTH_PAD_END) {
		pr_info("%s: wrong tx id(0x%x) for wireless 2.0\n", __func__, txid);
		i = 0; /* defalut value is WIRELESS_VOUT_10V */
	} else if (txid >= TX_ID_AUTH_PAD + charger->pdata->len_wc20_list) {
		pr_info("%s: undefined tx id(0x%x) of the device\n", __func__, txid);
		i = charger->pdata->len_wc20_list - 1;
	} else
		i = txid - TX_ID_AUTH_PAD;

	vout = charger->pdata->wireless20_vout_list[i];
	pr_info("%s: vout = %d\n", __func__, vout);
	return vout;
}

static u32 mfc_get_wireless20_vrect_by_txid(struct mfc_charger_data *charger, u8 txid)
{
	u32 vrect = 0, i = 0;

	if (txid < TX_ID_AUTH_PAD || txid > TX_ID_AUTH_PAD_END) {
		pr_info("%s: wrong tx id(0x%x) for wireless 2.0\n", __func__, txid);
		i = 0; /* defalut value is MFC_AFC_CONF_10V_TX */
	} else if (txid >= TX_ID_AUTH_PAD + charger->pdata->len_wc20_list) {
		pr_info("%s: undefined tx id(0x%x) of the device\n", __func__, txid);
		i = charger->pdata->len_wc20_list - 1;
	} else {
		i = txid - TX_ID_AUTH_PAD;
	}

	vrect = charger->pdata->wireless20_vrect_list[i];
	pr_info("%s: vrect = %d\n", __func__, vrect);
	return vrect;
}

static u32 mfc_get_wireless20_max_power_by_txid(struct mfc_charger_data *charger, u8 txid)
{
	u32 max_power = 0, i = 0;

	if (txid < TX_ID_AUTH_PAD || txid > TX_ID_AUTH_PAD_END) {
		pr_info("%s: wrong tx id(0x%x) for wireless 2.0\n", __func__, txid);
		i = 0; /* defalut value is SEC_WIRELESS_RX_POWER_12W */
	} else if (txid >= TX_ID_AUTH_PAD + charger->pdata->len_wc20_list) {
		pr_info("%s: undefined tx id(0x%x) of the device\n", __func__, txid);
		i = charger->pdata->len_wc20_list - 1;
	} else {
		i = txid - TX_ID_AUTH_PAD;
	}

	max_power = charger->pdata->wireless20_max_power_list[i];
	pr_info("%s: max rx power = %d\n", __func__, max_power);
	return max_power;
}

static void mfc_wpc_det_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_det_work.work);
	int wc_w_state;
	union power_supply_propval value;
	u8 pad_mode;
	u8 vrect;
	int vrect_level, vout_level;

	if (charger->is_mst_on == MST_MODE_2) {
		pr_info("%s: check wpc-state(%d - %d)\n", __func__,
			charger->wc_w_state, gpio_get_value(charger->pdata->wpc_det));

		if (charger->wc_w_state == 0) {
			pr_info("%s: skip wpc_det_work for MST operation\n", __func__);
			return;
		}
	}

	wc_w_state = gpio_get_value(charger->pdata->wpc_det);
	pr_info("%s: w(%d to %d)\n", __func__, charger->wc_w_state, wc_w_state);
	if (charger->wc_w_state == wc_w_state)
		return;

	mfc_get_chip_id(charger);
	__pm_stay_awake(charger->wpc_ws);
	pr_info("%s: start\n", __func__);
	charger->wc_w_state = wc_w_state;
	if (charger->wc_w_state) {
#if !IS_ENABLED(CONFIG_MST_V2)
		mfc_set_for_mst_removal(charger);
#endif
		charger->initial_wc_check = true;

		charger->pdata->otp_firmware_ver = mfc_get_firmware_version(charger, MFC_RX_FIRMWARE);
		charger->pdata->wc_ic_rev = mfc_get_ic_revision(charger, MFC_IC_REVISION);

		if (charger->is_otg_on)
			charger->pdata->vout_status = MFC_VOUT_5V;
		else
			charger->pdata->vout_status = MFC_VOUT_5_5V;

		mfc_set_vout(charger, charger->pdata->vout_status);

		charger->wc_tx_enable = false;
		charger->gear_start_time = 0;
#if 0 /* To prepare for the future issue */
		/* read firmware version */
		if (mfc_get_firmware_version(charger, MFC_RX_FIRMWARE) == MFC_OTP_FIRM_VERSION && adc_cal > 0)
			mfc_runtime_sram_change(charger);/* change sram */
#endif

		/* enable Mode Change INT */
		mfc_reg_update(charger->client, MFC_INT_A_ENABLE_L_REG,
						MFC_STAT_L_OP_MODE_MASK, MFC_STAT_L_OP_MODE_MASK);

		mfc_reg_update(charger->client, MFC_INT_A_ENABLE_L_REG,
						MFC_STAT_H_ADT_RECEIVED_MASK, MFC_STAT_H_ADT_RECEIVED_MASK);

		/* SET OV CLAMP 17V */
		mfc_reg_write(charger->client, MFC_RX_OV_CLAMP_REG, OV_CLAMP_17V_DATA);

		/* SET ILIM */
		mfc_reg_write(charger->client, MFC_ILIM_SET_REG, MFC_ILIM_MAX_DATA);

		/* read vrect adjust */
		mfc_reg_read(charger->client, MFC_VRECT_ADJ_REG, &vrect);

		pr_info("%s: wireless charger activated, set V_INT as PN\n", __func__);

		/* default : CMA enable only */
		mfc_reg_write(charger->client, MFC_RX_COMM_MOD_AFC_FET_REG, CMA_ENABLE_CMB_DISABLE_DATA);
		mfc_reg_write(charger->client, MFC_RX_COMM_MOD_FET_REG, CMA_ENABLE_CMB_DISABLE_DATA);

		/* read pad mode */
		mfc_reg_read(charger->client, MFC_SYS_OP_MODE_REG, &pad_mode);
		pad_mode = pad_mode >> MFC_SYS_OP_MODE_REG_RX_MODE_SHIFT;
		pr_info("%s: Pad type (0x%x)\n", __func__, pad_mode);
		if ((pad_mode == PAD_MODE_PMA_SR1) ||
			(pad_mode == PAD_MODE_PMA_SR1E)) {
			charger->pdata->cable_type = value.intval = SEC_WIRELESS_PAD_PMA;
			psy_do_property("wireless", set, POWER_SUPPLY_PROP_ONLINE, value);
		} else { /* WPC */
			charger->pdata->cable_type = value.intval = SEC_WIRELESS_PAD_WPC;
			psy_do_property("wireless", set, POWER_SUPPLY_PROP_ONLINE, value);
			__pm_stay_awake(charger->wpc_opfq_ws);
			queue_delayed_work(charger->wqueue, &charger->wpc_opfq_work, msecs_to_jiffies(10000));
		}

		/* set fod value */
		psy_do_property("battery", get, POWER_SUPPLY_PROP_CAPACITY, value);
		mfc_fod_set(charger, (value.intval > 85) ? FOD_STATE_CV : FOD_STATE_CC);

#if !defined(CONFIG_WIRELESS_NO_HV)
		vrect_level = mfc_get_adc(charger, MFC_ADC_VRECT);
		vout_level = mfc_get_adc(charger, MFC_ADC_VOUT);
		pr_info("%s: read vrect(%dmV), vout(%dmV)\n", __func__, vrect_level, vout_level);

		/* reboot status with previous hv voltage setting */
		if (vrect_level >= 8500 && vout_level >= 8500) {
			/* re-set vout level */
			charger->pad_vout = PAD_VOUT_10V;
			mfc_set_vout(charger, MFC_VOUT_10V);

			/* change cable type */
			charger->pdata->cable_type = value.intval = SEC_WIRELESS_PAD_WPC_HV;
			psy_do_property("wireless", set,
				POWER_SUPPLY_PROP_ONLINE, value);
		} else {
			/* send request afc_tx , request afc is mandatory */
			msleep(200);
			mfc_send_command(charger, MFC_REQUEST_AFC_TX);
		}
#endif
		/* set rpp scaling factor for LED cover */
		mfc_rpp_set(charger);
		charger->pdata->is_charging = 1;

		/* For wire + wireless case */
		psy_do_property("battery", get,
			POWER_SUPPLY_PROP_STATUS, value);
		if (value.intval == POWER_SUPPLY_STATUS_FULL) {
			pr_info("%s: full status\n", __func__);
			queue_delayed_work(charger->wqueue, &charger->wpc_cs100_work, msecs_to_jiffies(15000));
		}

		__pm_stay_awake(charger->wpc_tx_id_ws);
		queue_delayed_work(charger->wqueue, &charger->wpc_tx_id_work, msecs_to_jiffies(2500));
	} else {
		/* Send last tx_id to battery to count tx_id */
		value.intval = charger->tx_id;
		psy_do_property("wireless", set, POWER_SUPPLY_PROP_AUTHENTIC, value);
		charger->pdata->cable_type = SEC_WIRELESS_PAD_NONE;
		charger->pdata->is_charging = 0;
		charger->pdata->vout_status = MFC_VOUT_5V;
		charger->pad_vout = PAD_VOUT_5V;
		charger->pdata->opfq_cnt = 0;
		charger->pdata->trx_data_cmd = 0;
		charger->pdata->trx_data_val = 0;
		charger->vout_mode = 0;
		charger->is_full_status = 0;
		charger->pdata->capacity = 101;
		charger->is_afc_tx = false;
		charger->pad_ctrl_by_lcd = false;
		charger->tx_id = TX_ID_UNKNOWN;
		charger->i2c_error_count = 0;
		charger->adt_transfer_status = WIRELESS_AUTH_WAIT;
		charger->current_rx_power = TX_RX_POWER_0W;
		charger->tx_id_cnt = 0;
		charger->wc_ldo_status = MFC_LDO_ON;
		charger->tx_id_done = false;
		charger->req_tx_id = false;
		charger->is_abnormal_pad = false;
		value.intval = charger->is_abnormal_pad;
		psy_do_property("wireless", set,
			POWER_SUPPLY_EXT_PROP_WIRELESS_ABNORMAL_PAD, value);

		value.intval = SEC_WIRELESS_PAD_NONE;
		psy_do_property("wireless", set, POWER_SUPPLY_PROP_ONLINE, value);
		pr_info("%s: wpc deactivated, set V_INT as PD\n", __func__);

		if (delayed_work_pending(&charger->wpc_opfq_work)) {
			__pm_relax(charger->wpc_opfq_ws);
			cancel_delayed_work(&charger->wpc_opfq_work);
		}
		if (delayed_work_pending(&charger->wpc_afc_vout_work)) {
			__pm_relax(charger->wpc_afc_vout_ws);
			cancel_delayed_work(&charger->wpc_afc_vout_work);
		}
		if (delayed_work_pending(&charger->wpc_vout_mode_work)) {
			__pm_relax(charger->wpc_vout_mode_ws);
			cancel_delayed_work(&charger->wpc_vout_mode_work);
		}
		if (delayed_work_pending(&charger->wpc_cs100_work)) {
			__pm_relax(charger->wpc_cs100_ws);
			cancel_delayed_work(&charger->wpc_cs100_work);
		}
		if (delayed_work_pending(&charger->wpc_tx_ac_missing_work)) {
			__pm_relax(charger->wpc_tx_ac_missing_ws);
			cancel_delayed_work(&charger->wpc_tx_ac_missing_work);
		}

		cancel_delayed_work(&charger->wpc_isr_work);
		cancel_delayed_work(&charger->wpc_tx_isr_work);
		cancel_delayed_work(&charger->wpc_tx_id_work);
		cancel_delayed_work(&charger->wpc_i2c_error_work);
		__pm_relax(charger->wpc_rx_ws);
		__pm_relax(charger->wpc_tx_ws);
		__pm_relax(charger->wpc_tx_id_ws);
	}
	__pm_relax(charger->wpc_ws);

	/* cancel vrect check work */
	__pm_relax(charger->wpc_vrect_check_ws);
	cancel_delayed_work(&charger->wpc_vrect_check_work);
	charger->initial_vrect = false;

	pr_info("%s: end\n", __func__);
}

/* INT_A */
static void mfc_wpc_tx_isr_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_tx_isr_work.work);

	u8 status_l = 0, status_h = 0;
	int ret = 0;

	pr_info("@Tx_Mode %s\n", __func__);

	if (!charger->wc_tx_enable || !charger->wc_rx_connected) {
		__pm_relax(charger->wpc_tx_ws);
		return;
	}

	ret = mfc_reg_read(charger->client, MFC_STATUS_L_REG, &status_l);
	ret = mfc_reg_read(charger->client, MFC_STATUS_H_REG, &status_h);

	pr_info("@Tx_Mode %s: DATA RECEIVED! status(0x%x) \n", __func__, status_h << 8 | status_l);

	mfc_tx_handle_rx_packet(charger);

	__pm_relax(charger->wpc_tx_ws);
}

/* INT_A */
static void mfc_wpc_isr_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_isr_work.work);

	u8 cmd_data, val_data;

	union power_supply_propval value = {0, };

	if (!charger->wc_w_state) {
		pr_info("%s: charger->wc_w_state is 0. exit wpc_isr_work.\n", __func__);
		__pm_relax(charger->wpc_rx_ws);
		return;
	}

	pr_info("%s: cable_type (0x%x)\n", __func__, charger->pdata->cable_type);

	mfc_reg_read(charger->client, MFC_WPC_TRX_DATA2_COM_REG, &cmd_data);
	mfc_reg_read(charger->client, MFC_WPC_TRX_DATA2_VALUE1_REG, &val_data);
	charger->pdata->trx_data_cmd = cmd_data;
	charger->pdata->trx_data_val = val_data;

	pr_info("%s: WPC Interrupt Occurred, CMD : 0x%x, DATA : 0x%x\n", __func__, cmd_data, val_data);

	/* None or RX mode */
	if (cmd_data == WPC_TX_COM_AFC_SET) {
		if (charger->req_tx_id) {
			pr_err("%s: requested tx id, but other packet received.\n", __func__);
			charger->req_tx_id = false;
			charger->is_abnormal_pad = true;
			value.intval = charger->is_abnormal_pad;
			psy_do_property("wireless", set,
				POWER_SUPPLY_EXT_PROP_WIRELESS_ABNORMAL_PAD, value);
			if (charger->tx_id_cnt < TX_ID_CHECK_CNT)
				mfc_send_command(charger, MFC_AFC_CONF_5V_TX);
			__pm_relax(charger->wpc_rx_ws);
			return;
		}

		switch (val_data) {
		case TX_AFC_SET_5V:
			pr_info("%s: data = 0x%x, might be 5V irq\n", __func__, val_data);
			charger->pad_vout = PAD_VOUT_5V;
			break;
		case TX_AFC_SET_10V:
			pr_info("%s: data = 0x%x, might be 10V irq\n", __func__, val_data);
			if (!gpio_get_value(charger->pdata->wpc_det)) {
				pr_err("%s: Wireless charging is paused during set high voltage.\n", __func__);
				__pm_relax(charger->wpc_rx_ws);
				return;
			}
#if !defined(CONFIG_WIRELESS_NO_HV)
			if (is_hv_wireless_pad_type(charger->pdata->cable_type) ||
				charger->pdata->cable_type == SEC_WIRELESS_PAD_PREPARE_HV) {
				pr_err("%s: Is is already HV wireless cable. No need to set again\n", __func__);
				__pm_relax(charger->wpc_rx_ws);
				return;
			}

			if (charger->is_abnormal_pad) {
				pr_err("%s: Abnormal pad, Do not set\n", __func__);
				__pm_relax(charger->wpc_rx_ws);
				return;
			}

			/* send AFC_SET */
			mfc_send_command(charger, MFC_AFC_CONF_10V);
			msleep(500);

			/* change cable type */
			charger->pdata->cable_type = value.intval = SEC_WIRELESS_PAD_PREPARE_HV;
			psy_do_property("wireless", set, POWER_SUPPLY_PROP_ONLINE, value);

			charger->pad_vout = PAD_VOUT_10V;
#endif
			break;
		case TX_AFC_SET_12V:
			break;
		case TX_AFC_SET_18V:
		case TX_AFC_SET_19V:
		case TX_AFC_SET_20V:
		case TX_AFC_SET_24V:
			break;
		default:
			pr_info("%s: unsupport : 0x%x", __func__, val_data);
			break;
		}
	} else if (cmd_data == WPC_TX_COM_TX_ID) {
		if (charger->req_tx_id)
			charger->req_tx_id = false;

		if (!charger->tx_id_done) {
			charger->tx_id = val_data;
			mfc_fod_set(charger, (charger->pdata->capacity > 85) ? FOD_STATE_CV : FOD_STATE_CC);

			switch (val_data) {
			case TX_ID_UNKNOWN:
				break;
			case TX_ID_VEHICLE_PAD:
				if (charger->pad_vout == PAD_VOUT_10V) {
					if (charger->pdata->cable_type == SEC_WIRELESS_PAD_PREPARE_HV) {
						charger->pdata->cable_type = SEC_WIRELESS_PAD_VEHICLE_HV;
						value.intval = SEC_WIRELESS_PAD_PREPARE_HV;
					} else {
						charger->pdata->cable_type = value.intval = SEC_WIRELESS_PAD_VEHICLE_HV;
					}
				} else {
					charger->pdata->cable_type = value.intval = SEC_WIRELESS_PAD_VEHICLE;
				}
				pr_info("%s: VEHICLE Wireless Charge PAD %s\n", __func__,
					charger->pad_vout == PAD_VOUT_10V ? "HV" : "");

				break;
			case TX_ID_STAND_TYPE_START:
				if (charger->pad_vout == PAD_VOUT_10V) {
					if (charger->pdata->cable_type == SEC_WIRELESS_PAD_PREPARE_HV) {
						charger->pdata->cable_type = SEC_WIRELESS_PAD_WPC_STAND_HV;
						value.intval = SEC_WIRELESS_PAD_PREPARE_HV;
					} else {
						charger->pdata->cable_type = value.intval = SEC_WIRELESS_PAD_WPC_STAND_HV;
					}
				} else {
					charger->pdata->cable_type = value.intval = SEC_WIRELESS_PAD_WPC_STAND;
				}
				pr_info("%s: Cable(%d), STAND Wireless Charge PAD %s\n", __func__,
					charger->pdata->cable_type, charger->pad_vout == PAD_VOUT_10V ? "HV" : "");
				break;
			case TX_ID_MULTI_PORT_START ... TX_ID_MULTI_PORT_END:
				value.intval = charger->pdata->cable_type;
				pr_info("%s: MULTI PORT PAD : 0x%x\n", __func__, val_data);
				break;
			case TX_ID_BATT_PACK ... TX_ID_BATT_PACK_END:
				if (charger->pad_vout == PAD_VOUT_10V) {
					if (charger->pdata->cable_type == SEC_WIRELESS_PAD_PREPARE_HV) {
						charger->pdata->cable_type = SEC_WIRELESS_PAD_WPC_PACK_HV;
						value.intval = SEC_WIRELESS_PAD_PREPARE_HV;
						pr_info("%s: WIRELESS HV BATTERY PACK (PREP)\n", __func__);
					} else {
						charger->pdata->cable_type = value.intval = SEC_WIRELESS_PAD_WPC_PACK_HV;
						pr_info("%s: WIRELESS HV BATTERY PACK\n", __func__);
					}
				} else {
					charger->pdata->cable_type = value.intval = SEC_WIRELESS_PAD_WPC_PACK;
					pr_info("%s: WIRELESS BATTERY PACK\n", __func__);
				}
				if (charger->device_event & BATT_EXT_EVENT_CALL) {
					union power_supply_propval value2;
					pr_info("%s: enter PHM \n", __func__);
					/* notify "wireless" PHM status */
					value2.intval = 1;
					psy_do_property("wireless", set, POWER_SUPPLY_EXT_PROP_CALL_EVENT, value2);
					mfc_send_command(charger, MFC_PHM_ON);
					msleep(250);
					mfc_send_command(charger, MFC_PHM_ON);
				}
				break;
			case TX_ID_UNO_TX:
			case TX_ID_UNO_TX_B0 ... TX_ID_UNO_TX_MAX:
				charger->pdata->cable_type = value.intval = SEC_WIRELESS_PAD_TX;
				pr_info("@Tx_Mode %s: TX by UNO\n", __func__);
				if (charger->device_event & BATT_EXT_EVENT_CALL) {
					pr_info("%s: enter PHM \n", __func__);
					mfc_send_command(charger, MFC_PHM_ON);
					msleep(250);
					mfc_send_command(charger, MFC_PHM_ON);
				}
				break;
			case TX_ID_NON_AUTH_PAD ... TX_ID_NON_AUTH_PAD_END:
				value.intval = charger->pdata->cable_type;
				if (charger->pdata->wpc_vout_ctrl_lcd_on) {
					pr_info("%s: tx id = 0x%x , set op freq\n", __func__, val_data);
					mfc_send_command(charger, MFC_SET_OP_FREQ);
					msleep(500);
				}
				break;
			case TX_ID_AUTH_PAD ... TX_ID_AUTH_PAD_END:
				if (charger->tx_id == TX_ID_AUTH_PAD) {
					/* TX_ID_AUTH_PAD : CMA and CMB all enable */
					mfc_reg_write(charger->client, MFC_RX_COMM_MOD_AFC_FET_REG, CMA_ENABLE_CMB_ENABLE_DATA);
					mfc_reg_write(charger->client, MFC_RX_COMM_MOD_FET_REG, CMA_ENABLE_CMB_ENABLE_DATA);
					pr_info("%s: tx_id(0x%x), enable CMA and CMB \n", __func__, charger->tx_id);
				}
				charger->vout_by_txid = mfc_get_wireless20_vout_by_txid(charger, val_data);
				charger->vrect_by_txid = mfc_get_wireless20_vrect_by_txid(charger, val_data);
				charger->max_power_by_txid = mfc_get_wireless20_max_power_by_txid(charger, val_data);

				if (charger->pdata->wpc_vout_ctrl_lcd_on &&
					(val_data >= TX_ID_DAVINCI_PAD_H && val_data <= TX_ID_AUTH_PAD_ACLASS_END)) {
					pr_info("%s: tx id = 0x%x , set op freq\n", __func__, val_data);
					mfc_send_command(charger, MFC_SET_OP_FREQ);
					msleep(500);
				}
#if !defined(CONFIG_SEC_FACTORY)
				/* do not process during lpm and wired charging */
				if (!lpcharge) {
					if (charger->adt_transfer_status == WIRELESS_AUTH_WAIT) {
						if (delayed_work_pending(&charger->wpc_afc_vout_work)) {
							__pm_relax(charger->wpc_afc_vout_ws);
							cancel_delayed_work(&charger->wpc_afc_vout_work);
						}
						charger->adt_transfer_status = WIRELESS_AUTH_START;
						/* notify auth service to send TX PAD a request key */
						value.intval = WIRELESS_AUTH_START;
						psy_do_property("wireless", set,
							POWER_SUPPLY_EXT_PROP_WIRELESS_AUTH_ADT_STATUS, value);

						charger->pdata->cable_type = value.intval = SEC_WIRELESS_PAD_WPC_PREPARE_HV_20;
						pr_info("%s %s: AUTH PAD for WIRELESS 2.0 : 0x%x\n", WC_AUTH_MSG,
								__func__, val_data);
					} else {
						value.intval = charger->pdata->cable_type;
						pr_info("%s %s: undefined auth TX-ID scenario, auth service works strange...\n",
								WC_AUTH_MSG, __func__);
					}
				} else {
					value.intval = charger->pdata->cable_type;
					pr_info("%s %s: It is AUTH PAD for WIRELESS 2.0 but LPM, %d\n",
							WC_AUTH_MSG, __func__, charger->pdata->cable_type);
				}
#else /* FACTORY MODE dose not process auth, set 12W right after */
				if (charger->adt_transfer_status == WIRELESS_AUTH_WAIT) {
					if (delayed_work_pending(&charger->wpc_afc_vout_work)) {
						__pm_relax(charger->wpc_afc_vout_ws);
						cancel_delayed_work(&charger->wpc_afc_vout_work);
					}
					charger->adt_transfer_status = WIRELESS_AUTH_PASS;
					charger->pdata->cable_type = value.intval = SEC_WIRELESS_PAD_WPC_HV_20;
					__pm_stay_awake(charger->wpc_afc_vout_ws);
					queue_delayed_work(charger->wqueue, &charger->wpc_afc_vout_work, msecs_to_jiffies(0));
				}
				pr_info("%s %s: AUTH PAD for WIRELESS 2.0 but FACTORY MODE\n", WC_AUTH_MSG, __func__);
#endif
				break;
			case TX_ID_DREAM_STAND:
				value.intval = charger->pdata->cable_type;
				pr_info("%s: Dream PAD STAND : 0x%x\n", __func__, val_data);
				break;
			case TX_ID_DREAM_DOWN:
				value.intval = charger->pdata->cable_type;
				pr_info("%s: Dream PAD DOWN : 0x%x\n", __func__, val_data);
				break;
			default:
				value.intval = charger->pdata->cable_type;
				pr_info("%s: UNDEFINED PAD : 0x%x\n", __func__, val_data);
				break;
			}

			if (value.intval == 0) {
				pr_info("%s: cable type is null plz check out !!!\n", __func__);
				value.intval = charger->pdata->cable_type;
			}

			if (value.intval != SEC_WIRELESS_PAD_PREPARE_HV &&
				value.intval != SEC_WIRELESS_PAD_WPC_HV_20) {
				pr_info("%s: change cable type\n", __func__);
				psy_do_property("wireless", set, POWER_SUPPLY_PROP_ONLINE, value);
			}

			/* send rx power informaion of this pad such as SEC_WIRELESS_RX_POWER_12W, SEC_WIRELESS_RX_POWER_17_5W */
			value.intval = charger->max_power_by_txid;
			psy_do_property("wireless", set, POWER_SUPPLY_EXT_PROP_WIRELESS_RX_POWER, value);

			/* send max vout informaion of this pad such as WIRELESS_VOUT_10V, WIRELESS_VOUT_12_5V */
			value.intval = charger->vout_by_txid;
			psy_do_property("wireless", set, POWER_SUPPLY_EXT_PROP_WIRELESS_MAX_VOUT, value);

#if defined(CONFIG_SEC_FACTORY)
			queue_delayed_work(charger->wqueue, &charger->wpc_rx_power_work, msecs_to_jiffies(3000));
#endif
			charger->tx_id_done = true;
			pr_info("%s: TX_ID : 0x%x\n", __func__, val_data);
		} else {
			pr_err("%s: TX ID isr is already done\n", __func__);
		}
	} else if (cmd_data == WPC_TX_COM_CHG_ERR) {
		switch (val_data) {
		case TX_CHG_ERR_OTP:
		case TX_CHG_ERR_OCP:
		case TX_CHG_ERR_DARKZONE:
			pr_info("%s: Received CHG error from the TX(0x%x)\n", __func__, val_data);
			break;
		case TX_CHG_ERR_FOD:
			pr_info("%s: Received FOD state from the TX(0x%x)\n", __func__, val_data);
			value.intval = val_data;
			psy_do_property("wireless", set, POWER_SUPPLY_EXT_PROP_WIRELESS_ERR, value);
			break;
		default:
			pr_info("%s: Undefined Type(0x%x)\n", __func__, val_data);
			break;
		}
	} else if (cmd_data == WPC_TX_COM_RX_POWER) {
		if (charger->current_rx_power != val_data) {
			switch (val_data) {
			case TX_RX_POWER_3W:
				pr_info("%s %s: RX Power is 3W\n", WC_AUTH_MSG, __func__);
				charger->current_rx_power = TX_RX_POWER_3W;
				break;
			case TX_RX_POWER_5W:
				pr_info("%s %s: RX Power is 5W\n", WC_AUTH_MSG, __func__);
				charger->current_rx_power = TX_RX_POWER_5W;
				break;
			case TX_RX_POWER_6_5W:
				pr_info("%s %s: RX Power is 6.5W\n", WC_AUTH_MSG, __func__);
				charger->current_rx_power = TX_RX_POWER_6_5W;
				break;
			case TX_RX_POWER_7_5W:
				pr_info("%s %s: RX Power is 7.5W\n", WC_AUTH_MSG, __func__);
				mfc_reset_rx_power(charger, TX_RX_POWER_7_5W);
				charger->current_rx_power = TX_RX_POWER_7_5W;
				break;
			case TX_RX_POWER_10W:
				pr_info("%s %s: RX Power is 10W\n", WC_AUTH_MSG, __func__);
				charger->current_rx_power = TX_RX_POWER_10W;
				break;
			case TX_RX_POWER_12W:
				pr_info("%s %s: RX Power is 12W\n", WC_AUTH_MSG, __func__);
				mfc_reset_rx_power(charger, TX_RX_POWER_12W);
				charger->current_rx_power = TX_RX_POWER_12W;
				break;
			case TX_RX_POWER_15W:
				pr_info("%s %s: RX Power is 15W\n", WC_AUTH_MSG, __func__);
				mfc_reset_rx_power(charger, TX_RX_POWER_15W);
				charger->current_rx_power = TX_RX_POWER_15W;
				break;
			case TX_RX_POWER_17_5W:
				pr_info("%s %s: RX Power is 17.5W\n", WC_AUTH_MSG, __func__);
				mfc_reset_rx_power(charger, TX_RX_POWER_17_5W);
				charger->current_rx_power = TX_RX_POWER_17_5W;
				break;
			case TX_RX_POWER_20W:
				pr_info("%s %s: RX Power is 20W\n", WC_AUTH_MSG, __func__);
				mfc_reset_rx_power(charger, TX_RX_POWER_20W);
				charger->current_rx_power = TX_RX_POWER_20W;
				break;
			default:
				pr_info("%s %s: Undefined RX Power(0x%x)\n", WC_AUTH_MSG, __func__, val_data);
				break;
			}
		} else {
			pr_info("%s: skip same RX Power\n", __func__);
		}
	} else if (cmd_data == WPC_TX_COM_WPS) {
		switch (val_data) {
		case WPS_AICL_RESET:
			value.intval = 1;
			pr_info("@Tx_mode %s: Rx devic AICL Reset\n", __func__);
			psy_do_property("wireless", set, POWER_SUPPLY_PROP_CURRENT_MAX, value);
			break;
		default:
			pr_info("%s %s: Undefined RX Power(0x%x)\n", WC_AUTH_MSG, __func__, val_data);
			break;
		}
	}
	__pm_relax(charger->wpc_rx_ws);
}

static void mfc_wpc_tx_id_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_tx_id_work.work);

	if (!charger->tx_id && (!charger->is_abnormal_pad || charger->tx_id_cnt < TX_ID_CHECK_CNT)) {
		if (charger->tx_id_cnt < 10) {
			mfc_send_command(charger, MFC_REQUEST_TX_ID);
			charger->req_tx_id = true;
			charger->tx_id_cnt++;

			pr_info("%s: request TX ID (%d)\n", __func__, charger->tx_id_cnt);
			queue_delayed_work(charger->wqueue, &charger->wpc_tx_id_work, msecs_to_jiffies(1500));
			return;
		}
		pr_info("%s: TX ID not Received, cable_type(%d)\n",
			__func__, charger->pdata->cable_type);
		if (is_hv_wireless_pad_type(charger->pdata->cable_type)) {
			/* SET OV 13V */
			mfc_reg_write(charger->client, MFC_RX_OV_CLAMP_REG, OV_CLAMP_13V_DATA);
		} else {
			/* SET OV 11V */
			mfc_reg_write(charger->client, MFC_RX_OV_CLAMP_REG, OV_CLAMP_11V_DATA);
		}
		charger->req_tx_id = false;
	} else {
		pr_info("%s: TX ID (0x%x)\n", __func__, charger->tx_id);
	}

	__pm_relax(charger->wpc_tx_id_ws);
}

static void mfc_vrect_check_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_vrect_check_work.work);

	int vrect = 0;
	u8 irq_src[2];
	union power_supply_propval value = {0, };
	vrect = mfc_get_adc(charger, MFC_ADC_VRECT);
	pr_info("%s: vrect: %dmV\n", __func__, vrect);

	if (!charger->initial_vrect) {
		if (charger->pdata->cable_type == SEC_WIRELESS_PAD_NONE) {
			/* Attach case */
			charger->pdata->cable_type = value.intval = SEC_WIRELESS_PAD_FAKE;
			psy_do_property("wireless", set, POWER_SUPPLY_PROP_ONLINE, value);

			/* To make sure forced detach if VOUT_GD is not rising within 3 seconds */
			charger->initial_vrect = true;
			queue_delayed_work(charger->wqueue, &charger->wpc_vrect_check_work, msecs_to_jiffies(3000));
			return;
		} else if (vrect >= 2700 && !charger->is_mst_on) {
			if (charger->pdata->cable_type == SEC_WIRELESS_PAD_FAKE) {
				pr_info("%s: Temporary UVLO, FAKE. check again.\n", __func__);
				queue_delayed_work(charger->wqueue, &charger->wpc_vrect_check_work, msecs_to_jiffies(300));
			} else {
				pr_info("%s: Temporary UVLO, hold on wireless charging\n", __func__);
				__pm_relax(charger->wpc_vrect_check_ws);
			}
			return;
		}
	}

	/* Detach case */
	pr_info("%s: Vrect IRQ! wireless charging pad was removed!!\n", __func__);
	/* clear intterupt */
	if (charger->pdata->cable_type == SEC_WIRELESS_PAD_FAKE &&
		!gpio_get_value(charger->pdata->wpc_int)) {
		charger->pdata->cable_type = SEC_WIRELESS_PAD_NONE;
		mfc_reg_read(charger->client, MFC_INT_A_L_REG, &irq_src[0]);
		mfc_reg_read(charger->client, MFC_INT_A_H_REG, &irq_src[1]);
		mfc_reg_write(charger->client, MFC_INT_A_CLEAR_L_REG, irq_src[0]); // clear int
		mfc_reg_write(charger->client, MFC_INT_A_CLEAR_H_REG, irq_src[1]); // clear int
		mfc_set_cmd_l_reg(charger, MFC_CMD_CLEAR_INT_MASK, MFC_CMD_CLEAR_INT_MASK); // command
		pr_info("%s wc_w_state_irq = %d\n", __func__, gpio_get_value(charger->pdata->wpc_int));
	}
	charger->pdata->cable_type = SEC_WIRELESS_PAD_NONE;
	charger->initial_vrect = false;
	value.intval = SEC_WIRELESS_PAD_NONE;
	psy_do_property("wireless", set, POWER_SUPPLY_PROP_ONLINE, value);

	__pm_relax(charger->wpc_vrect_check_ws);
}

static irqreturn_t mfc_wpc_det_irq_thread(int irq, void *irq_data)
{
	struct mfc_charger_data *charger = irq_data;
	u8 irq_src[2];

	pr_info("%s\n", __func__);

	if (charger->is_probed) {
		queue_delayed_work(charger->wqueue, &charger->wpc_det_work, 0);
		/* clear intterupt */
		if (charger->pdata->cable_type == SEC_WIRELESS_PAD_FAKE) {
			mfc_reg_read(charger->client, MFC_INT_A_L_REG, &irq_src[0]);
			mfc_reg_read(charger->client, MFC_INT_A_H_REG, &irq_src[1]);
			mfc_reg_write(charger->client, MFC_INT_A_CLEAR_L_REG, irq_src[0]); // clear int
			mfc_reg_write(charger->client, MFC_INT_A_CLEAR_H_REG, irq_src[1]); // clear int
			mfc_set_cmd_l_reg(charger, MFC_CMD_CLEAR_INT_MASK, MFC_CMD_CLEAR_INT_MASK); // command
			pr_info("%s: wc_w_state_irq = %d\n", __func__, gpio_get_value(charger->pdata->wpc_int));
		}
	} else {
		pr_info("%s: prevent work thread before device is probed.\n", __func__);
	}

	return IRQ_HANDLED;
}

/* mfc_mst_routine : MST dedicated codes */
static void mfc_mst_routine(struct mfc_charger_data *charger, u8 irq_src_l, u8 irq_src_h)
{
	u8 data = 0;

	pr_info("%s\n", __func__);

	if (charger->is_mst_on == MST_MODE_2) {
		charger->wc_tx_enable = false;
#if defined(CONFIG_MST_V2)
		// it will send MST driver a message.
		mfc_send_mst_cmd(1, charger, irq_src_l, irq_src_h);
#endif
	} else if (charger->wc_tx_enable) {
		mfc_reg_read(charger->client, MFC_STATUS_H_REG, &data);
		data &= MFC_STAT_H_AC_MISSING_DET_MASK;
		msleep(100);
		pr_info("@Tx_Mode %s: 0x21 Register AC Missing(%d)\n", __func__, data);
		if (data) {
			mfc_reg_write(charger->client, MFC_MST_MODE_SEL_REG, MST_OFF_TX_ON_DATA); /* set TX-ON mode */
			pr_info("@Tx_Mode %s: TX-ON Mode : %d\n", __func__, charger->pdata->wc_ic_rev);
			if (charger->wc_rx_connected) {
				pr_info("@Tx_Mode %s: AC Missing: make Rx disconnected!\n", __func__);
				charger->wc_tx_ac_missing = true;
				charger->wc_rx_connected = false;
				__pm_stay_awake(charger->wpc_rx_connection_ws);
				queue_delayed_work(charger->wqueue, &charger->wpc_rx_connection_work, 0);
			}
		} //ac missing is 0, ie, TX detected
	}

#if !IS_ENABLED(CONFIG_MST_V2)
	mfc_set_for_mst_removal(charger);
#endif
}

static void mfc_check_sys_op_mode(struct mfc_charger_data *charger, u8 mode)
{
#if defined(CONFIG_TX_GEAR_PHM_VOUT_CTRL) || defined(CONFIG_TX_GEAR_AOV)
	union power_supply_propval value = {0, };
	u8 data = 0;
#endif

	mode &= 0xF;

	if (mode == MFC_TX_MODE_TX_PWR_HOLD) {
		if (charger->wc_rx_type == SS_GEAR) {
			/* start 3min alarm timer */
			pr_info("@Tx_Mode %s: Start PHM alarm by 3min\n", __func__);
			alarm_start(&charger->phm_alarm,
					ktime_add(ktime_get_boottime(), ktime_set(180, 0)));

#if defined(CONFIG_TX_GEAR_PHM_VOUT_CTRL) || defined(CONFIG_TX_GEAR_AOV)
			mfc_reg_update(charger->client, MFC_INT_A_ENABLE_H_REG,
				0, MFC_STAT_H_TX_CON_DISCON_MASK);
			mfc_reg_read(charger->client, MFC_INT_A_ENABLE_H_REG, &data);

			charger->tx_gear_phm = 1;
			value.intval = 1;
			psy_do_property("wireless", set,
				POWER_SUPPLY_EXT_PROP_GEAR_PHM_EVENT, value);
#endif
		} else {
			pr_info("%s: TX entered PHM but no 3min timer\n", __func__);
		}
	} else {
#if defined(CONFIG_TX_GEAR_PHM_VOUT_CTRL) || defined(CONFIG_TX_GEAR_AOV)
		if (charger->tx_gear_phm) {
			charger->tx_gear_phm = 0;
			value.intval = 0;
			psy_do_property("wireless", set,
				POWER_SUPPLY_EXT_PROP_GEAR_PHM_EVENT, value);
		}
#endif
		if (charger->phm_alarm.state & ALARMTIMER_STATE_ENQUEUED) {
			pr_info("@Tx_Mode %s: escape PHM mode, cancel PHM alarm\n", __func__);
			cancel_delayed_work(&charger->wpc_tx_phm_work);
			__pm_relax(charger->wpc_tx_phm_ws);
			alarm_cancel(&charger->phm_alarm);
#if defined(CONFIG_TX_GEAR_PHM_VOUT_CTRL) || defined(CONFIG_TX_GEAR_AOV)
			msleep(400);
			mfc_reg_update(charger->client, MFC_INT_A_ENABLE_H_REG,
				MFC_STAT_H_TX_CON_DISCON_MASK, MFC_STAT_H_TX_CON_DISCON_MASK);
			mfc_reg_read(charger->client, MFC_INT_A_ENABLE_H_REG, &data);
			pr_info("@Tx_Mode %s MFC_INT_A_ENABLE_H_REG(0x25)= 0x%x\n", __func__, data);
			mfc_reg_read(charger->client, MFC_STATUS_H_REG, &data);
			pr_info("@Tx_Mode %s MFC_STATUS_H_REG(0x21)= 0x%x\n", __func__, data);
			if (!(data & MFC_STAT_H_TX_CON_DISCON_MASK)) {
				/* determine rx connection status with tx sharing mode */
				if (!charger->wc_rx_connected) {
					pr_info("@Tx_Mode %s Ignore IRQ!! already Rx disconnected!\n", __func__);
				} else {
					charger->wc_rx_connected = false;
					__pm_stay_awake(charger->wpc_rx_connection_ws);
					queue_delayed_work(charger->wqueue,
						&charger->wpc_rx_connection_work, 0);
				}
			}
#endif
		}
	}
}

static int mfc_clear_irq(struct mfc_charger_data *charger, u8 *src_l, u8 *src_h, u8 *status_l, u8 *status_h)
{
	int wc_w_state_irq = 0;
	u8 new_src_l = 0, new_src_h = 0;
	u8 new_status_l = 0, new_status_h = 0;
	int ret = 0, rerun_ret = 1;

	pr_info("%s: start\n", __func__);

	ret = mfc_reg_write(charger->client, MFC_INT_A_CLEAR_L_REG, *src_l); // clear int
	ret = mfc_reg_write(charger->client, MFC_INT_A_CLEAR_H_REG, *src_h); // clear int
	mfc_set_cmd_l_reg(charger, MFC_CMD_CLEAR_INT_MASK, MFC_CMD_CLEAR_INT_MASK); // command

	//msleep(2);
	wc_w_state_irq = gpio_get_value(charger->pdata->wpc_int);
	pr_info("%s: wc_w_state_irq = %d\n", __func__, wc_w_state_irq);

	if (!wc_w_state_irq && (ret >= 0)) {
		pr_info("%s: wc_w_state_irq is not cleared\n", __func__);
		ret = mfc_reg_read(charger->client, MFC_INT_A_L_REG, &new_src_l);
		ret = mfc_reg_read(charger->client, MFC_INT_A_H_REG, &new_src_h);

		ret = mfc_reg_read(charger->client, MFC_STATUS_L_REG, &new_status_l);
		ret = mfc_reg_read(charger->client, MFC_STATUS_H_REG, &new_status_h);

		pr_info("%s: src_l[0x%x -> 0x%x], src_h[0x%x -> 0x%x], status_l[0x%x -> 0x%x], status_h[0x%x -> 0x%x]\n",
			__func__, *src_l, new_src_l, *src_h, new_src_h, *status_l, new_status_l, *status_h, new_status_h);

		rerun_ret = 0; // re-run isr

		/* do not try irq again with i2c fail status, need to end up the irq */
		if ((*src_l != new_src_l ||
			*src_h != new_src_h ||
			*status_l != new_status_l ||
			*status_h != new_status_h) &&
			(ret >= 0)) {
			*src_l = new_src_l;
			*src_h = new_src_h;
			*status_l = new_status_l;
			*status_h = new_status_h;
			pr_info("%s: re-run isr\n", __func__);
		} else if (ret < 0) {
			rerun_ret = 1; // do not re-run isr
			pr_info("%s: i2c fail, do not re-run isr\n", __func__);
		} else {
			pr_info("%s: re-run isr with same src, status\n", __func__);
		}
	}

	pr_info("%s: end (%d)\n", __func__, rerun_ret);
	return rerun_ret;
}

static irqreturn_t mfc_wpc_irq_thread(int irq, void *irq_data)
{
	struct mfc_charger_data *charger = irq_data;
	int wc_w_state_irq = 0;
	int ret = 0;
	u8 irq_src_l = 0, irq_src_h = 0;
	u8 status_l = 0, status_h = 0;
	u8 reg_data = 0;
	int isr_rerun_cnt = 0;
	bool end_irq = false;
	bool clear_irq = true;
	union power_supply_propval value;

	pr_info("%s: start!\n", __func__);

	wc_w_state_irq = gpio_get_value(charger->pdata->wpc_int);
	pr_info("%s: wc_w_state_irq = %d\n", __func__, wc_w_state_irq);

	if (wc_w_state_irq == 1 &&
		charger->pdata->cable_type == SEC_WIRELESS_PAD_FAKE) {
		pr_info("%s: Check vrect after 0.9 second to prevent reconnection by UVLO or Watch ping\n", __func__);
		if (delayed_work_pending(&charger->wpc_vrect_check_work)) {
			cancel_delayed_work(&charger->wpc_vrect_check_work);
			__pm_relax(charger->wpc_vrect_check_ws);
		}
		charger->initial_vrect = false;
		__pm_stay_awake(charger->wpc_vrect_check_ws);
		queue_delayed_work(charger->wqueue, &charger->wpc_vrect_check_work, msecs_to_jiffies(900));
		return IRQ_HANDLED;
	} else if (wc_w_state_irq == 1) {
		pr_info("%s: Rising edge, End up ISR\n", __func__);
		return IRQ_HANDLED;
	}

	__pm_stay_awake(charger->wpc_ws);

	ret = mfc_reg_read(charger->client, MFC_INT_A_L_REG, &irq_src_l);
	if (ret < 0) {
		pr_err("%s: Failed to read INT_A_L_REG: %d\n", __func__, ret);
		__pm_relax(charger->wpc_ws);
		return IRQ_HANDLED;
	}

	ret = mfc_reg_read(charger->client, MFC_INT_A_H_REG, &irq_src_h);
	if (ret < 0) {
		pr_err("%s: Failed to read INT_A_H_REG: %d\n", __func__, ret);
		__pm_relax(charger->wpc_ws);
		return IRQ_HANDLED;
	}

	ret = mfc_reg_read(charger->client, MFC_STATUS_L_REG, &status_l);
	if (ret < 0)
		pr_err("%s: Failed to read STATUS_L_REG: %d\n", __func__, ret);

	ret = mfc_reg_read(charger->client, MFC_STATUS_H_REG, &status_h);
	if (ret < 0)
		pr_err("%s: Failed to read STATUS_H_REG: %d\n", __func__, ret);


INT_RE:
	isr_rerun_cnt++;
	pr_info("%s: interrupt source(0x%x), status(0x%x) \n", __func__, irq_src_h << 8 | irq_src_l, status_h << 8 | status_l);

	if (irq_src_h & MFC_INTA_H_AC_MISSING_DET_MASK) {
		pr_info("%s: 1AC Missing ! : MFC is on\n", __func__);
		mfc_mst_routine(charger, irq_src_l, irq_src_h);
	}

	if (irq_src_l & MFC_INTA_L_OP_MODE_MASK) {
		pr_info("%s: MODE CHANGE IRQ !\n", __func__);
		if (charger->wc_tx_enable) {
			ret = mfc_reg_read(charger->client, MFC_SYS_OP_MODE_REG, &reg_data);
			mfc_check_sys_op_mode(charger, reg_data);
		}
	}

	if ((irq_src_l & MFC_INTA_L_OVER_VOL_MASK) || (irq_src_l & MFC_INTA_L_OVER_CURR_MASK))
		pr_info("%s: ABNORMAL STAT IRQ !\n", __func__);

	if (irq_src_l & MFC_INTA_L_OVER_TEMP_MASK) {
		pr_info("%s: OVER TEMP IRQ !\n", __func__);
		if (charger->wc_tx_enable) {
			value.intval = BATT_TX_EVENT_WIRELESS_RX_UNSAFE_TEMP;
			end_irq = true;
			goto INT_END;
		}
	}

	if (irq_src_l & MFC_INTA_L_STAT_VRECT_MASK) {
		pr_info("%s: Vrect IRQ !\n", __func__);
		if (charger->is_mst_on == MST_MODE_2 || charger->wc_tx_enable) {
			pr_info("%s: Noise made false Vrect IRQ!\n", __func__);
			if (charger->wc_tx_enable) {
				value.intval = BATT_TX_EVENT_WIRELESS_TX_ETC;
				end_irq = true;
				goto INT_END;
			}
		} else if (charger->pdata->cable_type == SEC_WIRELESS_PAD_NONE ||
				charger->pdata->cable_type == SEC_WIRELESS_PAD_FAKE) {
			int delay = (charger->test & (1 << MFC_TEST_VRECT_IRQ_DELAY)) ? 1000 : 0;

			clear_irq = (charger->pdata->cable_type != SEC_WIRELESS_PAD_NONE);
			__pm_stay_awake(charger->wpc_vrect_check_ws);
			queue_delayed_work(charger->wqueue, &charger->wpc_vrect_check_work, msecs_to_jiffies(delay));
		} else
			pr_info("%s: undefined Vrect IRQ scenario!\n", __func__);
	}

	if (irq_src_l & MFC_INTA_L_TXCONFLICT_MASK) {
		pr_info("@Tx_Mode %s: TXCONFLICT IRQ !\n", __func__);
		if (charger->wc_tx_enable && (status_l & MFC_STAT_L_TXCONFLICT_MASK)) {
			value.intval = BATT_TX_EVENT_WIRELESS_TX_ETC;
			end_irq = true;
			goto INT_END;
		}
	}

	if (irq_src_h & MFC_INTA_H_TRX_DATA_RECEIVED_MASK) {
		pr_info("%s: TX RECEIVED IRQ !\n", __func__);
		if (charger->wc_tx_enable && !delayed_work_pending(&charger->wpc_tx_isr_work)) {
			__pm_stay_awake(charger->wpc_tx_ws);
			queue_delayed_work(charger->wqueue, &charger->wpc_tx_isr_work, msecs_to_jiffies(1000));
		}
		else if (charger->pdata->cable_type == SEC_WIRELESS_PAD_WPC_STAND ||
			charger->pdata->cable_type == SEC_WIRELESS_PAD_WPC_STAND_HV)
			pr_info("%s: Don't run ISR_WORK for NO ACK !\n", __func__);
		else if (!delayed_work_pending(&charger->wpc_isr_work)) {
			__pm_stay_awake(charger->wpc_rx_ws);
			queue_delayed_work(charger->wqueue, &charger->wpc_isr_work, msecs_to_jiffies(500));
		}
	}

	if (irq_src_h & MFC_INTA_H_TX_CON_DISCON_MASK) {
		pr_info("@Tx_Mode %s: TX CONNECT IRQ !\n", __func__);
		charger->tx_status = SEC_TX_POWER_TRANSFER;
		if (status_h & MFC_STAT_H_TX_CON_DISCON_MASK) {
			/* determine rx connection status with tx sharing mode */
			if (!charger->wc_rx_connected) {
				charger->wc_rx_connected = true;
				__pm_stay_awake(charger->wpc_rx_connection_ws);
				queue_delayed_work(charger->wqueue, &charger->wpc_rx_connection_work, 0);
			}
		} else {
			/* determine rx connection status with tx sharing mode */
			if (!charger->wc_rx_connected) {
				pr_info("@Tx_Mode %s: Ignore IRQ!! already Rx disconnected!\n", __func__);
			} else {
				charger->wc_rx_connected = false;
				__pm_stay_awake(charger->wpc_rx_connection_ws);
				queue_delayed_work(charger->wqueue, &charger->wpc_rx_connection_work, 0);
			}
		}
	}

	/* when rx is not detacted in 3 mins */
	if (irq_src_h & MFC_INTA_H_TX_MODE_RX_NOT_DET_MASK) {
		if (!(irq_src_h & MFC_INTA_H_TX_CON_DISCON_MASK)) {
			pr_info("@Tx_Mode %s: Receiver NOT DETECTED IRQ !\n", __func__);
			if (charger->wc_tx_enable) {
				value.intval = BATT_TX_EVENT_WIRELESS_TX_ETC;
				end_irq = true;
				goto INT_END;
			}
		}
	}

	if (irq_src_h & MFC_STAT_H_ADT_RECEIVED_MASK) {
		pr_info("%s %s: ADT Received IRQ !\n", WC_AUTH_MSG, __func__);
		mfc_auth_adt_read(charger, ADT_buffer_rdata);
	}

	if (irq_src_h & MFC_STAT_H_ADT_SENT_MASK) {
		pr_info("%s %s: ADT Sent successful IRQ !\n", WC_AUTH_MSG, __func__);
		mfc_reg_update(charger->client, MFC_INT_A_ENABLE_H_REG, 0, MFC_STAT_H_ADT_SENT_MASK);
	}

	if (irq_src_h & MFC_INTA_H_TX_FOD_MASK) {
		pr_info("%s: TX FOD IRQ !\n", __func__);
		// this code will move to wpc_tx_isr_work
		if (charger->wc_tx_enable) {
			if (status_h & MFC_STAT_H_TX_FOD_MASK) {
				charger->wc_rx_fod = true;
				value.intval = BATT_TX_EVENT_WIRELESS_TX_FOD;
				end_irq = true;
				goto INT_END;
			}
		}
	}

	if (irq_src_h & MFC_INTA_H_TX_OCP_MASK) {
		pr_info("%s: TX Over Current IRQ !\n", __func__);
	}

INT_END:
	if (clear_irq) {
		/* clear intterupt */
		if (!mfc_clear_irq(charger, &irq_src_l, &irq_src_h, &status_l, &status_h) && isr_rerun_cnt < ISR_CNT)
			goto INT_RE;
	} else {
		pr_info("%s: Vrect IRQ - skip clear INT_A\n", __func__);
	}

	/* tx off should work having done i2c */
	if (end_irq)
		psy_do_property("wireless", set, POWER_SUPPLY_EXT_PROP_WIRELESS_TX_ERR, value);

	pr_info("%s: end!\n", __func__);

	__pm_relax(charger->wpc_ws);

	return IRQ_HANDLED;
}

static void mfc_chg_parse_fod_data(struct device_node *np,
		mfc_charger_platform_data_t *pdata)
{
	const char* fod_state_string[FOD_STATE_MAX] = {"cc", "cv", "full"};
	struct device_node *child = NULL;
	int ret = 0, idx = 0, i = 0;

	np = of_find_node_by_name(np, "fod_list");
	if (!np) {
		pr_err("%s: fod list is NULL!\n", __func__);
		return;
	}

	ret = of_property_read_u32(np, "count", &pdata->fod_data_count);
	if (ret < 0) {
		pr_err("%s: fod data size is NULL!\n", __func__);
		return;
	}

	pdata->fod_list = kzalloc(sizeof(mfc_fod_data) * pdata->fod_data_count, GFP_KERNEL);
	if (pdata->fod_list == NULL) {
		pr_err("%s: failed to alloc memory(%d)\n", __func__, pdata->fod_data_count);
		pdata->fod_data_count = 0;
		return;
	}

	for_each_child_of_node(np, child) {
		mfc_fod_data* pfod_data = NULL;

		if (idx >= pdata->fod_data_count) {
			pr_err("%s: skip set fod data because of overflow\n", __func__);
			break;
		}
		pfod_data = &pdata->fod_list[idx++];

		if (sscanf(child->name, "pad_0x%X", &pfod_data->pad_id) < 0) {
			pr_err("%s: failed to get pad id of %s\n", __func__, child->name);
			continue;
		}

		ret = of_property_read_u32(child, "flag", &pfod_data->flag);
		if (ret < 0) {
			pr_err("%s: failed to get flag of %s\n", __func__, child->name);
			continue;
		}

		for (i = 0; i < FOD_STATE_MAX; i++) {
			const u32 *p;
			u32* pfod = NULL;
			int len = 0;

			switch ((pfod_data->flag >> (i * 4)) & 0xF) {
			case FOD_FLAG_ADD:
				p = of_get_property(child, fod_state_string[i], &len);
				if (!p) {
					pr_err("%s: %s - %s is null!!\n", __func__, child->name, fod_state_string[i]);
					break;
				}

				pfod = kzalloc(sizeof(u32) * MFC_NUM_FOD_REG, GFP_KERNEL);
				if (pfod == NULL) {
					pr_err("%s: failed to alloc fod data\n", __func__);
					break;
				}

				len = (len / sizeof(u32));
				ret = of_property_read_u32_array(child, fod_state_string[i], pfod, len);
				if (ret < 0) {
					pr_err("%s: failed to get fod data(%s - %s)\n",
							__func__, child->name, fod_state_string[i]);
					break;
				}
				pfod_data->data[i] = pfod;
				break;
			case FOD_FLAG_USE_CC:
				if (pfod_data->data[FOD_STATE_CC] == NULL) {
					pr_err("%s: failed to get cc fod data(%s - %s)\n",
							__func__, child->name, fod_state_string[i]);
					break;
				}
				pfod_data->data[i] = pfod_data->data[FOD_STATE_CC];
				break;
			case FOD_FLAG_USE_DEFAULT:
				if ((pdata->fod_list[DEFAULT_PAD_ID].pad_id != 0) ||
					(pdata->fod_list[DEFAULT_PAD_ID].data[i] == NULL)) {
					pr_err("%s: failed to get default fod data(%s - %s)\n",
							__func__, child->name, fod_state_string[i]);
					break;
				}
				pfod_data->data[i] = pdata->fod_list[DEFAULT_PAD_ID].data[i];
				break;
			case FOD_FLAG_NONE:
			default:
				pr_info("%s: %s - %s is not set.", __func__, child->name, fod_state_string[i]);
				break;
			}
		}
	}

	for (idx = 0; idx < pdata->fod_data_count; idx++) {
		mfc_fod_data* pfod_data = &pdata->fod_list[idx];

		pr_info("%s: PAD_ID = 0x%X, FLAG=0x%X\n", __func__, pfod_data->pad_id, pfod_data->flag);
		for (i = 0; i < FOD_STATE_MAX; i++) {
			if (pfod_data->data[i] != NULL) {
				char temp_buf[1024] = {0, };
				int j = 0, size = 1024;

				for (j = 0; j < MFC_NUM_FOD_REG; j++) {
					snprintf(temp_buf + strlen(temp_buf), size, "%d ", pfod_data->data[i][j]);
					size = sizeof(temp_buf) - strlen(temp_buf);
				}
				pr_info("%s: %s - %s\n", __func__, fod_state_string[i], temp_buf);
			} else {
				pr_info("%s: %s is null\n", __func__, fod_state_string[i]);
			}
		}
	}
}

#if defined(CONFIG_WIRELESS_IC_PARAM)
static void mfc_parse_param_value(struct mfc_charger_data *charger)
{
	charger->wireless_param_info = wireless_ic;
	charger->wireless_chip_id_param = (wireless_ic & 0xFF000000) >> 24;
	charger->wireless_fw_ver_param = (wireless_ic & 0x00FFFF00) >> 8;
	charger->wireless_fw_mode_param = (wireless_ic & 0x000000F0) >> 4;

	pr_info("%s: wireless_ic(0x%08X), chip_id(0x%02X), fw_ver(0x%04X), fw_mode(0x%01X)\n",
		__func__, wireless_ic, charger->wireless_chip_id_param,
		charger->wireless_fw_ver_param, charger->wireless_fw_mode_param);
}
#endif

static int mfc_chg_parse_dt(struct device *dev,
		mfc_charger_platform_data_t *pdata)
{
	int ret = 0;
	struct device_node *np = dev->of_node;
	enum of_gpio_flags irq_gpio_flags;
	int len, i;
	const u32 *p;

	if (!np) {
		pr_err("%s: np NULL\n", __func__);
		return 1;
	} else {
		ret = of_property_read_string(np,
			"battery,wireless_charger_name", (char const **)&pdata->wireless_charger_name);
		if (ret < 0)
			pr_info("%s: Wireless Charger name is Empty\n", __func__);

		ret = of_property_read_string(np,
			"battery,charger_name", (char const **)&pdata->wired_charger_name);
		if (ret < 0)
			pr_info("%s: Charger name is Empty\n", __func__);

		ret = of_property_read_string(np,
			"battery,fuelgauge_name", (char const **)&pdata->fuelgauge_name);
		if (ret < 0)
			pr_info("%s: Fuelgauge name is Empty\n", __func__);

		ret = of_property_read_u32(np, "battery,mst_switch_delay",
						&pdata->mst_switch_delay);
		if (ret < 0) {
			pr_info("%s: mst_switch_delay is Empty\n", __func__);
			pdata->mst_switch_delay = 1000; /* set default value (dream) */
		}

		ret = of_property_read_u32(np, "battery,wc_cover_rpp",
						&pdata->wc_cover_rpp);
		if (ret < 0) {
			pr_info("%s: fail to read wc_cover_rpp.\n", __func__);
			pdata->wc_cover_rpp = 0x55;
		}

		ret = of_property_read_u32(np, "battery,phone_fod_threshold",
						&pdata->phone_fod_threshold);
		if (ret < 0) {
			pr_info("%s: fail to read phone_fod_threshold\n", __func__);
			pdata->phone_fod_threshold = 0x50;
		}

		ret = of_property_read_u32(np, "battery,wc_hv_rpp",
						&pdata->wc_hv_rpp);
		if (ret < 0) {
			pr_info("%s: fail to read wc_hv_rpp.\n", __func__);
			pdata->wc_hv_rpp = 0x40;
		}

		ret = of_property_read_u32(np, "battery,oc_fod1",
						&pdata->oc_fod1);
		if (ret < 0) {
			pr_info("%s: fail to read oc_fod1\n", __func__);
			pdata->oc_fod1 = 900; /* IC default */
		}

		ret = of_property_read_u32(np, "battery,gear_ping_freq",
						&pdata->gear_ping_freq);
		if (ret < 0) {
			pr_info("%s: fail to read gear_ping_freq\n", __func__);
			pdata->gear_ping_freq = 0x96; /* IC default */
		}

		ret = of_property_read_u32(np, "battery,gear_fod1",
						&pdata->gear_fod1);
		if (ret < 0) {
			pr_info("%s: fail to read gear_fod1\n", __func__);
			pdata->gear_fod1 = 1300; /* default */
		}

		ret = of_property_read_u32(np, "battery,gear_fod2",
						&pdata->gear_fod2);
		if (ret < 0) {
			pr_info("%s: fail to read gear_fod2\n", __func__);
			pdata->gear_fod2 = 1000; /* default */
		}

#if defined(CONFIG_TX_GEAR_AOV)
		ret = of_property_read_u32(np, "battery,gear_aov_phm_alarm",
						&pdata->gear_aov_phm_alarm);
		if (ret < 0) {
			pr_info("%s: fail to read gear_aov_phm_alarm\n", __func__);
			pdata->gear_aov_phm_alarm = 100; /* default */
		}

		ret = of_property_read_u32(np, "battery,gear_aov_re_phm_delay",
						&pdata->gear_re_phm_delay);
		if (ret < 0) {
			pr_info("%s: fail to read gear_re_phm_delay\n", __func__);
			pdata->gear_re_phm_delay = 3000; /* default */
		}
#endif

		ret = of_property_read_u32(np, "battery,gear_min_op_freq",
						&pdata->gear_min_op_freq);
		if (ret < 0) {
			pr_info("%s: fail to read gear_min_op_freq\n", __func__);
			pdata->gear_min_op_freq = 125;
		}

		ret = of_property_read_u32(np, "battery,tx_gear_min_op_freq_delay",
						&pdata->gear_min_op_freq_delay);
		if (ret < 0) {
			pr_info("%s: fail to read gear_min_op_freq_delay\n", __func__);
			pdata->gear_min_op_freq_delay = 0;
		}

		/* wpc_det */
		ret = pdata->wpc_det = of_get_named_gpio_flags(np, "battery,wpc_det",
				0, &irq_gpio_flags);
		if (ret < 0) {
			dev_err(dev, "%s: can't get wpc_det\r\n", __FUNCTION__);
		} else {
			pdata->irq_wpc_det = gpio_to_irq(pdata->wpc_det);
			pr_info("%s: wpc_det = 0x%x, irq_wpc_det = 0x%x\n", __func__, pdata->wpc_det, pdata->irq_wpc_det);
		}
		/* wpc_int (This GPIO means MFC_AP_INT) */
		ret = pdata->wpc_int = of_get_named_gpio_flags(np, "battery,wpc_int",
				0, &irq_gpio_flags);
		if (ret < 0) {
			dev_err(dev, "%s: can't wpc_int\r\n", __FUNCTION__);
		} else {
			pdata->irq_wpc_int = gpio_to_irq(pdata->wpc_int);
			pr_info("%s: wpc_int = 0x%x, irq_wpc_int = 0x%x\n", __func__, pdata->wpc_int, pdata->irq_wpc_int);
		}

		/* mst_pwr_en (MST PWR EN) */
		ret = pdata->mst_pwr_en = of_get_named_gpio_flags(np, "battery,mst_pwr_en",
				0, &irq_gpio_flags);
		if (ret < 0)
			dev_err(dev, "%s: can't mst_pwr_en\r\n", __FUNCTION__);

		/* wpc_en (MFC EN) */
		ret = pdata->wpc_en = of_get_named_gpio_flags(np, "battery,wpc_en",
				0, &irq_gpio_flags);
		if (ret < 0)
			dev_err(dev, "%s: can't wpc_en\r\n", __FUNCTION__);

		p = of_get_property(np, "battery,wireless20_vout_list", &len);
		if (p) {
			len = len / sizeof(u32);
			pdata->len_wc20_list = len;
			pdata->wireless20_vout_list = kzalloc(sizeof(*pdata->wireless20_vout_list) * len, GFP_KERNEL);
			ret = of_property_read_u32_array(np, "battery,wireless20_vout_list", pdata->wireless20_vout_list, len);

			for (i = 0; i < len; i++)
				pr_info("%s: wireless20_vout_list = %d ", __func__, pdata->wireless20_vout_list[i]);
			pr_info("%s: len_wc20_list = %d ", __func__, pdata->len_wc20_list);
		} else {
			pr_err("%s: there is no wireless20_vout_list\n", __func__);
		}

		p = of_get_property(np, "battery,wireless20_vrect_list", &len);
		if (p) {
			len = len / sizeof(u32);
			pdata->wireless20_vrect_list = kzalloc(sizeof(*pdata->wireless20_vrect_list) * len, GFP_KERNEL);
			ret = of_property_read_u32_array(np, "battery,wireless20_vrect_list", pdata->wireless20_vrect_list, len);

			for (i = 0; i < len; i++)
				pr_info("%s: wireless20_vrect_list = %d ", __func__, pdata->wireless20_vrect_list[i]);
		} else {
			pr_err("%s: there is no wireless20_vrect_list\n", __func__);
		}

		p = of_get_property(np, "battery,wireless20_max_power_list", &len);
		if (p) {
			len = len / sizeof(u32);
			pdata->wireless20_max_power_list = kzalloc(sizeof(*pdata->wireless20_max_power_list) * len, GFP_KERNEL);
			ret = of_property_read_u32_array(np, "battery,wireless20_max_power_list",
					pdata->wireless20_max_power_list, len);

			for (i = 0; i < len; i++)
				pr_info("%s: wireless20_max_power_list = %d ", __func__, pdata->wireless20_max_power_list[i]);
		} else {
			pr_err("%s: there is no wireless20_max_power_list\n", __func__);
		}

		pdata->wpc_vout_ctrl_lcd_on = of_property_read_bool(np, "battery,wpc_vout_ctrl_lcd_on");
		if (pdata->wpc_vout_ctrl_lcd_on)
			pr_info("%s: flicker w/a\n", __func__);

		mfc_chg_parse_fod_data(np, pdata);
		return 0;
	}
}

static int mfc_create_attrs(struct device *dev)
{
	int i, rc;

	for (i = 0; i < (int)ARRAY_SIZE(mfc_attrs); i++) {
		rc = device_create_file(dev, &mfc_attrs[i]);
		if (rc)
			goto create_attrs_failed;
	}
	return rc;

create_attrs_failed:
	dev_err(dev, "%s: failed (%d)\n", __func__, rc);
	while (i--)
		device_remove_file(dev, &mfc_attrs[i]);
	return rc;
}

ssize_t mfc_show_attrs(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct mfc_charger_data *charger = power_supply_get_drvdata(psy);

	const ptrdiff_t offset = attr - mfc_attrs;
	int i = 0;

	dev_info(charger->dev, "%s\n", __func__);

	switch (offset) {
	case MFC_ADDR:
		i += scnprintf(buf + i, PAGE_SIZE - i, "0x%x\n", charger->addr);
		break;
	case MFC_SIZE:
		i += scnprintf(buf + i, PAGE_SIZE - i, "0x%x\n", charger->size);
		break;
	case MFC_DATA:
		if (charger->size == 0) {
			charger->size = 1;
		} else if (charger->size + charger->addr <= 0xFFFF) {
			u8 data;
			int j;

			for (j = 0; j < charger->size; j++) {
				if (mfc_reg_read(charger->client, charger->addr + j, &data) < 0) {
					dev_info(charger->dev, "%s: read fail\n", __func__);
					i += scnprintf(buf + i, PAGE_SIZE - i, "addr: 0x%x read fail\n",
						charger->addr + j);
					continue;
				}
				i += scnprintf(buf + i, PAGE_SIZE - i, "addr: 0x%x, data: 0x%x\n",
					charger->addr + j, data);
			}
		}
		break;
	case MFC_PACKET:
		break;
	case MFC_FLICKER_TEST:
		i += scnprintf(buf + i, PAGE_SIZE - i, "%d %d\n",
			charger->flicker_delay,
			charger->flicker_vout_threshold);
		break;
	case TEST:
		i += scnprintf(buf + i, PAGE_SIZE - i, "0x%x\n", charger->test);
		break;
	default:
		return -EINVAL;
	}
	return i;
}

ssize_t mfc_store_attrs(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct mfc_charger_data *charger = power_supply_get_drvdata(psy);
	const ptrdiff_t offset = attr - mfc_attrs;
	int x, ret;

	dev_info(charger->dev, "%s\n", __func__);

	switch (offset) {
	case MFC_ADDR:
		if (sscanf(buf, "0x%4x\n", &x) == 1)
			charger->addr = x;
		ret = count;
		break;
	case MFC_SIZE:
		if (sscanf(buf, "%5d\n", &x) == 1)
			charger->size = x;
		ret = count;
		break;
	case MFC_DATA:
		if (sscanf(buf, "0x%10x", &x) == 1) {
			u8 data = x;

			if (mfc_reg_write(charger->client, charger->addr, data) < 0) {
				dev_info(charger->dev, "%s: addr: 0x%x write fail\n", __func__, charger->addr);
			}
		}
		ret = count;
		break;
	case MFC_PACKET:
	{
		u32 header, cmd, data;

		if (sscanf(buf, "0x%4x 0x%4x 0x%4x\n", &header, &cmd, &data) == 3) {
			dev_info(charger->dev, "%s: 0x%x, 0x%x, 0x%x \n", __func__, (u8)header, (u8)cmd, (u8)data);
			mfc_send_packet(charger, (u8)header, (u8)cmd, (u8)data);
		}
		ret = count;
	}
		break;
	case MFC_FLICKER_TEST:
	{
		char tc;
		if (sscanf(buf, "%c %10d\n", &tc, &x) == 2) {
			pr_info("%s: flicker test change value. %c -> %d\n", __func__, tc, x);
			if (tc == 'd') {
				charger->flicker_delay = x;
			} else if (tc == 'v') {
				charger->flicker_vout_threshold = x;
			}
		}
		ret = count;
		break;
	}
	case TEST:
	{
		int mask, size, value;
		if (sscanf(buf, "%d %d %d\n", &mask, &size, &value) >= 3) {
			if (mask + size >= MFC_TEST_MAX) {
				pr_info("%s: failed to write value by overflow\n", __func__);
			} else {
				x = 1;
				while (size-- > 0)
					x *= 2;
				x = ((x - 1) << mask);
				charger->test &= ~x;
				charger->test |= (value << mask);
				pr_info("%s: set test value(x=%x, mask=%x, value=%d, test=%x)\n",
					__func__, x, mask, value, charger->test);
			}
		} else {
			pr_info("%s: test cmd error\n", __func__);
		}
		ret = count;
	}
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}

static const struct power_supply_desc mfc_charger_power_supply_desc = {
	.name = "mfc-charger",
	.type = POWER_SUPPLY_TYPE_UNKNOWN,
	.properties = mfc_charger_props,
	.num_properties = ARRAY_SIZE(mfc_charger_props),
	.get_property = mfc_chg_get_property,
	.set_property = mfc_chg_set_property,
};

static void mfc_wpc_int_req_work(struct work_struct *work)
{
	struct mfc_charger_data *charger =
		container_of(work, struct mfc_charger_data, wpc_int_req_work.work);

	int ret = 0;

	pr_info("%s\n", __func__);
	/* wpc_irq */
	if (charger->pdata->irq_wpc_int) {
		msleep(100);
		ret = request_threaded_irq(charger->pdata->irq_wpc_int,
				NULL, mfc_wpc_irq_thread,
				IRQF_TRIGGER_FALLING |
				IRQF_TRIGGER_RISING |
				IRQF_ONESHOT,
				"wpc-irq", charger);
		if (ret)
			pr_err("%s: Failed to Request IRQ\n", __func__);
	}
	if (ret < 0)
		free_irq(charger->pdata->irq_wpc_det, NULL);
}

static enum alarmtimer_restart mfc_phm_alarm(
	struct alarm *alarm, ktime_t now)
{
	struct mfc_charger_data *charger = container_of(alarm,
				struct mfc_charger_data, phm_alarm);

	pr_info("%s: forced escape to PHM\n", __func__);
	__pm_stay_awake(charger->wpc_tx_phm_ws);
	queue_delayed_work(charger->wqueue, &charger->wpc_tx_phm_work, 0);

	return ALARMTIMER_NORESTART;
}

static int mfc_charger_probe(
						struct i2c_client *client,
						const struct i2c_device_id *id)
{
	struct device_node *of_node = client->dev.of_node;
	struct mfc_charger_data *charger;
	mfc_charger_platform_data_t *pdata = client->dev.platform_data;
	struct power_supply_config mfc_cfg = {};
	int ret = 0;
	int wc_w_state_irq;

	dev_info(&client->dev, "%s: MFC p9321 Charger Driver Loading\n", __func__);

	if (of_node) {
		pdata = devm_kzalloc(&client->dev, sizeof(*pdata), GFP_KERNEL);
		if (!pdata) {
			dev_err(&client->dev, "Failed to allocate memory\n");
			return -ENOMEM;
		}
		ret = mfc_chg_parse_dt(&client->dev, pdata);
		if (ret < 0)
			goto err_parse_dt;
	} else {
		pdata = client->dev.platform_data;
		return -ENOMEM;
	}

	charger = kzalloc(sizeof(*charger), GFP_KERNEL);
	if (charger == NULL) {
		dev_err(&client->dev, "Memory is not enough.\n");
		ret = -ENOMEM;
		goto err_wpc_nomem;
	}
	charger->dev = &client->dev;

	ret = i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA |
		I2C_FUNC_SMBUS_WORD_DATA | I2C_FUNC_SMBUS_I2C_BLOCK);
	if (!ret) {
		ret = i2c_get_functionality(client->adapter);
		dev_err(charger->dev, "I2C functionality is not supported.\n");
		ret = -ENOSYS;
		goto err_i2cfunc_not_support;
	}

	is_shutdn = false;
	charger->client = client;
	charger->pdata = pdata;

	pr_info("%s: %s\n", __func__, charger->pdata->wireless_charger_name);

	i2c_set_clientdata(client, charger);
#if defined(CONFIG_WIRELESS_IC_PARAM)
	mfc_parse_param_value(charger);
#endif

	charger->pdata->cable_type = SEC_WIRELESS_PAD_NONE;
	charger->pdata->is_charging = 0;
	charger->tx_status = 0;
	charger->pdata->cs100_status = 0;
	charger->pdata->capacity = 101;
	charger->pdata->vout_status = MFC_VOUT_5V;
	charger->pdata->opfq_cnt = 0;

	charger->flicker_delay = 1000;
	charger->flicker_vout_threshold = MFC_VOUT_8V;

	charger->is_mst_on = MST_MODE_0;
	charger->chip_id = MFC_CHIP_IDT;
	charger->chip_id_now = MFC_CHIP_ID_P9320;
	charger->is_otg_on = false;
	charger->led_cover = 0;
	charger->vout_mode = WIRELESS_VOUT_OFF;
	charger->is_full_status = 0;
	charger->is_afc_tx = false;
	charger->pad_ctrl_by_lcd = false;
	charger->wc_tx_enable = false;
	charger->wc_tx_ac_missing = false;
	charger->initial_wc_check = false;
	charger->wc_rx_connected = false;
	charger->wc_rx_fod = false;
	charger->adt_transfer_status = WIRELESS_AUTH_WAIT;
	charger->current_rx_power = TX_RX_POWER_0W;
	charger->tx_id = TX_ID_UNKNOWN;
	charger->tx_id_cnt = 0;
	charger->initial_vrect = false;
	charger->wc_ldo_status = MFC_LDO_ON;
	charger->tx_id_done = false;
	charger->wc_rx_type = NO_DEV;
	charger->is_suspend = false;
	charger->device_event = 0;
	charger->wpc_en_flag = (WPC_EN_SYSFS | WPC_EN_CHARGING | WPC_EN_CCIC);
	charger->req_tx_id = false;
	charger->is_abnormal_pad = false;
	charger->sleep_mode = false;

	mutex_init(&charger->io_lock);
	mutex_init(&charger->wpc_en_lock);

	/* wpc_det */
	if (charger->pdata->irq_wpc_det) {
		INIT_DELAYED_WORK(&charger->wpc_det_work, mfc_wpc_det_work);
		INIT_DELAYED_WORK(&charger->wpc_opfq_work, mfc_wpc_opfq_work);
	}

	/* wpc_irq (INT_A) */
	if (charger->pdata->irq_wpc_int) {
		INIT_DELAYED_WORK(&charger->wpc_isr_work, mfc_wpc_isr_work);
		INIT_DELAYED_WORK(&charger->wpc_tx_isr_work, mfc_wpc_tx_isr_work);
		INIT_DELAYED_WORK(&charger->wpc_tx_id_work, mfc_wpc_tx_id_work);
		INIT_DELAYED_WORK(&charger->wpc_int_req_work, mfc_wpc_int_req_work);
		INIT_DELAYED_WORK(&charger->wpc_vrect_check_work, mfc_vrect_check_work);
	}
	INIT_DELAYED_WORK(&charger->wpc_vout_mode_work, mfc_wpc_vout_mode_work);
	INIT_DELAYED_WORK(&charger->wpc_afc_vout_work, mfc_wpc_afc_vout_work);
	INIT_DELAYED_WORK(&charger->wpc_fw_update_work, mfc_wpc_fw_update_work);
	INIT_DELAYED_WORK(&charger->wpc_i2c_error_work, mfc_wpc_i2c_error_work);
	INIT_DELAYED_WORK(&charger->wpc_rx_type_det_work, mfc_wpc_rx_type_det_work);
	INIT_DELAYED_WORK(&charger->wpc_rx_connection_work, mfc_wpc_rx_connection_work);
	INIT_DELAYED_WORK(&charger->wpc_tx_op_freq_work, mfc_tx_op_freq_work);
	INIT_DELAYED_WORK(&charger->wpc_tx_min_op_freq_work, mfc_tx_min_op_freq_work);
#if defined(CONFIG_TX_GEAR_AOV)
	INIT_DELAYED_WORK(&charger->wpc_tx_aov_phm_work, mfc_tx_aov_phm_work);
#endif
	INIT_DELAYED_WORK(&charger->wpc_tx_phm_work, mfc_tx_phm_work);
	INIT_DELAYED_WORK(&charger->wpc_cs100_work, mfc_cs100_work);
	INIT_DELAYED_WORK(&charger->wpc_rx_power_work, mfc_wpc_rx_power_work);
	INIT_DELAYED_WORK(&charger->wpc_tx_ac_missing_work, mfc_tx_ac_missing_work);

	alarm_init(&charger->phm_alarm, ALARM_BOOTTIME, mfc_phm_alarm);

	mfc_cfg.drv_data = charger;
	charger->psy_chg = power_supply_register(charger->dev, &mfc_charger_power_supply_desc, &mfc_cfg);
	if (IS_ERR(charger->psy_chg)) {
		ret = PTR_ERR(charger->psy_chg);
		pr_err("%s: Failed to Register psy_chg(%d)\n", __func__, ret);
		goto err_supply_unreg;
	}

	charger->wqueue = create_singlethread_workqueue("mfc_workqueue");
	if (!charger->wqueue) {
		pr_err("%s: Fail to Create Workqueue\n", __func__);
		goto err_pdata_free;
	}

	charger->wpc_ws = wakeup_source_register(&client->dev, "wpc_ws");
	charger->wpc_rx_ws = wakeup_source_register(&client->dev, "wpc_rx_ws");
	charger->wpc_tx_ws = wakeup_source_register(&client->dev, "wpc_tx_ws");
	charger->wpc_update_ws = wakeup_source_register(&client->dev, "wpc_update_ws");
	charger->wpc_opfq_ws = wakeup_source_register(&client->dev, "wpc_opfq_ws");
	charger->wpc_tx_min_opfq_ws = wakeup_source_register(&client->dev, "wpc_tx_min_opfq_ws");
	charger->wpc_tx_opfq_ws = wakeup_source_register(&client->dev, "wpc_tx_opfq_ws");
	charger->wpc_afc_vout_ws = wakeup_source_register(&client->dev, "wpc_afc_vout_ws");
	charger->wpc_vout_mode_ws = wakeup_source_register(&client->dev, "wpc_vout_mode_ws");
	charger->wpc_rx_connection_ws = wakeup_source_register(&client->dev, "wpc_rx_connection_ws");
	charger->wpc_rx_det_ws = wakeup_source_register(&client->dev, "wpc_rx_det_ws");
#if defined(CONFIG_TX_GEAR_AOV)
	charger->wpc_tx_aov_phm_ws = wakeup_source_register(&client->dev, "wpc_tx_aov_phm_ws");
#endif
	charger->wpc_tx_phm_ws = wakeup_source_register(&client->dev, "wpc_tx_phm_ws");
	charger->wpc_vrect_check_ws = wakeup_source_register(&client->dev, "wpc_vrect_check_ws");
	charger->wpc_tx_id_ws = wakeup_source_register(&client->dev, "wpc_tx_id_ws");
	charger->wpc_cs100_ws = wakeup_source_register(&client->dev, "wpc_cs100_ws");
	charger->wpc_tx_ac_missing_ws = wakeup_source_register(&client->dev, "wpc_tx_ac_missing_ws");

	/* Enable interrupts after battery driver load */
	/* wpc_det */
	if (charger->pdata->irq_wpc_det) {
		ret = request_threaded_irq(charger->pdata->irq_wpc_det,
				NULL, mfc_wpc_det_irq_thread,
				IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING |
				IRQF_ONESHOT,
				"wpd-det-irq", charger);
		if (ret) {
			pr_err("%s: Failed to Request IRQ\n", __func__);
			goto err_irq_wpc_det;
		}
	}

	/* wpc_irq */
	queue_delayed_work(charger->wqueue, &charger->wpc_int_req_work, msecs_to_jiffies(100));

	wc_w_state_irq = gpio_get_value(charger->pdata->wpc_int);
	pr_info("%s: wc_w_state_irq = %d\n", __func__, wc_w_state_irq);
	if (gpio_get_value(charger->pdata->wpc_det)) {
		u8 irq_src[2];
		pr_info("%s: Charger interrupt occurred during lpm\n", __func__);

		mfc_reg_read(charger->client, MFC_INT_A_L_REG, &irq_src[0]);
		mfc_reg_read(charger->client, MFC_INT_A_H_REG, &irq_src[1]);
		/* clear intterupt */
		pr_info("%s: interrupt source(0x%x)\n", __func__, irq_src[1] << 8 | irq_src[0]);
		mfc_reg_write(charger->client, MFC_INT_A_CLEAR_L_REG, irq_src[0]); // clear int
		mfc_reg_write(charger->client, MFC_INT_A_CLEAR_H_REG, irq_src[1]); // clear int
		mfc_set_cmd_l_reg(charger, MFC_CMD_CLEAR_INT_MASK, MFC_CMD_CLEAR_INT_MASK); // command
		queue_delayed_work(charger->wqueue, &charger->wpc_det_work, 0);
		if (!wc_w_state_irq && !delayed_work_pending(&charger->wpc_isr_work)) {
			__pm_stay_awake(charger->wpc_rx_ws);
			queue_delayed_work(charger->wqueue, &charger->wpc_isr_work, msecs_to_jiffies(2000));
		}
	}
/*#if !defined(CONFIG_SEC_FACTORY)
	else if (!lpcharge) {
		pr_info("%s: call wpc_fw_booting_work for firmware update\n", __func__);
		queue_delayed_work(charger->wqueue, &charger->wpc_fw_booting_work, 0);
	}
#endif*/

	ret = mfc_create_attrs(&charger->psy_chg->dev);
	if (ret)
		dev_err(charger->dev, "%s: Failed to create_attrs\n", __func__);

	charger->is_probed = true;
	dev_info(&client->dev, "%s: MFC Charger Driver Loaded\n", __func__);

	device_init_wakeup(charger->dev, 1);
	return 0;

err_irq_wpc_det:
	wakeup_source_unregister(charger->wpc_ws);
	wakeup_source_unregister(charger->wpc_rx_ws);
	wakeup_source_unregister(charger->wpc_tx_ws);
	wakeup_source_unregister(charger->wpc_update_ws);
	wakeup_source_unregister(charger->wpc_opfq_ws);
	wakeup_source_unregister(charger->wpc_tx_opfq_ws);
	wakeup_source_unregister(charger->wpc_tx_min_opfq_ws);
	wakeup_source_unregister(charger->wpc_afc_vout_ws);
	wakeup_source_unregister(charger->wpc_vout_mode_ws);
	wakeup_source_unregister(charger->wpc_rx_connection_ws);
	wakeup_source_unregister(charger->wpc_rx_det_ws);
#if defined(CONFIG_TX_GEAR_AOV)
	wakeup_source_unregister(charger->wpc_tx_aov_phm_ws);
#endif
	wakeup_source_unregister(charger->wpc_tx_phm_ws);
	wakeup_source_unregister(charger->wpc_vrect_check_ws);
	wakeup_source_unregister(charger->wpc_tx_id_ws);
err_pdata_free:
	power_supply_unregister(charger->psy_chg);
err_supply_unreg:
	mutex_destroy(&charger->io_lock);
	mutex_destroy(&charger->wpc_en_lock);
err_i2cfunc_not_support:
	kfree(charger);
err_wpc_nomem:
err_parse_dt:
	devm_kfree(&client->dev, pdata);

	return ret;
}

static int mfc_charger_remove(struct i2c_client *client)
{
	struct mfc_charger_data *charger = i2c_get_clientdata(client);

	alarm_cancel(&charger->phm_alarm);

	return 0;
}

#if defined(CONFIG_PM)
static int mfc_charger_suspend(struct device *dev)
{
	struct mfc_charger_data *charger = dev_get_drvdata(dev);

	pr_info("%s: det(%d) int(%d)\n", __func__,
			gpio_get_value(charger->pdata->wpc_det),
			gpio_get_value(charger->pdata->wpc_int));

	charger->is_suspend = true;

	if (device_may_wakeup(charger->dev)) {
		enable_irq_wake(charger->pdata->irq_wpc_int);
		enable_irq_wake(charger->pdata->irq_wpc_det);
	}
#if !IS_ENABLED(CONFIG_ENABLE_WIRELESS_IRQ_IN_SLEEP)
	disable_irq(charger->pdata->irq_wpc_int);
	disable_irq(charger->pdata->irq_wpc_det);
#endif

	return 0;
}

static int mfc_charger_resume(struct device *dev)
{
	struct mfc_charger_data *charger = dev_get_drvdata(dev);

	pr_info("%s: det(%d) int(%d)\n", __func__,
			gpio_get_value(charger->pdata->wpc_det),
			gpio_get_value(charger->pdata->wpc_int));

	charger->is_suspend = false;

	if (device_may_wakeup(charger->dev)) {
		disable_irq_wake(charger->pdata->irq_wpc_int);
		disable_irq_wake(charger->pdata->irq_wpc_det);
	}
#if !IS_ENABLED(CONFIG_ENABLE_WIRELESS_IRQ_IN_SLEEP)
	enable_irq(charger->pdata->irq_wpc_int);
	enable_irq(charger->pdata->irq_wpc_det);
#endif

	return 0;
}
#else
#define mfc_charger_suspend NULL
#define mfc_charger_resume NULL
#endif

static void mfc_charger_shutdown(struct i2c_client *client)
{
	struct mfc_charger_data *charger = i2c_get_clientdata(client);
	is_shutdn = true;
	pr_info("%s\n", __func__);

	alarm_cancel(&charger->phm_alarm);

	if (gpio_get_value(charger->pdata->wpc_det)) {
		pr_info("%s: forced 5V Vout\n", __func__);
		mfc_set_vrect_adjust(charger, MFC_HEADROOM_1);
		mfc_set_vout(charger, MFC_VOUT_5V);

		if (charger->is_afc_tx)
			mfc_send_command(charger, MFC_AFC_CONF_5V_TX);
	}

	free_irq(charger->pdata->irq_wpc_det, charger);
	free_irq(charger->pdata->irq_wpc_int, charger);

	cancel_delayed_work(&charger->wpc_det_work);
	cancel_delayed_work(&charger->wpc_opfq_work);
	cancel_delayed_work(&charger->wpc_isr_work);
	cancel_delayed_work(&charger->wpc_tx_isr_work);
	cancel_delayed_work(&charger->wpc_tx_id_work);
	cancel_delayed_work(&charger->wpc_int_req_work);
	cancel_delayed_work(&charger->wpc_vrect_check_work);
	cancel_delayed_work(&charger->wpc_vout_mode_work);
	cancel_delayed_work(&charger->wpc_afc_vout_work);
	cancel_delayed_work(&charger->wpc_fw_update_work);
	cancel_delayed_work(&charger->wpc_i2c_error_work);
	cancel_delayed_work(&charger->wpc_rx_type_det_work);
	cancel_delayed_work(&charger->wpc_rx_connection_work);
	cancel_delayed_work(&charger->wpc_tx_op_freq_work);
	cancel_delayed_work(&charger->wpc_tx_min_op_freq_work);
	cancel_delayed_work(&charger->wpc_tx_phm_work);
	cancel_delayed_work(&charger->wpc_cs100_work);
	cancel_delayed_work(&charger->wpc_rx_power_work);
	cancel_delayed_work(&charger->wpc_tx_ac_missing_work);
}

static const struct i2c_device_id mfc_charger_id_table[] = {
	{ "mfc-charger", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, mfc_charger_id_table);

#ifdef CONFIG_OF
static struct of_device_id mfc_charger_match_table[] = {
	{ .compatible = "idt,mfc-charger",},
	{},
};
#else
#define mfc_charger_match_table NULL
#endif

const struct dev_pm_ops mfc_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(mfc_charger_suspend,mfc_charger_resume)
};

static struct i2c_driver mfc_charger_driver = {
	.driver = {
		.name	= "mfc-charger",
		.owner	= THIS_MODULE,
#if defined(CONFIG_PM)
		.pm = &mfc_pm,
#endif /* CONFIG_PM */
		.of_match_table = mfc_charger_match_table,
	},
	.shutdown	= mfc_charger_shutdown,
	.probe	= mfc_charger_probe,
	.remove	= mfc_charger_remove,
	.id_table	= mfc_charger_id_table,
};

static int __init mfc_charger_init(void)
{
#if defined(CONFIG_WIRELESS_CHARGER_HAL_MFC)
	if (mfc_chip_id_now != 0xFF && mfc_chip_id_now != MFC_CHIP_ID_P9320)
		return -ENODEV;
#endif
	pr_info("%s\n", __func__);
	return i2c_add_driver(&mfc_charger_driver);
}

static void __exit mfc_charger_exit(void)
{
	pr_info("%s\n", __func__);
	i2c_del_driver(&mfc_charger_driver);
}

module_init(mfc_charger_init);
module_exit(mfc_charger_exit);

MODULE_DESCRIPTION("Samsung MFC Charger Driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
