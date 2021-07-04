/*
 * Copyrights (C) 2017 Samsung Electronics, Inc.
 * Copyrights (C) 2017 Maxim Integrated Products, Inc.
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
 */

#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#ifdef CONFIG_OF
#include <linux/of_gpio.h>
#endif
#include <linux/mfd/core.h>
#include <linux/mfd/max77705.h>
#include <linux/mfd/max77705-private.h>
#include <linux/usb_notify.h>
#if IS_ENABLED(CONFIG_PDIC_NOTIFIER)
#include <linux/usb/typec/common/pdic_core.h>
#include <linux/usb/typec/common/pdic_sysfs.h>
#include <linux/usb/typec/common/pdic_notifier.h>
#endif
#include <linux/muic/muic.h>
#include <linux/usb/typec/maxim/max77705-muic.h>
#include <linux/usb/typec/maxim/max77705_usbc.h>
#include <linux/usb/typec/maxim/max77705_alternate.h>
#if defined(CONFIG_CCIC_MAX77705_DEBUG)
#include <linux/usb/typec/maxim/max77705_debug.h>
#endif
#include <linux/usb/typec/manager/if_cb_manager.h>
#include <linux/battery/sec_battery_common.h>
#ifdef MAX77705_SYS_FW_UPDATE
#include <linux/firmware.h>
#if IS_ENABLED(CONFIG_SPU_VERIFY)
#include <linux/spu-verify.h>
#endif
#endif

static enum pdic_sysfs_property max77705_sysfs_properties[] = {
	PDIC_SYSFS_PROP_CHIP_NAME,
	PDIC_SYSFS_PROP_CUR_VERSION,
	PDIC_SYSFS_PROP_SRC_VERSION,
	PDIC_SYSFS_PROP_LPM_MODE,
	PDIC_SYSFS_PROP_STATE,
	PDIC_SYSFS_PROP_RID,
	PDIC_SYSFS_PROP_CTRL_OPTION,
	PDIC_SYSFS_PROP_BOOTING_DRY,
	PDIC_SYSFS_PROP_FW_UPDATE,
	PDIC_SYSFS_PROP_FW_UPDATE_STATUS,
	PDIC_SYSFS_PROP_FW_WATER,
	PDIC_SYSFS_PROP_DEX_FAN_UVDM,
	PDIC_SYSFS_PROP_ACC_DEVICE_VERSION,
	PDIC_SYSFS_PROP_DEBUG_OPCODE,
	PDIC_SYSFS_PROP_CONTROL_GPIO,
	PDIC_SYSFS_PROP_USBPD_IDS,
	PDIC_SYSFS_PROP_USBPD_TYPE,
	PDIC_SYSFS_PROP_CC_PIN_STATUS,
	PDIC_SYSFS_PROP_RAM_TEST,
	PDIC_SYSFS_PROP_SBU_ADC,
	PDIC_SYSFS_PROP_VSAFE0V_STATUS,
	PDIC_SYSFS_PROP_OVP_IC_SHUTDOWN,
	PDIC_SYSFS_PROP_HMD_POWER,
#if defined(CONFIG_SEC_FACTORY)
	PDIC_SYSFS_PROP_15MODE_WATERTEST_TYPE,
#endif
	PDIC_SYSFS_PROP_MAX_COUNT,
};
#define DRIVER_VER		"1.2VER"

#define MAX77705_MAX_APDCMD_TIME (10*HZ)

#define MAX77705_PMIC_REG_INTSRC_MASK 0x23
#define MAX77705_PMIC_REG_INTSRC 0x22

#define MAX77705_IRQSRC_CHG	(1 << 0)
#define MAX77705_IRQSRC_FG      (1 << 2)
#define MAX77705_IRQSRC_MUIC	(1 << 3)

#define MAX77705_RAM_TEST

#ifdef MAX77705_RAM_TEST
#define MAX77705_RAM_TEST_RETRY_COUNT 1
#define MAX77705_RAM_TEST_SUCCESS 0xA1
#define MAX77705_RAM_TEST_FAIL 0x51

enum MAX77705_RAM_TEST_MODE {
	MAX77705_RAM_TEST_STOP_MODE,
	MAX77705_RAM_TEST_START_MODE,
	MAX77705_RAM_TEST_RETRY_MODE,
};

enum MAX77705_RAM_TEST_RESULT {
	MAX77705_RAM_TEST_RESULT_SUCCESS,
	MAX77705_RAM_TEST_RESULT_FAIL_USBC_FUELGAUAGE,
	MAX77705_RAM_TEST_RESULT_FAIL_USBC,
	MAX77705_RAM_TEST_RESULT_FAIL_FUELGAUAGE,
};
#endif

struct max77705_usbc_platform_data *g_usbc_data;

#ifdef MAX77705_SYS_FW_UPDATE
#define MAXIM_DEFAULT_FW		"secure_max77705.bin"
#define MAXIM_SPU_FW			"/pdic/pdic_fw.bin"

struct pdic_fw_update {
	char id[10];
	char path[50];
	int need_verfy;
	int enforce_do;
};
#endif

static void max77705_usbc_mask_irq(struct max77705_usbc_platform_data *usbc_data);
static void max77705_usbc_umask_irq(struct max77705_usbc_platform_data *usbc_data);
static void max77705_get_version_info(struct max77705_usbc_platform_data *usbc_data);

#ifdef CONFIG_MAX77705_GRL_ENABLE
static int max77705_i2c_master_write(struct max77705_usbc_platform_data *usbpd_data,
			int slave_addr, u8 *reg_addr)
{
	int err;
	int tries = 0;
	u8 buffer[2] = { reg_addr[0], reg_addr[1]};

	struct i2c_msg msgs[] = {
		{
			.addr = slave_addr,
			.flags = usbpd_data->muic->flags & I2C_M_TEN,
			.len = 2,
			.buf = buffer,
		},
	};

	do {
		err = i2c_transfer(usbpd_data->muic->adapter, msgs, 1);
		if (err < 0)
			msg_maxim("i2c_transfer error:%d, addr : %x ,data : %x\n", err, reg_addr[0], reg_addr[1]);
	} while ((err != 1) && (++tries < 20));

	if (err != 1) {
		msg_maxim("write transfer error:%d, addr : %x ,data : %x\n", err, reg_addr[0], reg_addr[1]);
		err = -EIO;
		return err;
	}

	return 1;
}
#endif

#ifdef MAX77705_RAM_TEST
static void max77705_verify_ram_bist_write(struct max77705_usbc_platform_data *usbc_data)
{
	usbc_cmd_data write_data;
	u8 irq_reg[MAX77705_IRQ_GROUP_NR] = {0};
	write_data.opcode = OPCODE_RAM_TEST_COMMAND;
	write_data.write_data[0] = 0x0;
	write_data.write_length = 0x1;
	write_data.read_length = 0x6;
	write_data.is_uvdm = 0x0;
	/* clear all interrpts */
	max77705_bulk_read(usbc_data->muic, MAX77705_USBC_REG_UIC_INT,
			4, &irq_reg[USBC_INT]);
	msg_maxim("[MAX77705] irq_reg, %x, %x, %x, %x", irq_reg[USBC_INT], irq_reg[CC_INT], irq_reg[PD_INT], irq_reg[VDM_INT]);
	max77705_write_reg(usbc_data->muic, REG_UIC_INT_M, 0x3F);
	max77705_write_reg(usbc_data->muic, REG_CC_INT_M, 0xFF);
	max77705_write_reg(usbc_data->muic, REG_PD_INT_M, 0xFF);
	max77705_write_reg(usbc_data->muic, REG_VDM_INT_M, 0xFF);

	max77705_usbc_opcode_write(usbc_data, &write_data);
	if(usbc_data->ram_test_enable == MAX77705_RAM_TEST_STOP_MODE) {
		usbc_data->ram_test_enable = MAX77705_RAM_TEST_START_MODE;
		usbc_data->ram_test_retry = 0x0;
	}
}
#endif

int max77705_current_pr_state(struct max77705_usbc_platform_data *usbc_data)
{
	int current_pr = usbc_data->cc_data->current_pr;
	return current_pr;

}

void blocking_auto_vbus_control(int enable)
{
	int current_pr = 0;

	msg_maxim("disable : %d", enable);

	if (enable) {
		current_pr = max77705_current_pr_state(g_usbc_data);
		switch (current_pr) {
		case SRC:
			/* turn off the vbus */
			max77705_vbus_turn_on_ctrl(g_usbc_data, OFF, false);
			break;
		default:
			break;
		}
		g_usbc_data->mpsm_mode = MPSM_ON;
	} else {
		current_pr = max77705_current_pr_state(g_usbc_data);
		switch (current_pr) {
		case SRC:
			max77705_vbus_turn_on_ctrl(g_usbc_data, ON, false);
			break;
		default:
			break;

		}
		g_usbc_data->mpsm_mode = MPSM_OFF;
	}
	msg_maxim("current_pr : %x disable : %x", current_pr, enable);
}
EXPORT_SYMBOL(blocking_auto_vbus_control);

static void vbus_control_hard_reset(struct work_struct *work)
{
	struct max77705_usbc_platform_data *usbpd_data = g_usbc_data;

	msg_maxim("current_pr=%d", usbpd_data->cc_data->current_pr);

	if (usbpd_data->cc_data->current_pr == SRC)
		max77705_vbus_turn_on_ctrl(usbpd_data, ON, false);
}

void max77705_usbc_enable_auto_vbus(struct max77705_usbc_platform_data *usbc_data)
{
	usbc_cmd_data write_data;

	init_usbc_cmd_data(&write_data);
	write_data.opcode = OPCODE_SAMSUNG_FACTORY_TEST;
	write_data.write_data[0] = 0x2;
	write_data.write_length = 0x1;
	write_data.read_length = 0x1;
	max77705_usbc_opcode_write(usbc_data, &write_data);
	msg_maxim("TURN ON THE AUTO VBUS");
	usbc_data->auto_vbus_en = true;
}

void max77705_usbc_disable_auto_vbus(struct max77705_usbc_platform_data *usbc_data)
{
	usbc_cmd_data write_data;

	init_usbc_cmd_data(&write_data);
	write_data.opcode = OPCODE_SAMSUNG_FACTORY_TEST;
	write_data.write_data[0] = 0x0;
	write_data.write_length = 0x1;
	write_data.read_length = 0x1;
	max77705_usbc_opcode_write(usbc_data, &write_data);
	msg_maxim("TURN OFF THE AUTO VBUS");
	usbc_data->auto_vbus_en = false;
}

void max77705_usbc_enable_audio(struct max77705_usbc_platform_data *usbc_data)
{
	usbc_cmd_data write_data;

	/* we need new function for BIT_CCDbgEn */
	usbc_data->op_ctrl1_w |= (BIT_CCDbgEn | BIT_CCAudEn);

	init_usbc_cmd_data(&write_data);
	write_data.opcode = OPCODE_CCCTRL1_W;
	write_data.write_data[0] = usbc_data->op_ctrl1_w;
	write_data.write_length = 0x1;
	write_data.read_length = 0x1;
	max77705_usbc_opcode_write(usbc_data, &write_data);
	msg_maxim("Enable Audio Detect");
}

static void max77705_usbc_debug_function(struct max77705_usbc_platform_data *usbc_data)
{
	usbc_cmd_data write_data;

	msg_maxim("called");

	init_usbc_cmd_data(&write_data);
	write_data.opcode = 0x74;
	write_data.write_data[0] = 0x0;
	write_data.write_length = 0x1;
	write_data.read_length = 0xA;
	max77705_usbc_opcode_write(usbc_data, &write_data);
}

static int max77705_usbc_gpio5_direction_output(
		struct max77705_usbc_platform_data *usbc_data, int value)
{
	int i = 0;
	usbc_cmd_data write_data;

	usbc_data->ovp_gpio = 0xf; /* invalid value */

	reinit_completion(&usbc_data->ccic_sysfs_completion);

	msg_maxim("gpio5: %s", value ? "High" : "Low");
	init_usbc_cmd_data(&write_data);
	write_data.opcode = OPCODE_SAMSUNG_GPIO5_CONTROL;
	write_data.write_length = 0x2;
	write_data.write_data[0] = 0x1; /* output */
	write_data.write_data[1] = !!value;
	write_data.read_length = 0x2;
	max77705_usbc_opcode_write(usbc_data, &write_data);

	i = wait_for_completion_timeout(&usbc_data->ccic_sysfs_completion,
			msecs_to_jiffies(1000 * 5));
	if (i == 0)
		msg_maxim("CCIC SYSFS COMPLETION TIMEOUT");

	if (usbc_data->ovp_gpio != !!value) {
		msg_maxim("Value is different");
		return -1;
	}

	msg_maxim("gpio5: %s, done", usbc_data->ovp_gpio ? "High" : "Low");

	return 0;
}

static void max77705_usbc_gpio5_read_complete(
		struct max77705_usbc_platform_data *usbc_data,
		unsigned char *data)
{
	u8 direction = data[1];
	u8 value = data[2];

	msg_maxim("gpio5: Direction:%s, Value:%s",
			direction ? "Output" : "Input",
			value ? "High" : "Low");

	if (direction == 1)
		usbc_data->ovp_gpio = value;

	complete(&usbc_data->ccic_sysfs_completion);
}

#ifdef CONFIG_MAX77705_GRL_ENABLE
static void max77705_set_forcetrimi(struct max77705_usbc_platform_data *usbc_data)
{
	u8 ArrSendData[2] = {0x00, 0x00};

	msg_maxim("IN++");
	mutex_lock(&usbc_data->max77705->i2c_lock);
//	ArrSendData[0] = 0xFE;
//	ArrSendData[1] = 0xC5;
//	max77705_i2c_master_write(usbc_data, 0x66, ArrSendData);
//	ArrSendData[0] = 0xb3;
//	ArrSendData[1] = 0x0c;
//	max77705_i2c_master_write(usbc_data, 0x62, ArrSendData);
	ArrSendData[0] = 0x1F;
	ArrSendData[1] = 0x04;
	max77705_i2c_master_write(usbc_data, 0x62, ArrSendData);
	msleep(100);
	mutex_unlock(&usbc_data->max77705->i2c_lock);
	msg_maxim("OUT");
}
#endif

static void max77705_send_role_swap_message(struct max77705_usbc_platform_data *usbpd_data, u8 mode)
{
	usbc_cmd_data write_data;

	max77705_usbc_clear_queue(usbpd_data);
	init_usbc_cmd_data(&write_data);
	write_data.opcode = 0x37;
	/* 0x1 : DR_SWAP, 0x2 : PR_SWAP, 0x4: Manual Role Swap */
	write_data.write_data[0] = mode;
	write_data.write_length = 0x1;
	write_data.read_length = 0x1;
	max77705_usbc_opcode_write(usbpd_data, &write_data);
}

void max77705_rprd_mode_change(struct max77705_usbc_platform_data *usbpd_data, u8 mode)
{
	msg_maxim("mode = 0x%x", mode);

	switch (mode) {
	case TYPE_C_ATTACH_DFP:
	case TYPE_C_ATTACH_UFP:
		max77705_send_role_swap_message(usbpd_data, MANUAL_ROLE_SWAP);
		msleep(1000);
		break;
	default:
		break;
	};
}

void max77705_power_role_change(struct max77705_usbc_platform_data *usbpd_data, int power_role)
{
	msg_maxim("power_role = 0x%x", power_role);

	switch (power_role) {
	case TYPE_C_ATTACH_SRC:
	case TYPE_C_ATTACH_SNK:
		max77705_send_role_swap_message(usbpd_data, POWER_ROLE_SWAP);
		break;
	};
}

void max77705_data_role_change(struct max77705_usbc_platform_data *usbpd_data, int data_role)
{
	msg_maxim("data_role = 0x%x", data_role);

	switch (data_role) {
	case TYPE_C_ATTACH_DFP:
	case TYPE_C_ATTACH_UFP:
		max77705_send_role_swap_message(usbpd_data, DATA_ROLE_SWAP);
		break;
	};
}

#if !defined(CONFIG_SUPPORT_USB_TYPEC_OPS)
static int max77705_dr_set(const struct typec_capability *cap, enum typec_data_role role)
#else
static int max77705_dr_set(struct typec_port *port, enum typec_data_role role)
#endif
{
#if !defined(CONFIG_SUPPORT_USB_TYPEC_OPS)
	struct max77705_usbc_platform_data *usbpd_data = container_of(cap, struct max77705_usbc_platform_data, typec_cap);
#else
	struct max77705_usbc_platform_data *usbpd_data = typec_get_drvdata(port);
#endif
#if defined(CONFIG_USB_HW_PARAM)
	struct otg_notify *o_notify = get_otg_notify();
#endif /* CONFIG_USB_HW_PARAM */

	if (!usbpd_data)
		return -EINVAL;
	msg_maxim("typec_power_role=%d, typec_data_role=%d, role=%d",
		usbpd_data->typec_power_role, usbpd_data->typec_data_role, role);
	
	if (usbpd_data->typec_data_role != TYPEC_DEVICE
		&& usbpd_data->typec_data_role != TYPEC_HOST)
		return -EPERM;
	else if (usbpd_data->typec_data_role == role)
		return -EPERM;

	reinit_completion(&usbpd_data->typec_reverse_completion);
	if (role == TYPEC_DEVICE) {
		msg_maxim("try reversing, from DFP to UFP");
		usbpd_data->typec_try_state_change = TRY_ROLE_SWAP_DR;
		max77705_data_role_change(usbpd_data, TYPE_C_ATTACH_UFP);
	} else if (role == TYPEC_HOST) {
		msg_maxim("try reversing, from UFP to DFP");
		usbpd_data->typec_try_state_change = TRY_ROLE_SWAP_DR;
		max77705_data_role_change(usbpd_data, TYPE_C_ATTACH_DFP);
	} else {
		msg_maxim("invalid typec_role");
		return -EIO;
	}
	if (!wait_for_completion_timeout(&usbpd_data->typec_reverse_completion,	
				msecs_to_jiffies(TRY_ROLE_SWAP_WAIT_MS))) {
		usbpd_data->typec_try_state_change = TRY_ROLE_SWAP_NONE;
		return -ETIMEDOUT; 
	}
#if defined(CONFIG_USB_HW_PARAM)
	if (o_notify)
		inc_hw_param(o_notify, USB_CCIC_DR_SWAP_COUNT);
#endif /* CONFIG_USB_HW_PARAM */
	return 0;
}

#if !defined(CONFIG_SUPPORT_USB_TYPEC_OPS)
static int max77705_pr_set(const struct typec_capability *cap, enum typec_role role)
#else
static int max77705_pr_set(struct typec_port *port, enum typec_role role)
#endif
{
#if !defined(CONFIG_SUPPORT_USB_TYPEC_OPS)
	struct max77705_usbc_platform_data *usbpd_data = container_of(cap, struct max77705_usbc_platform_data, typec_cap);
#else
	struct max77705_usbc_platform_data *usbpd_data = typec_get_drvdata(port);
#endif
#if defined(CONFIG_USB_HW_PARAM)
	struct otg_notify *o_notify = get_otg_notify();
#endif /* CONFIG_USB_HW_PARAM */

	if (!usbpd_data)
		return -EINVAL;

	msg_maxim("typec_power_role=%d, typec_data_role=%d, role=%d",
		usbpd_data->typec_power_role, usbpd_data->typec_data_role, role);

	if (usbpd_data->typec_power_role != TYPEC_SINK
	    && usbpd_data->typec_power_role != TYPEC_SOURCE)
		return -EPERM;
	else if (usbpd_data->typec_power_role == role)
		return -EPERM;

	reinit_completion(&usbpd_data->typec_reverse_completion);
	if (role == TYPEC_SINK) {
		msg_maxim("try reversing, from Source to Sink");
		usbpd_data->typec_try_state_change = TRY_ROLE_SWAP_PR;
		max77705_power_role_change(usbpd_data, TYPE_C_ATTACH_SNK);
	} else if (role == TYPEC_SOURCE) {
		msg_maxim("try reversing, from Sink to Source");
		usbpd_data->typec_try_state_change = TRY_ROLE_SWAP_PR;
		max77705_power_role_change(usbpd_data, TYPE_C_ATTACH_SRC);
	} else {
		msg_maxim("invalid typec_role");
		return -EIO;
	}
	if (!wait_for_completion_timeout(&usbpd_data->typec_reverse_completion,	
				msecs_to_jiffies(TRY_ROLE_SWAP_WAIT_MS))) {
		usbpd_data->typec_try_state_change = TRY_ROLE_SWAP_NONE;
		if (usbpd_data->typec_power_role != role)
		return -ETIMEDOUT; 
	}
#if defined(CONFIG_USB_HW_PARAM)
	if (o_notify)
		inc_hw_param(o_notify, USB_CCIC_PR_SWAP_COUNT);
#endif
	return 0;
}

#if !defined(CONFIG_SUPPORT_USB_TYPEC_OPS)
static int max77705_port_type_set(const struct typec_capability *cap, enum typec_port_type port_type)
#else
static int max77705_port_type_set(struct typec_port *port, enum typec_port_type port_type)
#endif
{
#if !defined(CONFIG_SUPPORT_USB_TYPEC_OPS)
	struct max77705_usbc_platform_data *usbpd_data = container_of(cap, struct max77705_usbc_platform_data, typec_cap);
#else
	struct max77705_usbc_platform_data *usbpd_data = typec_get_drvdata(port);
#endif

	if (!usbpd_data)
		return -EINVAL;

	msg_maxim("typec_power_role=%d, typec_data_role=%d, port_type=%d",
		usbpd_data->typec_power_role, usbpd_data->typec_data_role, port_type);

	reinit_completion(&usbpd_data->typec_reverse_completion);
	if (port_type == TYPEC_PORT_DFP) {
		msg_maxim("try reversing, from UFP(Sink) to DFP(Source)");
		usbpd_data->typec_try_state_change = TRY_ROLE_SWAP_TYPE;
		max77705_rprd_mode_change(usbpd_data, TYPE_C_ATTACH_DFP);
	} else if (port_type == TYPEC_PORT_UFP) {
		msg_maxim("try reversing, from DFP(Source) to UFP(Sink)");
#if IS_ENABLED(CONFIG_PDIC_NOTIFIER)
		max77705_ccic_event_work(usbpd_data,
			PDIC_NOTIFY_DEV_MUIC, PDIC_NOTIFY_ID_ATTACH,
			0/*attach*/, 0/*rprd*/, 0);
#endif
		usbpd_data->typec_try_state_change = TRY_ROLE_SWAP_TYPE;
		max77705_rprd_mode_change(usbpd_data, TYPE_C_ATTACH_UFP);
	} else {
		msg_maxim("invalid typec_role");
		return 0;
	}

	if (!wait_for_completion_timeout(&usbpd_data->typec_reverse_completion, 
				msecs_to_jiffies(TRY_ROLE_SWAP_WAIT_MS))) {
		usbpd_data->typec_try_state_change = TRY_ROLE_SWAP_NONE;
		return -ETIMEDOUT;
	}
	return 0;
}

#if defined(CONFIG_SUPPORT_USB_TYPEC_OPS)
static const struct typec_operations max77705_ops = {
	.dr_set = max77705_dr_set,
	.pr_set = max77705_pr_set,
	.port_type_set = max77705_port_type_set
};
#endif

int max77705_get_pd_support(struct max77705_usbc_platform_data *usbc_data)
{
	bool support_pd_role_swap = false;
	struct device_node *np = NULL;

	np = of_find_compatible_node(NULL, NULL, "maxim,max77705_pdic");

	if (np)
		support_pd_role_swap = of_property_read_bool(np, "support_pd_role_swap");
	else
		msg_maxim("np is null");

	msg_maxim("TYPEC_CLASS: support_pd_role_swap is %d, usbc_data->pd_support : %d",
		support_pd_role_swap, usbc_data->pd_support);

	if (support_pd_role_swap && usbc_data->pd_support)
		return TYPEC_PWR_MODE_PD;

	return usbc_data->pwr_opmode;
}

#if defined(MAX77705_SYS_FW_UPDATE)
static int max77705_firmware_update_sys(struct max77705_usbc_platform_data *data, int fw_dir)
{
	struct max77705_usbc_platform_data *usbc_data = data;
	max77705_fw_header *fw_header;
	const struct firmware *fw_entry;
	int fw_size, ret = 0;
	long fw_verify_size;
	struct pdic_fw_update fwup[FWUP_CMD_MAX] = {
		{"BUILT_IN", "", 0, 1},
		{"UMS", MAXIM_DEFAULT_FW, 0, 1},
		{"SPU", MAXIM_SPU_FW, 1, 0},
		{"SPU_V", MAXIM_SPU_FW, 1, 0}
	};

	if (!usbc_data) {
		msg_maxim("usbc_data is null!!");
		return -ENODEV;
	}

	switch (fw_dir) {
#if defined(CONFIG_CCIC_MAX77705_DEBUG)
	case UMS:
		break;
#endif
#if IS_ENABLED(CONFIG_SPU_VERIFY)
	case SPU:
	case SPU_VERIFICATION:
		break;
#endif
	case BUILT_IN:
		max77705_usbc_fw_setting(usbc_data->max77705, fwup[fw_dir].enforce_do);
		return 0;
	default:
		return -EINVAL;
	}

	ret = request_firmware(&fw_entry, fwup[fw_dir].path, usbc_data->dev);
	if (ret) {
		pr_info("%s: firmware is not available %d\n", __func__, ret);
		return ret;
	}

	fw_size = (int)fw_entry->size;
	fw_header = (max77705_fw_header *)fw_entry->data;
	msg_maxim("Req fw %02X.%02X, length=%ld", fw_header->major, fw_header->minor, fw_entry->size);

#if IS_ENABLED(CONFIG_SPU_VERIFY)
	if (fwup[fw_dir].need_verfy) {
		fw_size = (int)fw_entry->size - SPU_METADATA_SIZE(PDIC);
		fw_verify_size = spu_firmware_signature_verify("PDIC", fw_entry->data, fw_entry->size);
		if (fw_verify_size != fw_size) {
			pr_info("%s: signature verify failed, verify_ret:%ld, ori_size:%d\n",
							__func__, fw_verify_size, fw_size);
			ret = -EPERM;
			goto out;
		}
		if (fw_dir == SPU_VERIFICATION)
			goto out;
	}
#endif

	switch (usbc_data->max77705->pmic_rev) {
	case MAX77705_PASS4:
	case MAX77705_PASS5:
		ret = max77705_usbc_fw_update(usbc_data->max77705, fw_entry->data,
					fw_size, fwup[fw_dir].enforce_do);
		break;
	default:
		msg_maxim("FAILED PMIC_REVISION isn't valid (pmic_rev : 0x%x)\n",
					usbc_data->max77705->pmic_rev);
		break;
	}

out:
	release_firmware(fw_entry);
	return ret;
}
#endif

static int max77705_firmware_update_misc(struct max77705_usbc_platform_data *data,
	void *fw_data, size_t fw_size)
{
	struct max77705_usbc_platform_data *usbc_data = data;
	max77705_fw_header *fw_header;
	int ret = 0;
	const u8 *fw_bin;
	size_t fw_bin_len;
	u8 pmic_rev = 0;/* pmic Rev */
	u8 fw_enable = 0;

	if (!usbc_data) {
		msg_maxim("usbc_data is null!!");
		ret = -ENOMEM;
		goto out;
	}

	pmic_rev = usbc_data->max77705->pmic_rev;

	if (fw_size > 0) {
		msg_maxim("start, size %ld Bytes", fw_size);

		fw_bin_len = fw_size;
		fw_bin = fw_data;
		fw_header = (max77705_fw_header *)fw_bin;
		max77705_read_reg(usbc_data->muic,
				REG_UIC_FW_REV, &usbc_data->FW_Revision);
		max77705_read_reg(usbc_data->muic,
				REG_UIC_FW_MINOR, &usbc_data->FW_Minor_Revision);
		usbc_data->FW_Minor_Revision &= MINOR_VERSION_MASK;
		msg_maxim("chip %02X.%02X, fw %02X.%02X",
				usbc_data->FW_Revision, usbc_data->FW_Minor_Revision,
				fw_header->major, fw_header->minor);
		switch (pmic_rev) {
		case MAX77705_PASS4:
		case MAX77705_PASS5:
			fw_enable = 1;
			break;
		default:
			msg_maxim("FAILED F/W via SYS and PMIC_REVISION isn't valid");
			break;
		};

		if (fw_enable)
			ret = max77705_usbc_fw_update(usbc_data->max77705, fw_bin, (int)fw_bin_len, 1);
		else
			msg_maxim("FAILED F/W MISMATCH pmic_rev : 0x%x, fw_header->major : 0x%x",
					pmic_rev, fw_header->major);
	}
out:
	return ret;
}

void max77705_manual_jig_on(struct max77705_usbc_platform_data *usbpd_data, int mode)
{
	usbc_cmd_data read_data;
	usbc_cmd_data write_data;

	msg_maxim("usb: mode=%s", mode ? "High" : "Low");

	init_usbc_cmd_data(&read_data);
	init_usbc_cmd_data(&write_data);
	read_data.opcode = OPCODE_CTRL3_R;
	read_data.write_length = 0x0;
	read_data.read_length = 0x1;
	write_data.opcode = OPCODE_CTRL3_W;
	if (mode)
		write_data.write_data[0] = 0x1;
	else
		write_data.write_data[0] = 0x0;

	write_data.write_data[1] = 0x1;
	write_data.write_length = 0x2;

	write_data.read_length = 0x0;

	max77705_usbc_opcode_read(usbpd_data, &read_data);
	max77705_usbc_opcode_write(usbpd_data, &write_data);
}

void max77705_control_option_command(struct max77705_usbc_platform_data *usbpd_data, int cmd)
{
	struct max77705_cc_data *cc_data = usbpd_data->cc_data;
	u8 ccstat = 0;
	usbc_cmd_data write_data;

	/* for maxim request : they want to check ccstate here */
	max77705_read_reg(usbpd_data->muic, REG_CC_STATUS0, &cc_data->cc_status0);
	ccstat =  (cc_data->cc_status0 & BIT_CCStat) >> FFS(BIT_CCStat);
	msg_maxim("usb: cmd=0x%x ccstat : %d", cmd, ccstat);

	init_usbc_cmd_data(&write_data);
	/* 1 : Vconn control option command ON */
	/* 2 : Vconn control option command OFF */
	/* 3 : Water Detect option command ON */
	/* 4 : Water Detect option command OFF */
	if (cmd == 1)
		usbpd_data->vconn_test = 0;
	else if (cmd == 2)
		usbpd_data->vconn_test = 0; /* do nothing */
	else if (cmd == 3) { /* if SBU pin is low, water interrupt is happened. */
		write_data.opcode = 0x54;
		write_data.write_data[0] = 0x3;
		write_data.write_length = 0x1;
		write_data.read_length = 0x0;
		max77705_usbc_opcode_write(usbpd_data, &write_data);
	} else if (cmd == 4) {
		write_data.opcode = 0x54;
		write_data.write_data[0] = 0x2;
		write_data.write_length = 0x1;
		write_data.read_length = 0x0;
		max77705_usbc_opcode_write(usbpd_data, &write_data);
	}
	if ((cmd & 0xF) == 0x3)
		usbpd_data->fac_water_enable = 1;
	else if ((cmd & 0xF) == 0x4)
		usbpd_data->fac_water_enable = 0;
}

void max77705_response_sbu_read(struct max77705_usbc_platform_data *usbpd_data, unsigned char *data)
{
	u8 sbu1 = 0, sbu2 = 0;

	sbu1 = data[1];
	sbu2 = data[2];

	msg_maxim("SBU1 = 0x%x, SBU2 = 0x%x", sbu1, sbu2);

	if (sbu1 == 0x0)
		usbpd_data->sbu[0] = 0;
	else
		usbpd_data->sbu[0] = 1;
	if (sbu2 == 0x0)
		usbpd_data->sbu[1] = 0;
	else
		usbpd_data->sbu[1] = 1;
	complete(&usbpd_data->ccic_sysfs_completion);
}

void max77705_request_sbu_read(struct max77705_usbc_platform_data *usbpd_data)
{
	usbc_cmd_data write_data;

	init_usbc_cmd_data(&write_data);
	write_data.opcode = OPCODE_READ_SBU;
	write_data.write_data[0] = 0x1;
	write_data.write_length = 0x1;
	write_data.read_length = 0x2;
	max77705_usbc_opcode_write(usbpd_data, &write_data);
}

void max77705_request_control3_reg_read(struct max77705_usbc_platform_data *usbpd_data)
{
	usbc_cmd_data read_data;

	init_usbc_cmd_data(&read_data);
	read_data.opcode = OPCODE_CTRLREG3_R;
	read_data.write_length = 0x0;
	read_data.read_length = 0x1;
	max77705_usbc_opcode_read(g_usbc_data, &read_data);
}

void max77705_set_CCForceError(struct max77705_usbc_platform_data *usbpd_data)
{
	usbc_cmd_data write_data;

	init_usbc_cmd_data(&write_data);
	write_data.opcode = OPCODE_CCCTRL2_W;
	write_data.write_data[0] = 0x84;
	write_data.write_length = 0x1;
	write_data.read_length = 0x0;
	max77705_usbc_opcode_write(usbpd_data, &write_data);
}

void max77705_set_lockerroren(struct max77705_usbc_platform_data *usbpd_data,
	unsigned char data, u8 en)
{
	usbc_cmd_data write_data;
	u8 control3_reg = data;

	init_usbc_cmd_data(&write_data);
	write_data.opcode = OPCODE_CTRLREG3_W;
	control3_reg &= ~(0x1 << 1);
	write_data.write_data[0] = control3_reg | ((en & 0x1) << 1);
	write_data.write_length = 0x1;
	write_data.read_length = 0x0;
	max77705_usbc_opcode_write(usbpd_data, &write_data);
}

void max77705_control3_read_complete(struct max77705_usbc_platform_data *usbpd_data,
	unsigned char *data)
{
	usbpd_data->control3_reg = data[1];
	complete(&usbpd_data->op_completion);
}

void pdic_manual_ccopen_request(int is_on)
{
	struct max77705_usbc_platform_data *usbpd_data = g_usbc_data;

	msg_maxim("is_on %d > %d", usbpd_data->cc_open_req, is_on);
	if (usbpd_data->cc_open_req != is_on) {
		usbpd_data->cc_open_req = is_on;
		schedule_work(&usbpd_data->cc_open_req_work);
	}
}
EXPORT_SYMBOL(pdic_manual_ccopen_request);

static void max77705_cc_open_work_func(
		struct work_struct *work)
{
	struct max77705_usbc_platform_data *usbc_data;
	u8 lock_err_en;
#ifdef CONFIG_USB_NOTIFY_PROC_LOG
	int event;
#endif

	usbc_data = container_of(work, struct max77705_usbc_platform_data, cc_open_req_work);
	msg_maxim("%s", usbc_data->cc_open_req? "set":"clear");

	if (usbc_data->cc_open_req) {
		reinit_completion(&usbc_data->op_completion);
		max77705_request_control3_reg_read(usbc_data);	/* ref 0x65 -> write 0x67*/
		if (!wait_for_completion_timeout(&usbc_data->op_completion, msecs_to_jiffies(1000))) {
			msg_maxim("OPCMD COMPLETION TIMEOUT");
			return;
		}
		lock_err_en = GET_CONTROL3_LOCK_ERROR_EN(usbc_data->control3_reg);
		msg_maxim("data: 0x%x lock_err_en=%d", usbc_data->control3_reg, lock_err_en);
		if (!lock_err_en) {
#ifdef CONFIG_USB_NOTIFY_PROC_LOG
			event = NOTIFY_EXTRA_CCOPEN_REQ_SET;
			store_usblog_notify(NOTIFY_EXTRA, (void *)&event, NULL);
#endif
			max77705_set_lockerroren(usbc_data, usbc_data->control3_reg, 1);
		}
		max77705_set_CCForceError(usbc_data);
	} else {
		reinit_completion(&usbc_data->op_completion);
		max77705_request_control3_reg_read(usbc_data);
		if (!wait_for_completion_timeout(&usbc_data->op_completion, msecs_to_jiffies(1000)))
			msg_maxim("OPCMD COMPLETION TIMEOUT");

		lock_err_en = GET_CONTROL3_LOCK_ERROR_EN(usbc_data->control3_reg);
		msg_maxim("data: 0x%x lock_err_en=%d", usbc_data->control3_reg, lock_err_en);
		if (lock_err_en) {
#ifdef CONFIG_USB_NOTIFY_PROC_LOG
			event = NOTIFY_EXTRA_CCOPEN_REQ_CLEAR;
			store_usblog_notify(NOTIFY_EXTRA, (void *)&event, NULL);
#endif
			max77705_set_lockerroren(usbc_data, usbc_data->control3_reg, 0);
		}
	}
}

void max77705_response_selftest_read(struct max77705_usbc_platform_data *usbpd_data, unsigned char *data)
{
	u8 cc = 0;

	cc = data[1];
	usbpd_data->sbu[0] = data[2];
	usbpd_data->sbu[1] = data[3];

	msg_maxim("SELFTEST CC = %x SBU1 = 0x%x, SBU2 = 0x%x", cc,
		  usbpd_data->sbu[0], usbpd_data->sbu[1]);
	complete(&usbpd_data->ccic_sysfs_completion);
}

void max77705_request_selftest_read(struct max77705_usbc_platform_data *usbpd_data)
{
	usbc_cmd_data write_data;

	init_usbc_cmd_data(&write_data);
	write_data.opcode = OPCODE_READ_SELFTEST;
	write_data.write_length = 0x1;
	write_data.write_data[0] = 0x1;
	write_data.read_length = 0x3;
	max77705_usbc_opcode_write(usbpd_data, &write_data);
}

#if defined(MAX77705_SYS_FW_UPDATE)
static int max77705_firmware_update_sysfs(struct max77705_usbc_platform_data *usbpd_data, int fw_dir)
{
	int ret = 0;
	usbpd_data->fw_update = 1;
	max77705_usbc_mask_irq(usbpd_data);
	max77705_write_reg(usbpd_data->muic, REG_PD_INT_M, 0xFF);
	max77705_write_reg(usbpd_data->muic, REG_CC_INT_M, 0xFF);
	max77705_write_reg(usbpd_data->muic, REG_UIC_INT_M, 0xFF);
	max77705_write_reg(usbpd_data->muic, REG_VDM_INT_M, 0xFF);
	ret = max77705_firmware_update_sys(usbpd_data, fw_dir);
	max77705_write_reg(usbpd_data->muic, REG_UIC_INT_M, REG_UIC_INT_M_INIT);
	max77705_write_reg(usbpd_data->muic, REG_CC_INT_M, REG_CC_INT_M_INIT);
	max77705_write_reg(usbpd_data->muic, REG_PD_INT_M, REG_PD_INT_M_INIT);
	max77705_write_reg(usbpd_data->muic, REG_VDM_INT_M, REG_VDM_INT_M_INIT);
	// max77705_usbc_enable_auto_vbus(usbpd_data);
	max77705_set_enable_alternate_mode(ALTERNATE_MODE_START);
	max77705_usbc_umask_irq(usbpd_data);
	if (ret)
		usbpd_data->fw_update = 2;
	else
		usbpd_data->fw_update = 0;
	return ret;
}
#endif

static int max77705_firmware_update_callback(void *data,
		void *fw_data, size_t fw_size)
{
	struct max77705_usbc_platform_data *usbpd_data
		= (struct max77705_usbc_platform_data *)data;
	int ret = 0;

	usbpd_data->fw_update = 1;
	max77705_usbc_mask_irq(usbpd_data);
	max77705_write_reg(usbpd_data->muic, REG_PD_INT_M, 0xFF);
	max77705_write_reg(usbpd_data->muic, REG_CC_INT_M, 0xFF);
	max77705_write_reg(usbpd_data->muic, REG_UIC_INT_M, 0xFF);
	max77705_write_reg(usbpd_data->muic, REG_VDM_INT_M, 0xFF);
	ret = max77705_firmware_update_misc(usbpd_data, fw_data, fw_size);
	max77705_write_reg(usbpd_data->muic, REG_UIC_INT_M, REG_UIC_INT_M_INIT);
	max77705_write_reg(usbpd_data->muic, REG_CC_INT_M, REG_CC_INT_M_INIT);
	max77705_write_reg(usbpd_data->muic, REG_PD_INT_M, REG_PD_INT_M_INIT);
	max77705_write_reg(usbpd_data->muic, REG_VDM_INT_M, REG_VDM_INT_M_INIT);
	// max77705_usbc_enable_auto_vbus(usbpd_data);
	max77705_set_enable_alternate_mode(ALTERNATE_MODE_START);
	max77705_usbc_umask_irq(usbpd_data);
	if (ret)
		usbpd_data->fw_update = 2;
	else
		usbpd_data->fw_update = 0;
	return ret;
}

static unsigned long max77705_get_firmware_size(void *data)
{
	struct max77705_usbc_platform_data *usbpd_data
		= (struct max77705_usbc_platform_data *)data;
	unsigned long ret = 0;

	ret = usbpd_data->max77705->fw_size;

	return ret;
}

#if defined(MAX77705_SYS_FW_UPDATE)
static void max77705_firmware_update_sysfs_work(struct work_struct *work)
{
	struct max77705_usbc_platform_data *usbpd_data = container_of(work,
			struct max77705_usbc_platform_data, fw_update_work);

	max77705_firmware_update_sysfs(usbpd_data, BUILT_IN);
}
#endif

int max77705_request_vsafe0v_read(struct max77705_usbc_platform_data *usbpd_data)
{
	u8  cc_status1 = 0;
	int vsafe0v = 0;

	max77705_read_reg(usbpd_data->muic, REG_CC_STATUS1, &cc_status1);
	
	vsafe0v = (cc_status1 & BIT_VSAFE0V) >> FFS(BIT_VSAFE0V);
	pr_info("%s: ccstatus1: 0x%x  %d \n", __func__, cc_status1, vsafe0v);
	return vsafe0v;
}

#if IS_ENABLED(CONFIG_PDIC_NOTIFIER)
static int max77705_sysfs_get_local_prop(struct _pdic_data_t *ppdic_data,
					enum pdic_sysfs_property prop,
					char *buf)
{
	int retval = -ENODEV, i = 0;
	u8 cur_major = 0, cur_minor = 0, src_major = 0, src_minor = 0;
	struct max77705_usbc_platform_data *usbpd_data =
		(struct max77705_usbc_platform_data *)ppdic_data->drv_data;

	if (!usbpd_data) {
		msg_maxim("usbpd_data is null : request prop = %d", prop);
		return -ENODEV;
	}

	switch (prop) {
	case PDIC_SYSFS_PROP_CUR_VERSION:
		retval = max77705_read_reg(usbpd_data->muic, REG_UIC_FW_REV, &cur_major);
		if (retval < 0) {
			msg_maxim("Failed to read FW_REV");
			return retval;
		}
		retval = max77705_read_reg(usbpd_data->muic, REG_UIC_FW_MINOR, &cur_minor);
		if (retval < 0) {
			msg_maxim("Failed to read FW_MINOR_REV");
			return retval;
		}
		cur_minor &= MINOR_VERSION_MASK;
		retval = sprintf(buf, "%02X.%02X\n", cur_major, cur_minor);
		msg_maxim("usb: PDIC_SYSFS_PROP_CUR_VERSION : %02X.%02X",
				cur_major, cur_minor);
		break;
	case PDIC_SYSFS_PROP_SRC_VERSION:
		if (usbpd_data->max77705->pmic_rev == MAX77705_PASS5) {
			src_major = BOOT_FLASH_FW_PASS2[4];
			src_minor = BOOT_FLASH_FW_PASS2[5] & MINOR_VERSION_MASK;
		} else {
			src_major = 0xFF; 
			src_minor = 0xFF; 
		}
		retval = sprintf(buf, "%02X.%02X\n", src_major, src_minor);
		msg_maxim("usb: PDIC_SYSFS_PROP_SRC_VERSION : %02X.%02X",
				src_major, src_minor);
		break;
	case PDIC_SYSFS_PROP_LPM_MODE:
		retval = sprintf(buf, "%d\n", usbpd_data->manual_lpm_mode);
		msg_maxim("usb: PDIC_SYSFS_PROP_LPM_MODE : %d",
				usbpd_data->manual_lpm_mode);
		break;
	case PDIC_SYSFS_PROP_STATE:
		retval = sprintf(buf, "%d\n", usbpd_data->pd_state);
		msg_maxim("usb: PDIC_SYSFS_PROP_STATE : %d",
				usbpd_data->pd_state);
		break;
	case PDIC_SYSFS_PROP_RID:
		retval = sprintf(buf, "%d\n", usbpd_data->cur_rid);
		msg_maxim("usb: PDIC_SYSFS_PROP_RID : %d",
				usbpd_data->cur_rid);
		break;
	case PDIC_SYSFS_PROP_BOOTING_DRY:
		usbpd_data->sbu[0] = 0;usbpd_data->sbu[1] = 0; 
		reinit_completion(&usbpd_data->ccic_sysfs_completion);
		max77705_request_selftest_read(usbpd_data);
		i = wait_for_completion_timeout(&usbpd_data->ccic_sysfs_completion, msecs_to_jiffies(1000 * 5));
		if (i == 0)
			msg_maxim("CCIC SYSFS COMPLETION TIMEOUT");
		msg_maxim("usb: PDIC_SYSFS_PROP_BOOTING_DRY timeout : %d", i);
		if (usbpd_data->sbu[0] >= 7 && usbpd_data->sbu[1]  >= 7)
			retval = sprintf(buf, "%d\n", 1);
		else
			retval = sprintf(buf, "%d\n", 0);
		break;
	case PDIC_SYSFS_PROP_FW_UPDATE_STATUS:

		retval = sprintf(buf, "%s\n", usbpd_data->fw_update == 1 ? "UPDATE" :
						usbpd_data->fw_update == 2 ? "NG" : "OK");
		msg_maxim("usb: PDIC_SYSFS_PROP_FW_UPDATE_STATUS : %s", buf);
		break;
	case PDIC_SYSFS_PROP_FW_WATER:
		retval = sprintf(buf, "%d\n", usbpd_data->current_connstat == WATER ? 1 : 0);
		msg_maxim("usb: PDIC_SYSFS_PROP_FW_WATER : %d",
				usbpd_data->current_connstat == WATER ? 1 : 0);
		break;
	case PDIC_SYSFS_PROP_ACC_DEVICE_VERSION:
		retval = sprintf(buf, "%04x\n", usbpd_data->Device_Version);
		msg_maxim("usb: PDIC_SYSFS_PROP_ACC_DEVICE_VERSION : %d",
				usbpd_data->Device_Version);
		break;
	case PDIC_SYSFS_PROP_CONTROL_GPIO:
		usbpd_data->sbu[0] = 0;usbpd_data->sbu[1] = 0; 
		reinit_completion(&usbpd_data->ccic_sysfs_completion);
		max77705_request_sbu_read(usbpd_data);
		i = wait_for_completion_timeout(&usbpd_data->ccic_sysfs_completion, msecs_to_jiffies(200 * 5));
		if (i == 0)
			msg_maxim("CCIC SYSFS COMPLETION TIMEOUT");
		/* compare SBU1, SBU2 values after interrupt */
		msg_maxim("usb: PDIC_SYSFS_PROP_CONTROL_GPIO SBU1 = 0x%x ,SBU2 = 0x%x timeout:%d",
		usbpd_data->sbu[0], usbpd_data->sbu[1], i);
		retval = sprintf(buf, "%d %d\n", usbpd_data->sbu[0], usbpd_data->sbu[1]);
		break;
	case PDIC_SYSFS_PROP_USBPD_IDS:
		retval = sprintf(buf, "%04x:%04x\n",
				le16_to_cpu(usbpd_data->Vendor_ID),
				le16_to_cpu(usbpd_data->Product_ID));
		msg_maxim("usb: PDIC_SYSFS_USBPD_IDS : %s", buf);
		break;
	case PDIC_SYSFS_PROP_USBPD_TYPE:
		retval = sprintf(buf, "%d\n", usbpd_data->acc_type);
		msg_maxim("usb: PDIC_SYSFS_USBPD_TYPE : %d",
				usbpd_data->acc_type);
		break;
	case PDIC_SYSFS_PROP_CC_PIN_STATUS:
		retval = sprintf(buf, "%d\n", usbpd_data->cc_pin_status);
		msg_maxim("usb: PDIC_SYSFS_PROP_PIN_STATUS : %d",
				usbpd_data->cc_pin_status);
		break;
#ifdef MAX77705_RAM_TEST
	case PDIC_SYSFS_PROP_RAM_TEST:
		max77705_verify_ram_bist_write(usbpd_data);
		for (i = 0; i < 300; i++) {
			msleep(10);
			if (usbpd_data->ram_test_enable == MAX77705_RAM_TEST_STOP_MODE) {
				msleep(3000);
				break;
			}
		}
		msg_maxim("usb: PDIC_SYSFS_PROP_RAM_TEST : %d", usbpd_data->ram_test_result);
		retval = sprintf(buf, "%d\n", usbpd_data->ram_test_result);
		break;
#endif
	case PDIC_SYSFS_PROP_SBU_ADC:
		usbpd_data->sbu[0] = 0;usbpd_data->sbu[1] = 0; 
		reinit_completion(&usbpd_data->ccic_sysfs_completion);
		max77705_request_selftest_read(usbpd_data);
		i = wait_for_completion_timeout(&usbpd_data->ccic_sysfs_completion, msecs_to_jiffies(1000 * 5));
		if (i == 0)
			msg_maxim("CCIC SYSFS COMPLETION TIMEOUT");
		msg_maxim("usb: PDIC_SYSFS_PROP_SBU_ADC : %d %d timeout : %d",
				usbpd_data->sbu[0], usbpd_data->sbu[1], i);
		retval = sprintf(buf, "%d %d\n", usbpd_data->sbu[0], usbpd_data->sbu[1]);
		break;
	case PDIC_SYSFS_PROP_VSAFE0V_STATUS:
		usbpd_data->vsafe0v_status = max77705_request_vsafe0v_read(usbpd_data);
		retval = sprintf(buf, "%d\n", usbpd_data->vsafe0v_status);
		msg_maxim("usb: PDIC_SYSFS_PROP_VSAFE0V_STATUS : %d",
				usbpd_data->vsafe0v_status);
		break;
#if defined(CONFIG_SEC_FACTORY)
	case PDIC_SYSFS_PROP_15MODE_WATERTEST_TYPE:
#if 0 //use unsupport water pid fw feature (ex CONFIG_MAX77705_FW_PID05_SUPPORT)
		retval = sprintf(buf, "unsupport\n");
#else
		retval = sprintf(buf, "uevent\n");
#endif
		pr_info("%s : PDIC_SYSFS_PROP_15MODE_WATERTEST_TYPE : %s", __func__, buf);
		break;
#endif
	default:
		msg_maxim("prop read not supported prop (%d)", prop);
		retval = -ENODATA;
		break;
	}

	return retval;
}

/*
 * assume that 1 HMD device has name(14),vid(4),pid(4) each, then
 * max 32 HMD devices(name,vid,pid) need 806 bytes including TAG, NUM, comba
 */
#define MAX_HMD_POWER_STORE_LEN	1024
enum {
	HMD_POWER_MON = 0,	/* monitor name field */
	HMD_POWER_VID,		/* vid field */
	HMD_POWER_PID,		/* pid field */
	HMD_POWER_FIELD_MAX,
};

/* convert VID/PID string to uint in hexadecimal */
static int _max77705_strtoint(char *tok, uint *result)
{
	int  ret = 0;

	if (!tok || !result) {
		msg_maxim("invalid arg!");
		ret = -EINVAL;
		goto end;
	}

	if (strlen(tok) == 5 && tok[4] == 0xa/*LF*/) {
		/* continue since it's ended with line feed */
	} else if (strlen(tok) != 4) {
		msg_maxim("%s should have 4 len, but %lu!", tok, strlen(tok));
		ret = -EINVAL;
		goto end;
	}

	ret = kstrtouint(tok, 16, result);
	if (ret) {
		msg_maxim("fail to convert %s! ret:%d", tok, ret);
		goto end;
	}
end:
	return ret;
}

int max77705_store_hmd_dev(struct max77705_usbc_platform_data *usbc_data, char *str, size_t len, int num_hmd)
{
	struct max77705_hmd_power_dev *hmd_list;
	char *tok;
	int  i, j, ret = 0, rmdr;
	uint value;

	if (num_hmd <= 0 || num_hmd > MAX_NUM_HMD) {
		msg_maxim("invalid num_hmd! %d", num_hmd);
		ret = -EINVAL;
		goto end;
	}

	hmd_list = usbc_data->hmd_list;
	if (!hmd_list) {
		msg_maxim("hmd_list is null!");
		ret = -ENOMEM;
		goto end;
	}

	msg_maxim("+++ %s, %lu, %d", str, len, num_hmd);

	/* reset */
	for (i = 0; i < MAX_NUM_HMD; i++) {
		memset(hmd_list[i].hmd_name, 0, NAME_LEN_HMD);
		hmd_list[i].vid  = 0;
		hmd_list[i].pid = 0;
	}

	tok = strsep(&str, ",");
	i = 0, j = 0;
	while (tok != NULL && *tok != 0xa/*LF*/) {
		if (i > num_hmd * HMD_POWER_FIELD_MAX) {
			msg_maxim("num of tok cannot exceed <%dx%d>!",
				num_hmd, HMD_POWER_FIELD_MAX);
			break;
		}
		if (j > MAX_NUM_HMD) {
			msg_maxim("num of HMD cannot exceed %d!",
				MAX_NUM_HMD);
			break;
		}

		rmdr = i % HMD_POWER_FIELD_MAX;

		switch (rmdr) {
		case HMD_POWER_MON:
			strlcpy(hmd_list[j].hmd_name, tok, NAME_LEN_HMD);
			break;

		case HMD_POWER_VID:
		case HMD_POWER_PID:
			ret = _max77705_strtoint(tok, &value);
			if (ret)
				goto end;

			if (rmdr == HMD_POWER_VID) {
				hmd_list[j].vid  = value;
			} else {
				hmd_list[j].pid = value;
				j++;	/* move next */
			}
			break;
		}

		tok = strsep(&str, ",");
		i++;
	}
	for (i = 0; i < MAX_NUM_HMD; i++) {
		if (strlen(hmd_list[i].hmd_name) > 0)
			msg_maxim("%s,0x%04x,0x%04x",
				hmd_list[i].hmd_name,
				hmd_list[i].vid,
				hmd_list[i].pid);
	}

end:
	return ret;
}

static void max77705_control_gpio_for_sbu(int onoff)
{
	struct otg_notify *o_notify = get_otg_notify();
	struct usb_notifier_platform_data *pdata = get_notify_data(o_notify);

	if (o_notify && o_notify->set_ldo_onoff)
		o_notify->set_ldo_onoff(pdata, onoff);
}

static ssize_t max77705_sysfs_set_prop(struct _pdic_data_t *ppdic_data,
				    enum pdic_sysfs_property prop,
				    const char *buf, size_t size)
{
	ssize_t retval = size;
	int mode = 0;
#ifdef MAX77705_SYS_FW_UPDATE
	u8 FW_Revision = 0, FW_Minor_Revision = 0;
#endif
	int ret = 0;
	struct max77705_usbc_platform_data *usbpd_data =
		(struct max77705_usbc_platform_data *)ppdic_data->drv_data;
	int rv, len;
	char str[MAX_HMD_POWER_STORE_LEN] = {0,}, *p, *tok;
#if IS_ENABLED(CONFIG_USB_NOTIFY_LAYER)
	struct otg_notify *o_notify = get_otg_notify();
#endif

	if (!usbpd_data) {
		msg_maxim("usbpd_data is null : request prop = %d", prop);
		return -ENODEV;
	}
	switch (prop) {
	case PDIC_SYSFS_PROP_LPM_MODE:
		rv = sscanf(buf, "%d", &mode);
		msg_maxim("usb: PDIC_SYSFS_PROP_LPM_MODE mode=%d", mode);
		switch (mode) {
		case 0:
			/* Disable Low Power Mode for App (SW JIGON Disable) */
			max77705_manual_jig_on(usbpd_data, 0);
			usbpd_data->manual_lpm_mode = 0;
			break;
		case 1:
			/* Enable Low Power Mode for App (SW JIGON Enable) */
			max77705_manual_jig_on(usbpd_data, 1);
			usbpd_data->manual_lpm_mode = 1;
			break;
		case 2:
			/* SW JIGON Enable */
			max77705_manual_jig_on(usbpd_data, 1);
			usbpd_data->manual_lpm_mode = 1;
			break;
		default:
			/* SW JIGON Disable */
			max77705_manual_jig_on(usbpd_data, 0);
			usbpd_data->manual_lpm_mode = 0;
			break;
			}
		break;
	case PDIC_SYSFS_PROP_CTRL_OPTION:
		rv = sscanf(buf, "%d", &mode);
		msg_maxim("usb: PDIC_SYSFS_PROP_CTRL_OPTION mode=%d", mode);
		max77705_control_option_command(usbpd_data, mode);
		break;
	case PDIC_SYSFS_PROP_FW_UPDATE:
		rv = sscanf(buf, "%d", &mode);
		msg_maxim("PDIC_SYSFS_PROP_FW_UPDATE mode=%d", mode);

#ifdef MAX77705_SYS_FW_UPDATE
		retval = max77705_read_reg(usbpd_data->muic, REG_UIC_FW_REV, &FW_Revision);
		if (retval < 0) {
			msg_maxim("Failed to read FW_REV");
			return retval;
		}
		retval = max77705_read_reg(usbpd_data->muic, REG_UIC_FW_MINOR, &FW_Minor_Revision);
		if (retval < 0) {
			msg_maxim("Failed to read FW_MINOR_REV");
			return retval;
		}
		FW_Minor_Revision &= MINOR_VERSION_MASK;
		pr_info("%s before : FW_REV %02X.%02X\n", __func__, FW_Revision, FW_Minor_Revision);

		/* Factory cmd for firmware update
		* argument represent what is source of firmware like below.
		*
		* 0 : [BUILT_IN] Getting firmware from source.
		* 1 : [UMS] Getting firmware from sd card.
		* 2 : [SPU] Getting firmware from SPU APP.
		*/
		switch (mode) {
		case BUILT_IN:
			schedule_work(&usbpd_data->fw_update_work);
			break;
		case UMS:
		case SPU:
		case SPU_VERIFICATION:
			ret = max77705_firmware_update_sysfs(usbpd_data, mode);
			break;
		default:
			ret = -EINVAL;
			msg_maxim("Not support command[%d]", mode);
			break;
		}
		if (ret < 0) {
			msg_maxim("Failed to update FW");
			return ret;
		}

		max77705_get_version_info(usbpd_data);
#else
		return -EINVAL;
#endif
	    break;
	case PDIC_SYSFS_PROP_DEX_FAN_UVDM:
		rv = sscanf(buf, "%d", &mode);
		msg_maxim("PDIC_SYSFS_PROP_DEX_FAN_UVDM mode=%d", mode);
		max77705_send_dex_fan_unstructured_vdm_message(usbpd_data, mode);
		break;
	case PDIC_SYSFS_PROP_DEBUG_OPCODE:
		rv = sscanf(buf, "%d", &mode);
		msg_maxim("PDIC_SYSFS_PROP_DEBUG_OPCODE mode=%d", mode);
		if (mode)
			max77705_usbc_debug_function(usbpd_data);
		break;
	case PDIC_SYSFS_PROP_CONTROL_GPIO:
		rv = sscanf(buf, "%d", &mode);
		msg_maxim("PDIC_SYSFS_PROP_CONTROL_GPIO mode=%d. do nothing for control gpio.", mode);
		/* orignal concept : mode 0 : SBU1/SBU2 set as open-drain status
		 *                         mode 1 : SBU1/SBU2 set as default status - Pull up
		 *  But, max77705 is always open-drain status so we don't need to control it.
		 */
		max77705_control_gpio_for_sbu(!mode);
		break;
	case PDIC_SYSFS_PROP_OVP_IC_SHUTDOWN:
		rv = sscanf(buf, "%d", &mode);
		msg_maxim("PDIC_SYSFS_PROP_OVP_IC_SHUTDOWN mode=%d", mode);
		ret = max77705_usbc_gpio5_direction_output(usbpd_data, mode);
		if (ret)
			return -ENODATA;
		break;
	case PDIC_SYSFS_PROP_HMD_POWER:
		if (size >= MAX_HMD_POWER_STORE_LEN) {
			msg_maxim("too long args! %lu", size);
			return -EOVERFLOW;
		}
		mutex_lock(&usbpd_data->hmd_power_lock);
		memcpy(str, buf, size);
		p	= str;
		tok = strsep(&p, ",");
		len = strlen(tok);
		msg_maxim("tok: %s, len: %d", tok, len);

		if (!strncmp(TAG_HMD, tok, len)) {
			/* called by HmtManager to inform list of supported HMD devices
			 *
			 * Format :
			 *	 HMD,NUM,NAME01,VID01,PID01,NAME02,VID02,PID02,...
			 *
			 *	 HMD  : tag
			 *	 NUM  : num of HMD dev ..... max 2 bytes to decimal (max 32)
			 *	 NAME : name of HMD ...... max 14 bytes, char string
			 *	 VID  : vendor	id ....... 4 bytes to hexadecimal
			 *	 PID  : product id ....... 4 bytes to hexadecimal
			 *
			 * ex) HMD,2,PicoVR,2d40,0000,Nreal light,0486,573c
			 *
			 * call hmd store function with tag(HMD),NUM removed
			 */
			int num_hmd = 0, sz = 0;

			tok = strsep(&p, ",");
			sz	= strlen(tok);
			kstrtouint(tok, 10, &num_hmd);
			msg_maxim("HMD num: %d, sz:%d", num_hmd, sz);

			max77705_store_hmd_dev(usbpd_data, str + (len + sz + 2), size - (len + sz + 2),
				num_hmd);

			if (usbpd_data->acc_type == PDIC_DOCK_NEW && max77705_check_hmd_dev(usbpd_data)) {
#if IS_ENABLED(CONFIG_USB_NOTIFY_LAYER)
				if (o_notify)
					send_otg_notify(o_notify, NOTIFY_EVENT_HMD_EXT_CURRENT, 1);
#endif
			}
			mutex_unlock(&usbpd_data->hmd_power_lock);
			return size;
		}
		mutex_unlock(&usbpd_data->hmd_power_lock);
		break;
	default:
		pr_info("%s prop write not supported prop (%d)\n", __func__, prop);
		retval = -ENODATA;
		return retval;
	}
	return size;
}

static int max77705_sysfs_is_writeable(struct _pdic_data_t *ppdic_data,
				    enum pdic_sysfs_property prop)
{
	switch (prop) {
	case PDIC_SYSFS_PROP_LPM_MODE:
	case PDIC_SYSFS_PROP_CTRL_OPTION:
	case PDIC_SYSFS_PROP_DEBUG_OPCODE:
	case PDIC_SYSFS_PROP_CONTROL_GPIO:
		return 1;
	default:
		return 0;
	}
}

static int max77705_sysfs_is_writeonly(struct _pdic_data_t *ppdic_data,
				    enum pdic_sysfs_property prop)
{
	switch (prop) {
	case PDIC_SYSFS_PROP_FW_UPDATE:
	case PDIC_SYSFS_PROP_DEX_FAN_UVDM:
	case PDIC_SYSFS_PROP_OVP_IC_SHUTDOWN:
	case PDIC_SYSFS_PROP_HMD_POWER:
		return 1;
	default:
		return 0;
	}
}
#endif
static ssize_t max77705_fw_update(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int start_fw_update = 0;
	usbc_cmd_data read_data;
	usbc_cmd_data write_data;

	init_usbc_cmd_data(&read_data);
	init_usbc_cmd_data(&write_data);
	read_data.opcode = OPCODE_CTRL1_R;
	read_data.write_length = 0x0;
	read_data.read_length = 0x1;

	write_data.opcode = OPCODE_CTRL1_W;
	write_data.write_data[0] = 0x09;
	write_data.write_length = 0x1;
	write_data.read_length = 0x0;

	if (kstrtou32(buf, 0, &start_fw_update)) {
		dev_err(dev,
			"%s: Failed converting from str to u32.", __func__);
	}

	msg_maxim("start_fw_update %d", start_fw_update);

	max77705_usbc_opcode_rw(g_usbc_data, &read_data, &write_data);

	switch (start_fw_update) {
	case 1:
		max77705_usbc_opcode_rw(g_usbc_data, &read_data, &write_data);
		break;
	case 2:
		max77705_usbc_opcode_read(g_usbc_data, &read_data);
		break;
	case 3:
		max77705_usbc_opcode_write(g_usbc_data, &write_data);

		break;
#ifdef CONFIG_MAX77705_GRL_ENABLE
	case 11:
		msg_maxim("SYSTEM MESSAGE GRL COMMAND!!!");
		write_data.opcode = OPCODE_GRL_COMMAND;
		write_data.write_data[0] = 0x1;
		write_data.write_length = 0x1;
		write_data.read_length = 0x2;
		max77705_usbc_opcode_write(g_usbc_data, &write_data);
#endif
#ifdef MAX77705_RAM_TEST
	case 15:
		max77705_verify_ram_bist_write(g_usbc_data);
#endif
		break;
	default:
		break;
	}
	return size;
}
static DEVICE_ATTR(fw_update, S_IRUGO | S_IWUSR | S_IWGRP,
		NULL, max77705_fw_update);

static struct attribute *max77705_attr[] = {
	&dev_attr_fw_update.attr,
	NULL,
};

static struct attribute_group max77705_attr_grp = {
	.attrs = max77705_attr,
};

static void max77705_get_version_info(struct max77705_usbc_platform_data *usbc_data)
{
	u8 hw_rev[4] = {0, };
	u8 sw_main[3] = {0, };
	u8 sw_boot = 0;

	max77705_read_reg(usbc_data->muic, REG_UIC_HW_REV, &hw_rev[0]);
	max77705_read_reg(usbc_data->muic, REG_UIC_FW_MINOR, &sw_main[1]);
	max77705_read_reg(usbc_data->muic, REG_UIC_FW_REV, &sw_main[0]);

	usbc_data->HW_Revision = hw_rev[0];
	usbc_data->FW_Minor_Revision = sw_main[1] & MINOR_VERSION_MASK;
	usbc_data->FW_Revision = sw_main[0];

	/* H/W, Minor, Major, Boot */
	msg_maxim("HW rev is %02Xh, FW rev is %02X.%02X!",
			usbc_data->HW_Revision, usbc_data->FW_Revision, usbc_data->FW_Minor_Revision);

	store_ccic_version(&hw_rev[0], &sw_main[0], &sw_boot);
}

static void max77705_init_opcode
		(struct max77705_usbc_platform_data *usbc_data, int reset)
{
	struct max77705_platform_data *pdata = usbc_data->max77705_data;

	max77705_usbc_disable_auto_vbus(usbc_data);
	if (pdata && pdata->support_audio)
		max77705_usbc_enable_audio(usbc_data);
	if (reset)
		max77705_set_enable_alternate_mode(ALTERNATE_MODE_START);
}

static bool max77705_check_recover_opcode(u8 opcode)
{
	bool ret = false;

	switch (opcode) {
	case OPCODE_CCCTRL1_W:
	case OPCODE_SAMSUNG_FACTORY_TEST:
	case OPCODE_SET_ALTERNATEMODE:
		ret = true;
		break;
	default:
		ret = false;
		break;
	}
	return ret;
}

static void max77705_recover_opcode
		(struct max77705_usbc_platform_data *usbc_data, bool opcode_list[])
{
	int i;

	for (i = 0; i < OPCODE_NONE; i++) {
		if (opcode_list[i]) {
			msg_maxim("opcode = 0x%02x", i);
			switch (i) {
			case OPCODE_CCCTRL1_W:
				if (usbc_data->op_ctrl1_w & BIT_CCAudEn)
					max77705_usbc_enable_audio(usbc_data);
				break;
			case OPCODE_SAMSUNG_FACTORY_TEST:
				if (usbc_data->auto_vbus_en)
					max77705_usbc_enable_auto_vbus(usbc_data);
				else
					max77705_usbc_disable_auto_vbus(usbc_data);
				break;
			case OPCODE_SET_ALTERNATEMODE:
				max77705_set_enable_alternate_mode
					(usbc_data->set_altmode);
				break;
			default:
				break;
			}
			opcode_list[i] = false;
		}
	}
}

void init_usbc_cmd_data(usbc_cmd_data *cmd_data)
{
	cmd_data->opcode = OPCODE_NONE;
	cmd_data->prev_opcode = OPCODE_NONE;
	cmd_data->response = OPCODE_NONE;
	cmd_data->val = REG_NONE;
	cmd_data->mask = REG_NONE;
	cmd_data->reg = REG_NONE;
	cmd_data->noti_cmd = OPCODE_NOTI_NONE;
	cmd_data->write_length = 0;
	cmd_data->read_length = 0;
	cmd_data->seq = 0;
	cmd_data->is_uvdm = 0;
	memset(cmd_data->write_data, REG_NONE, OPCODE_DATA_LENGTH);
	memset(cmd_data->read_data, REG_NONE, OPCODE_DATA_LENGTH);
}

static void init_usbc_cmd_node(usbc_cmd_node *usbc_cmd_node)
{
	usbc_cmd_data *cmd_data = &(usbc_cmd_node->cmd_data);

	pr_debug("%s:%s\n", "MAX77705", __func__);

	usbc_cmd_node->next = NULL;

	init_usbc_cmd_data(cmd_data);
}

static void copy_usbc_cmd_data(usbc_cmd_data *from, usbc_cmd_data *to)
{
	to->opcode = from->opcode;
	to->response = from->response;
	memcpy(to->read_data, from->read_data, OPCODE_DATA_LENGTH);
	memcpy(to->write_data, from->write_data, OPCODE_DATA_LENGTH);
	to->reg = from->reg;
	to->mask = from->mask;
	to->val = from->val;
	to->seq = from->seq;
	to->read_length = from->read_length;
	to->write_length = from->write_length;
	to->prev_opcode = from->prev_opcode;
	to->is_uvdm = from->is_uvdm;
}

bool is_empty_usbc_cmd_queue(usbc_cmd_queue_t *usbc_cmd_queue)
{
	bool ret = false;

	if (usbc_cmd_queue->front == NULL)
		ret = true;

	if (ret)
		msg_maxim("usbc_cmd_queue Empty(%c)", ret ? 'T' : 'F');

	return ret;
}

void enqueue_usbc_cmd(usbc_cmd_queue_t *usbc_cmd_queue, usbc_cmd_data *cmd_data)
{
	usbc_cmd_node	*temp_node = kzalloc(sizeof(usbc_cmd_node), GFP_KERNEL);

	if (!temp_node) {
		msg_maxim("failed to allocate usbc command queue");
		return;
	}

	init_usbc_cmd_node(temp_node);

	copy_usbc_cmd_data(cmd_data, &(temp_node->cmd_data));

	if (is_empty_usbc_cmd_queue(usbc_cmd_queue)) {
		usbc_cmd_queue->front = temp_node;
		usbc_cmd_queue->rear = temp_node;
	} else {
		usbc_cmd_queue->rear->next = temp_node;
		usbc_cmd_queue->rear = temp_node;
	}
	
#if defined(CONFIG_QCOM_IFPMIC_SUSPEND)
	if (g_usbc_data && g_usbc_data->max77705)
		g_usbc_data->max77705->is_usbc_queue = 1;
#endif
}

static void dequeue_usbc_cmd
	(usbc_cmd_queue_t *usbc_cmd_queue, usbc_cmd_data *cmd_data)
{
	usbc_cmd_node *temp_node;

	if (is_empty_usbc_cmd_queue(usbc_cmd_queue)) {
		msg_maxim("Queue, Empty!");
		return;
	}

	temp_node = usbc_cmd_queue->front;
	copy_usbc_cmd_data(&(temp_node->cmd_data), cmd_data);

	msg_maxim("Opcode(0x%02x) Response(0x%02x)", cmd_data->opcode, cmd_data->response);

	if (usbc_cmd_queue->front->next == NULL) {
		msg_maxim("front->next = NULL");
		usbc_cmd_queue->front = NULL;
	} else
		usbc_cmd_queue->front = usbc_cmd_queue->front->next;

	if (is_empty_usbc_cmd_queue(usbc_cmd_queue))
		usbc_cmd_queue->rear = NULL;

	kfree(temp_node);
}

static bool front_usbc_cmd
	(usbc_cmd_queue_t *cmd_queue, usbc_cmd_data *cmd_data)
{
	if (is_empty_usbc_cmd_queue(cmd_queue)) {
		msg_maxim("Queue, Empty!");
		return false;
	}

	copy_usbc_cmd_data(&(cmd_queue->front->cmd_data), cmd_data);
	msg_maxim("Opcode(0x%02x)", cmd_data->opcode);
	return true;
}

static bool is_usbc_notifier_opcode(u8 opcode)
{
	bool noti = false;

	return noti;
}

#if defined(CONFIG_QCOM_IFPMIC_SUSPEND)
bool check_usbc_opcode_queue(void)
{
	struct max77705_usbc_platform_data *usbpd_data = g_usbc_data;
	usbc_cmd_queue_t *cmd_queue = NULL;
	bool ret = true;

	if (usbpd_data == NULL)
		goto err;

	cmd_queue = &(usbpd_data->usbc_cmd_queue);

	if (cmd_queue == NULL)
		goto err;

	ret = is_empty_usbc_cmd_queue(cmd_queue);

err:
	return ret;
}
EXPORT_SYMBOL(check_usbc_opcode_queue);
#endif

/*
 * max77705_i2c_opcode_write - SMBus "opcode write" protocol
 * @chip: max77705 platform data
 * @command: OPcode
 * @values: Byte array into which data will be read; big enough to hold
 *	the data returned by the slave.
 *
 * This executes the SMBus "opcode read" protocol, returning negative errno
 * else the number of data bytes in the slave's response.
 */
int max77705_i2c_opcode_write(struct max77705_usbc_platform_data *usbc_data,
		u8 opcode, u8 length, u8 *values)
{
	u8 write_values[OPCODE_MAX_LENGTH] = { 0, };
	int ret = 0;

	if (length > OPCODE_DATA_LENGTH)
		return -EMSGSIZE;

	write_values[0] = opcode;
	if (length)
		memcpy(&write_values[1], values, length);

#if 0
	int i = 0; // To use this, move int i to the top to avoid build error
	for (i = 0; i < length + OPCODE_SIZE; i++)
		msg_maxim("[%d], 0x[%x]", i, write_values[i]);
#else
	msg_maxim("opcode 0x%x, write_length %d",
			opcode, length + OPCODE_SIZE);
	print_hex_dump(KERN_INFO, "max77705: opcode_write: ",
			DUMP_PREFIX_OFFSET, 16, 1, write_values,
			length + OPCODE_SIZE, false);
#endif

	/* Write opcode and data */
	ret = max77705_bulk_write(usbc_data->muic, OPCODE_WRITE,
			length + OPCODE_SIZE, write_values);
	/* Write end of data by 0x00 */
	if (length < OPCODE_DATA_LENGTH)
		max77705_write_reg(usbc_data->muic, OPCODE_WRITE_END, 0x00);

	if (opcode == OPCODE_SET_ALTERNATEMODE)
		usbc_data->set_altmode_error = ret;

	if (ret == 0)
		usbc_data->opcode_stamp = jiffies;
	
	return ret;
}

/**
 * max77705_i2c_opcode_read - SMBus "opcode read" protocol
 * @chip: max77705 platform data
 * @command: OPcode
 * @values: Byte array into which data will be read; big enough to hold
 *	the data returned by the slave.
 *
 * This executes the SMBus "opcode read" protocol, returning negative errno
 * else the number of data bytes in the slave's response.
 */
int max77705_i2c_opcode_read(struct max77705_usbc_platform_data *usbc_data,
		u8 opcode, u8 length, u8 *values)
{
	int size = 0;

	if (length > OPCODE_DATA_LENGTH)
		return -EMSGSIZE;

	/*
	 * We don't need to use opcode to get any feedback
	 */

	/* Read opcode data */
	size = max77705_bulk_read(usbc_data->muic, OPCODE_READ,
			length + OPCODE_SIZE, values);

#if 0
	int i = 0; // To use this, move int i to the top to avoid build error
	for (i = 0; i < length + OPCODE_SIZE; i++)
		msg_maxim("[%d], 0x[%x]", i, values[i]);
#else
	msg_maxim("opcode 0x%x, read_length %d, ret_error %d",
			opcode, length + OPCODE_SIZE, size);
	print_hex_dump(KERN_INFO, "max77705: opcode_read: ",
			DUMP_PREFIX_OFFSET, 16, 1, values,
			length + OPCODE_SIZE, false);
#endif
	return size;
}

static void max77705_notify_execute(struct max77705_usbc_platform_data *usbc_data,
		const usbc_cmd_data *cmd_data)
{
		/* to do  */
}

static void max77705_handle_update_opcode(struct max77705_usbc_platform_data *usbc_data,
		const usbc_cmd_data *cmd_data, unsigned char *data)
{
	usbc_cmd_data write_data;
	u8 read_value = data[1];
	u8 write_value = (read_value & (~cmd_data->mask)) | (cmd_data->val & cmd_data->mask);
	u8 opcode = cmd_data->response + 1; /* write opcode = read opocde + 1 */

	pr_info("%s: value update [0x%x]->[0x%x] at OPCODE(0x%x)\n", __func__,
			read_value, write_value, opcode);

	init_usbc_cmd_data(&write_data);
	write_data.opcode = opcode;
	write_data.write_length = 1;
	write_data.write_data[0] = write_value;
	write_data.read_length = 0;

	max77705_usbc_opcode_push(usbc_data, &write_data);
}

static void max77705_request_response(struct max77705_usbc_platform_data *usbc_data)
{
	usbc_cmd_data value;

	init_usbc_cmd_data(&value);
	value.opcode = OPCODE_READ_RESPONSE_FOR_GET_REQUEST;
	value.read_length = 28;
	max77705_usbc_opcode_push(usbc_data, &value);

	pr_info("%s : OPCODE(0x%02x) W_LENGTH(%d) R_LENGTH(%d)\n",
		__func__, value.opcode, value.write_length, value.read_length);
}

#define UNKNOWN_VID 0xFFFF
void max77705_send_get_request(struct max77705_usbc_platform_data *usbc_data, unsigned char *data)
{
	enum {
		SENT_REQ_MSG = 0,
		ERR_SNK_RDY = 5,
		ERR_PD20,
		ERR_SNKTXNG,
	};
	SEC_PD_SINK_STATUS *snk_sts = &usbc_data->pd_data->pd_noti.sink_status;

	if (data[1] == SENT_REQ_MSG) {
		max77705_request_response(usbc_data);
	} else { /* ERROR case */
		/* Mark Error in xid */
		snk_sts->xid = (UNKNOWN_VID << 16) | (data[1] << 8);
		msg_maxim("%s, Err : %d", __func__, data[1]);
	}
}

void max77705_extend_msg_process(struct max77705_usbc_platform_data *usbc_data, unsigned char *data,
		unsigned char len)
{
	SEC_PD_SINK_STATUS *snk_sts = &usbc_data->pd_data->pd_noti.sink_status;

	snk_sts->vid = *(unsigned short *)(data + 2);
	snk_sts->pid = *(unsigned short *)(data + 4);
	snk_sts->xid = *(unsigned int *)(data + 6);
	msg_maxim("%s, %04x, %04x, %08x",
		__func__, snk_sts->vid, snk_sts->pid, snk_sts->xid);
	if (snk_sts->fp_sec_pd_ext_cb)
		snk_sts->fp_sec_pd_ext_cb(snk_sts->vid, snk_sts->pid);
}

void max77705_read_response(struct max77705_usbc_platform_data *usbc_data, unsigned char *data)
{
	SEC_PD_SINK_STATUS *snk_sts = &usbc_data->pd_data->pd_noti.sink_status;

	switch (data[1] >> 5) {
	case OPCODE_GET_SRC_CAP_EXT:
		max77705_extend_msg_process(usbc_data, data+2, data[1] & 0x1F);
		break;
	default:
		snk_sts->xid = (UNKNOWN_VID << 16) | data[1];
		msg_maxim("%s, Err : %d", __func__, data[1]);
		break;
	}
}

static void max77705_irq_execute(struct max77705_usbc_platform_data *usbc_data,
		const usbc_cmd_data *cmd_data)
{
	int len = cmd_data->read_length;
	unsigned char data[OPCODE_DATA_LENGTH] = {0,};
	u8 response = 0xff;
	u8 vdm_opcode_header = 0x0;
	UND_DATA_MSG_VDM_HEADER_Type vdm_header;
	u8 vdm_command = 0x0;
	u8 vdm_type = 0x0;
	u8 vdm_response = 0x0;
	u8 reqd_vdm_command = 0;
	uint8_t W_DATA = 0x0;

	memset(&vdm_header, 0, sizeof(UND_DATA_MSG_VDM_HEADER_Type));
	max77705_i2c_opcode_read(usbc_data, cmd_data->opcode,
			len, data);

	/* opcode identifying the messsage type. (0x51)*/
	response = data[0];

	if (response != cmd_data->response) {
		msg_maxim("Response [0x%02x] != [0x%02x]",
			response, cmd_data->response);
#if !defined (MAX77705_GRL_ENABLE)
		if (cmd_data->response == OPCODE_FW_OPCODE_CLEAR) {
			msg_maxim("Response after FW opcode cleared, just return");
			return;
		}
#endif
	}

	/* to do(read switch case) */
	switch (response) {
	case OPCODE_BCCTRL1_R:
	case OPCODE_BCCTRL2_R:
	case OPCODE_CTRL1_R:
	case OPCODE_CTRL2_R:
	case OPCODE_CTRL3_R:
	case OPCODE_CCCTRL1_R:
	case OPCODE_CCCTRL2_R:
	case OPCODE_CCCTRL3_R:
	case OPCODE_HVCTRL_R:
	case OPCODE_OPCODE_VCONN_ILIM_R:
	case OPCODE_CHGIN_ILIM_R:
	case OPCODE_CHGIN_ILIM2_R:
		if (cmd_data->seq == OPCODE_UPDATE_SEQ)
			max77705_handle_update_opcode(usbc_data, cmd_data, data);
		break;
#if IS_ENABLED(CONFIG_HV_MUIC_MAX77705_AFC)
	case COMMAND_AFC_RESULT_READ:
	case COMMAND_QC_2_0_SET:
		max77705_muic_handle_detect_dev_hv(usbc_data->muic_data, data);
		break;
#endif
	case OPCODE_CURRENT_SRCCAP:
		max77705_current_pdo(usbc_data, data);
		break;
	case OPCODE_GET_SRCCAP:
		max77705_pdo_list(usbc_data, data);
		break;
	case OPCODE_SEND_GET_REQUEST:
		max77705_send_get_request(usbc_data, data);
		break;
	case OPCODE_READ_RESPONSE_FOR_GET_REQUEST:
		max77705_read_response(usbc_data, data);
		break;
	case OPCODE_SRCCAP_REQUEST:
		/*
		 * If response of Source_Capablities message is SinkTxNg(0xFE) or Not in Ready State(0xFF)
		 * It means that the message can not be sent to Port Partner.
		 * After Attaching Rp 3.0A, send again the message.
		 */
		if (data[1] == 0xfe || data[1] == 0xff){
			usbc_data->srcccap_request_retry = true;
			pr_info("%s : srcccap_request_retry is set\n", __func__);
		}
		break;
	case OPCODE_APDO_SRCCAP_REQUEST:
		max77705_response_apdo_request(usbc_data, data);
		break;
	case OPCODE_SET_PPS:		
		max77705_response_set_pps(usbc_data, data);
		break;
	case OPCODE_SAMSUNG_READ_MESSAGE:
		pr_info("@TA_ALERT: %s : OPCODE[%x] Data[1] = 0x%x Data[7] = 0x%x Data[9] = 0x%x\n",
			__func__, OPCODE_SAMSUNG_READ_MESSAGE, data[1], data[7], data[9]);
#if defined(CONFIG_DIRECT_CHARGING)
		if ((data[0] == 0x5D) &&
			/* OCP would be set to Alert or Status message */
			((data[1] == 0x01 && data[7] == 0x04) || (data[1] == 0x02 && (data[9] & 0x02)))) {
			union power_supply_propval value = {0,};
			value.intval = true;
			psy_do_property("battery", set,
				POWER_SUPPLY_EXT_PROP_DIRECT_TA_ALERT, value);
		}
#endif
		break;
	case OPCODE_VDM_DISCOVER_GET_VDM_RESP:
		max77705_vdm_message_handler(usbc_data, data, len + OPCODE_SIZE);
		break;
	case OPCODE_READ_SBU:
		max77705_response_sbu_read(usbc_data, data);
		break;
	case OPCODE_VDM_DISCOVER_SET_VDM_REQ:
		vdm_opcode_header = data[1];
		switch (vdm_opcode_header) {
		case 0xFF:
			msg_maxim("This isn't invalid response(OPCODE : 0x48, HEADER : 0xFF)");
			break;
		default:
			memcpy(&vdm_header, &data[2], sizeof(vdm_header));
			vdm_type = vdm_header.BITS.VDM_Type;
			vdm_command = vdm_header.BITS.VDM_command;
			vdm_response = vdm_header.BITS.VDM_command_type;
			msg_maxim("vdm_type[%x], vdm_command[%x], vdm_response[%x]",
				vdm_type, vdm_command, vdm_response);
			switch (vdm_type) {
			case STRUCTURED_VDM:
				if (vdm_response == SEC_UVDM_RESPONDER_ACK) {
					switch (vdm_command) {
					case Discover_Identity:
						msg_maxim("ignore Discover_Identity");
						break;
					case Discover_SVIDs:
						msg_maxim("ignore Discover_SVIDs");
						break;
					case Discover_Modes:
						/* work around. The discover mode irq is not happened */
						if (vdm_header.BITS.Standard_Vendor_ID
										== SAMSUNG_VENDOR_ID) {
							if (usbc_data->send_enter_mode_req == 0) {
								/*Samsung Enter Mode */
								msg_maxim("dex: second enter mode request");
								usbc_data->send_enter_mode_req = 1;
								max77705_vdm_process_set_Dex_enter_mode_req(usbc_data);
							}
						} else
							msg_maxim("ignore Discover_Modes");
						break;
					case Enter_Mode:
						/* work around. The enter mode irq is not happened */
						if (vdm_header.BITS.Standard_Vendor_ID
										== SAMSUNG_VENDOR_ID) {
							usbc_data->is_samsung_accessory_enter_mode = 1;
							msg_maxim("dex mode enter_mode ack status!");
						} else
							msg_maxim("ignore Enter_Mode");
						break;
					case Exit_Mode:
						msg_maxim("ignore Exit_Mode");
						break;
					case Attention:
						msg_maxim("ignore Attention");
						break;
					case Configure:
						break;
					default:
						msg_maxim("vdm_command isn't valid[%x]", vdm_command);
						break;
					};
				} else if (vdm_response == SEC_UVDM_ININIATOR) {
					switch (vdm_command) {
					case Attention:
						/* Attention message is not able to be received via 0x48 OPCode */
						/* Check requested vdm command and responded vdm command */
						{
							/* Read requested vdm command */
							max77705_read_reg(usbc_data->muic, 0x23, &reqd_vdm_command);
							reqd_vdm_command &= 0x1F; /* Command bit, b4...0 */

							if (reqd_vdm_command == Configure) {
								W_DATA = 1 << (usbc_data->dp_selected_pin - 1);
								/* Retry Configure message */
								msg_maxim("Retry Configure message, W_DATA = %x, dp_selected_pin = %d",
										W_DATA, usbc_data->dp_selected_pin);
								max77705_vdm_process_set_DP_configure_mode_req(usbc_data, W_DATA);
							}
						}
						break;
					case Discover_Identity:
					case Discover_SVIDs:
					case Discover_Modes:
					case Enter_Mode:
					case Configure:
					default:
						/* Nothing */
						break;
					};
				} else
					msg_maxim("vdm_response is error value[%x]", vdm_response);
				break;
			case UNSTRUCTURED_VDM:
				max77705_sec_unstructured_message_handler(usbc_data, data, len + OPCODE_SIZE);
				break;
			default:
				msg_maxim("vdm_type isn't valid error");
				break;
			};
			break;
		};
		break;
	case OPCODE_SET_ALTERNATEMODE:
		usbc_data->max77705->set_altmode_en = 1;
		break;
	case OPCODE_READ_SELFTEST:
		max77705_response_selftest_read(usbc_data, data);
		break;
#ifdef CONFIG_MAX77705_GRL_ENABLE
	case OPCODE_GRL_COMMAND:
		max77705_set_forcetrimi(usbc_data);
		break;
#else
	case OPCODE_FW_OPCODE_CLEAR:
		msg_maxim("Cleared FW OPCODE");
		break;
#endif
#ifdef MAX77705_RAM_TEST
	case OPCODE_RAM_TEST_COMMAND:
		msg_maxim("MXIM DEBUG : [%x], [%x], [%x]", data[0], data[1], data[2]);
		if (data[1] == MAX77705_RAM_TEST_SUCCESS && data[2] == MAX77705_RAM_TEST_SUCCESS) {
			msg_maxim("MXIM DEBUG : the RAM Testing is OK");
			usbc_data->ram_test_result = MAX77705_RAM_TEST_RESULT_SUCCESS;
			usbc_data->ram_test_retry = 0;
		} else {
			if (data[1] == MAX77705_RAM_TEST_FAIL && data[2] == MAX77705_RAM_TEST_FAIL) {
				msg_maxim("MXIM DEBUG : the RAM Testing is FAIL : [%x], [%x], [%x]",data[0], data[1],data[2]);
				usbc_data->ram_test_result = MAX77705_RAM_TEST_RESULT_FAIL_USBC_FUELGAUAGE;
				usbc_data->ram_test_retry = 0;
			} else if (data[1] == MAX77705_RAM_TEST_SUCCESS && data[2] == MAX77705_RAM_TEST_FAIL) {
				msg_maxim("MXIM DEBUG : the USBC RAM Testing  is FAIL : [%x], [%x], [%x]",data[0], data[1],data[2]);
				usbc_data->ram_test_result = MAX77705_RAM_TEST_RESULT_FAIL_USBC;
				usbc_data->ram_test_retry = 0;
			} else if (data[1] == MAX77705_RAM_TEST_FAIL && data[2] == MAX77705_RAM_TEST_SUCCESS) {
				msg_maxim("MXIM DEBUG : the FG RAM Testing is FAIL : [%x], [%x], [%x]",data[0], data[1],data[2]);
				usbc_data->ram_test_result = MAX77705_RAM_TEST_RESULT_FAIL_FUELGAUAGE;
				usbc_data->ram_test_retry = 0;
			} else {
				msg_maxim("MXIM DEBUG : the RAM Testing is WRONG : [%x], [%x], [%x], [%x], [%x],mode: [%x], cnt : [%x]",
						data[0], data[1],data[2], data[3], data[4],
						usbc_data->ram_test_enable,usbc_data->ram_test_retry);
				if (usbc_data->ram_test_enable == MAX77705_RAM_TEST_START_MODE) {
					usbc_data->ram_test_retry = MAX77705_RAM_TEST_RETRY_COUNT;
					usbc_data->ram_test_enable = MAX77705_RAM_TEST_RETRY_MODE;
				}
				if (!usbc_data->ram_test_retry) {
					usbc_data->ram_test_enable = MAX77705_RAM_TEST_STOP_MODE;
					usbc_data->ram_test_retry = 0;
					msg_maxim("MXIM DEBUG : the RAM Testing is FAIL : [%x], [%x], [%x], [%x], [%x], cnt : [%x]",
						data[0], data[1],data[2],data[3], data[4],usbc_data->ram_test_retry);
				}
				usbc_data->ram_test_retry--;
			}

		}
		break;
#endif
	case OPCODE_CTRLREG3_R:
			max77705_control3_read_complete(usbc_data, data);
		break;
	case OPCODE_SAMSUNG_GPIO5_CONTROL:
		max77705_usbc_gpio5_read_complete(usbc_data, data);
		break;
#if defined(CONFIG_SUPPORT_SHIP_MODE)
	case OPCODE_SAMSUNG_SHIPMODE_EN:
		if (data[2] == 0x01)
			usbc_data->ship_mode_en = 1;
		pr_info("%s: Response SHIPMODE_EN, [0x%x], [0x%x:%s], ship_mode_en(%d)\n",
			__func__, data[1], data[2], (data[2] == 0x01) ? "Enable" : "Fail",
			usbc_data->ship_mode_en);
		break;
#endif
	default:
		break;
	}
}

void max77705_usbc_dequeue_queue(struct max77705_usbc_platform_data *usbc_data)
{
	usbc_cmd_data cmd_data;
	usbc_cmd_queue_t *cmd_queue = NULL;

	cmd_queue = &(usbc_data->usbc_cmd_queue);

	init_usbc_cmd_data(&cmd_data);

	if (is_empty_usbc_cmd_queue(cmd_queue)) {
		msg_maxim("Queue, Empty");
		return;
	}

	dequeue_usbc_cmd(cmd_queue, &cmd_data);
	msg_maxim("!! Dequeue queue : opcode : %x, 1st data : %x. 2st data : %x",
		cmd_data.write_data[0],
		cmd_data.read_data[0],
		cmd_data.val);
}

#if !defined (MAX77705_GRL_ENABLE)
static void max77705_usbc_clear_fw_queue(struct max77705_usbc_platform_data *usbc_data)
{
	usbc_cmd_data write_data;

	msg_maxim("called");

	init_usbc_cmd_data(&write_data);
	write_data.opcode = OPCODE_FW_OPCODE_CLEAR;
	max77705_usbc_opcode_write(usbc_data, &write_data);
}
#endif

void max77705_usbc_clear_queue(struct max77705_usbc_platform_data *usbc_data)
{
	usbc_cmd_data cmd_data;
	usbc_cmd_queue_t *cmd_queue = NULL;

	mutex_lock(&usbc_data->op_lock);
	msg_maxim("IN");
	cmd_queue = &(usbc_data->usbc_cmd_queue);

	while (!is_empty_usbc_cmd_queue(cmd_queue)) {
		init_usbc_cmd_data(&cmd_data);
		dequeue_usbc_cmd(cmd_queue, &cmd_data);
		if (max77705_check_recover_opcode(cmd_data.opcode))
			usbc_data->recover_opcode_list[cmd_data.opcode]
				= usbc_data->need_recover = true;
	}
	usbc_data->opcode_stamp = 0;
	msg_maxim("OUT");
	mutex_unlock(&usbc_data->op_lock);
#if !defined (MAX77705_GRL_ENABLE)
	/* also clear fw opcode queue to sync with driver */
	max77705_usbc_clear_fw_queue(usbc_data);
#endif
}

static void max77705_usbc_cmd_run(struct max77705_usbc_platform_data *usbc_data)
{
	usbc_cmd_queue_t *cmd_queue = NULL;
	usbc_cmd_node *run_node;
	usbc_cmd_data cmd_data;
	int ret = 0;

	cmd_queue = &(usbc_data->usbc_cmd_queue);


	run_node = kzalloc(sizeof(usbc_cmd_node), GFP_KERNEL);
	if (!run_node) {
		msg_maxim("failed to allocate muic command queue");
		return;
	}

	init_usbc_cmd_node(run_node);

	init_usbc_cmd_data(&cmd_data);

	if (is_empty_usbc_cmd_queue(cmd_queue)) {
		msg_maxim("Queue, Empty");
		kfree(run_node);
		return;
	}

	dequeue_usbc_cmd(cmd_queue, &cmd_data);

	if (is_usbc_notifier_opcode(cmd_data.opcode)) {
		max77705_notify_execute(usbc_data, &cmd_data);
		max77705_usbc_cmd_run(usbc_data);
	} else if (cmd_data.opcode == OPCODE_NONE) {/* Apcmdres isr */
		msg_maxim("Apcmdres ISR !!!");
		max77705_irq_execute(usbc_data, &cmd_data);
		usbc_data->opcode_stamp = 0;
		max77705_usbc_cmd_run(usbc_data);
	} else { /* No ISR */
		msg_maxim("No ISR");
		copy_usbc_cmd_data(&cmd_data, &(usbc_data->last_opcode));
		ret = max77705_i2c_opcode_write(usbc_data, cmd_data.opcode,
				cmd_data.write_length, cmd_data.write_data);
		if (ret < 0) {
			msg_maxim("i2c write fail. dequeue opcode");
			max77705_usbc_dequeue_queue(usbc_data);
		}
	}
	kfree(run_node);
}

void max77705_usbc_opcode_write(struct max77705_usbc_platform_data *usbc_data,
	usbc_cmd_data *write_op)
{
	usbc_cmd_queue_t *cmd_queue = &(usbc_data->usbc_cmd_queue);
	usbc_cmd_data execute_cmd_data;
	usbc_cmd_data current_cmd;

	mutex_lock(&usbc_data->op_lock);
	init_usbc_cmd_data(&current_cmd);

	/* the messages sent to USBC. */
	init_usbc_cmd_data(&execute_cmd_data);
	execute_cmd_data.opcode = write_op->opcode;
	execute_cmd_data.write_length = write_op->write_length;
	execute_cmd_data.is_uvdm = write_op->is_uvdm;
	memcpy(execute_cmd_data.write_data, write_op->write_data, OPCODE_DATA_LENGTH);
	execute_cmd_data.seq = OPCODE_WRITE_SEQ;
	enqueue_usbc_cmd(cmd_queue, &execute_cmd_data);

	/* the messages recevied From USBC. */
	init_usbc_cmd_data(&execute_cmd_data);
	execute_cmd_data.response = write_op->opcode;
	execute_cmd_data.read_length = write_op->read_length;
	execute_cmd_data.is_uvdm = write_op->is_uvdm;
	execute_cmd_data.seq = OPCODE_WRITE_SEQ;
	enqueue_usbc_cmd(cmd_queue, &execute_cmd_data);

	msg_maxim("W->W opcode[0x%02x] write_length[%d] read_length[%d]",
		write_op->opcode, write_op->write_length, write_op->read_length);

	front_usbc_cmd(cmd_queue, &current_cmd);
	if (current_cmd.opcode == write_op->opcode)
		max77705_usbc_cmd_run(usbc_data);
	else {
		msg_maxim("!!!current_cmd.opcode [0x%02x][0x%02x], read_op->opcode[0x%02x]",
			current_cmd.opcode, current_cmd.response, write_op->opcode);
		if (usbc_data->opcode_stamp != 0 && current_cmd.opcode == OPCODE_NONE) {
			if (time_after(jiffies,
					usbc_data->opcode_stamp + MAX77705_MAX_APDCMD_TIME)) {
				usbc_data->opcode_stamp = 0;
				msg_maxim("error. we will dequeue response data");
				max77705_usbc_dequeue_queue(usbc_data);
				max77705_usbc_cmd_run(usbc_data);
			}
		}
	}
	mutex_unlock(&usbc_data->op_lock);
}

void max77705_usbc_opcode_read(struct max77705_usbc_platform_data *usbc_data,
	usbc_cmd_data *read_op)
{
	usbc_cmd_queue_t *cmd_queue = &(usbc_data->usbc_cmd_queue);
	usbc_cmd_data execute_cmd_data;
	usbc_cmd_data current_cmd;

	mutex_lock(&usbc_data->op_lock);
	init_usbc_cmd_data(&current_cmd);

	/* the messages sent to USBC. */
	init_usbc_cmd_data(&execute_cmd_data);
	execute_cmd_data.opcode = read_op->opcode;
	execute_cmd_data.write_length = read_op->write_length;
	execute_cmd_data.is_uvdm = read_op->is_uvdm;
	memcpy(execute_cmd_data.write_data, read_op->write_data, read_op->write_length);
	execute_cmd_data.seq = OPCODE_READ_SEQ;
	enqueue_usbc_cmd(cmd_queue, &execute_cmd_data);

	/* the messages recevied From USBC. */
	init_usbc_cmd_data(&execute_cmd_data);
	execute_cmd_data.response = read_op->opcode;
	execute_cmd_data.read_length = read_op->read_length;
	execute_cmd_data.is_uvdm = read_op->is_uvdm;
	execute_cmd_data.seq = OPCODE_READ_SEQ;
	enqueue_usbc_cmd(cmd_queue, &execute_cmd_data);

	msg_maxim("R->R opcode[0x%02x] write_length[%d] read_length[%d]",
		read_op->opcode, read_op->write_length, read_op->read_length);

	front_usbc_cmd(cmd_queue, &current_cmd);
	if (current_cmd.opcode == read_op->opcode)
		max77705_usbc_cmd_run(usbc_data);
	else {
		msg_maxim("!!!current_cmd.opcode [0x%02x][0x%02x], read_op->opcode[0x%02x]",
			current_cmd.opcode, current_cmd.response, read_op->opcode);
		if (usbc_data->opcode_stamp != 0 && current_cmd.opcode == OPCODE_NONE) {
			if (time_after(jiffies,
					usbc_data->opcode_stamp + MAX77705_MAX_APDCMD_TIME)) {
				usbc_data->opcode_stamp = 0;
				msg_maxim("error. we will dequeue response data");
				max77705_usbc_dequeue_queue(usbc_data);
				max77705_usbc_cmd_run(usbc_data);
			}
		}
	}

	mutex_unlock(&usbc_data->op_lock);
}

void max77705_usbc_opcode_update(struct max77705_usbc_platform_data *usbc_data,
	usbc_cmd_data *update_op)
{
	usbc_cmd_queue_t *cmd_queue = &(usbc_data->usbc_cmd_queue);
	usbc_cmd_data execute_cmd_data;
	usbc_cmd_data current_cmd;

	switch (update_op->opcode) {
	case OPCODE_BCCTRL1_R:
	case OPCODE_BCCTRL2_R:
	case OPCODE_CTRL1_R:
	case OPCODE_CTRL2_R:
	case OPCODE_CTRL3_R:
	case OPCODE_CCCTRL1_R:
	case OPCODE_CCCTRL2_R:
	case OPCODE_CCCTRL3_R:
	case OPCODE_HVCTRL_R:
	case OPCODE_OPCODE_VCONN_ILIM_R:
	case OPCODE_CHGIN_ILIM_R:
	case OPCODE_CHGIN_ILIM2_R:
		break;
	default:
		pr_err("%s: invalid usage(0x%x), return\n", __func__, update_op->opcode);
		return;
	}

	mutex_lock(&usbc_data->op_lock);
	init_usbc_cmd_data(&current_cmd);

	/* the messages sent to USBC. */
	init_usbc_cmd_data(&execute_cmd_data);
	execute_cmd_data.opcode = update_op->opcode;
	execute_cmd_data.write_length = 0;
	execute_cmd_data.is_uvdm = update_op->is_uvdm;
	memcpy(execute_cmd_data.write_data, update_op->write_data, update_op->write_length);
	execute_cmd_data.seq = OPCODE_UPDATE_SEQ;
	enqueue_usbc_cmd(cmd_queue, &execute_cmd_data);

	/* the messages recevied From USBC. */
	init_usbc_cmd_data(&execute_cmd_data);
	execute_cmd_data.response = update_op->opcode;
	execute_cmd_data.read_length = 1;
	execute_cmd_data.seq = OPCODE_UPDATE_SEQ;
	execute_cmd_data.val = update_op->val;
	execute_cmd_data.mask = update_op->mask;
	execute_cmd_data.is_uvdm = update_op->is_uvdm;
	enqueue_usbc_cmd(cmd_queue, &execute_cmd_data);

	msg_maxim("U->U opcode[0x%02x] write_length[%d] read_length[%d]",
		update_op->opcode, update_op->write_length, update_op->read_length);

	front_usbc_cmd(cmd_queue, &current_cmd);
	if (current_cmd.opcode == update_op->opcode)
		max77705_usbc_cmd_run(usbc_data);
	else {
		msg_maxim("!!! current_cmd.opcode [0x%02x], update_op->opcode[0x%02x]",
			current_cmd.opcode, update_op->opcode);
		if (usbc_data->opcode_stamp != 0 && current_cmd.opcode == OPCODE_NONE) {
			if (time_after(jiffies,
					usbc_data->opcode_stamp + MAX77705_MAX_APDCMD_TIME)) {
				usbc_data->opcode_stamp = 0;
				msg_maxim("error. we will dequeue response data");
				max77705_usbc_dequeue_queue(usbc_data);
				max77705_usbc_cmd_run(usbc_data);
			}
		}
	}

	mutex_unlock(&usbc_data->op_lock);
}

void max77705_usbc_opcode_push(struct max77705_usbc_platform_data *usbc_data,
	usbc_cmd_data *read_op)
{
	usbc_cmd_queue_t *cmd_queue = &(usbc_data->usbc_cmd_queue);
	usbc_cmd_data execute_cmd_data;
	usbc_cmd_data current_cmd;

	init_usbc_cmd_data(&current_cmd);

	/* the messages sent to USBC. */
	init_usbc_cmd_data(&execute_cmd_data);
	execute_cmd_data.opcode = read_op->opcode;
	execute_cmd_data.write_length = read_op->write_length;
	execute_cmd_data.is_uvdm = read_op->is_uvdm;
	memcpy(execute_cmd_data.write_data, read_op->write_data, read_op->write_length);
	execute_cmd_data.seq = OPCODE_PUSH_SEQ;
	enqueue_usbc_cmd(cmd_queue, &execute_cmd_data);

	/* the messages recevied From USBC. */
	init_usbc_cmd_data(&execute_cmd_data);
	execute_cmd_data.response = read_op->opcode;
	execute_cmd_data.read_length = read_op->read_length;
	execute_cmd_data.is_uvdm = read_op->is_uvdm;
	execute_cmd_data.seq = OPCODE_PUSH_SEQ;
	enqueue_usbc_cmd(cmd_queue, &execute_cmd_data);

	msg_maxim("P->P opcode[0x%02x] write_length[%d] read_length[%d]",
		read_op->opcode, read_op->write_length, read_op->read_length);
}

void max77705_usbc_opcode_rw(struct max77705_usbc_platform_data *usbc_data,
	usbc_cmd_data *read_op, usbc_cmd_data *write_op)
{
	usbc_cmd_queue_t *cmd_queue = &(usbc_data->usbc_cmd_queue);
	usbc_cmd_data execute_cmd_data;
	usbc_cmd_data current_cmd;

	mutex_lock(&usbc_data->op_lock);
	init_usbc_cmd_data(&current_cmd);

	/* the messages sent to USBC. */
	init_usbc_cmd_data(&execute_cmd_data);
	execute_cmd_data.opcode = read_op->opcode;
	execute_cmd_data.write_length = read_op->write_length;
	execute_cmd_data.is_uvdm = read_op->is_uvdm;
	memcpy(execute_cmd_data.write_data, read_op->write_data, read_op->write_length);
	execute_cmd_data.seq = OPCODE_RW_SEQ;
	enqueue_usbc_cmd(cmd_queue, &execute_cmd_data);

	/* the messages recevied From USBC. */
	init_usbc_cmd_data(&execute_cmd_data);
	execute_cmd_data.response = read_op->opcode;
	execute_cmd_data.read_length = read_op->read_length;
	execute_cmd_data.is_uvdm = read_op->is_uvdm;
	execute_cmd_data.seq = OPCODE_RW_SEQ;
	enqueue_usbc_cmd(cmd_queue, &execute_cmd_data);

	/* the messages sent to USBC. */
	init_usbc_cmd_data(&execute_cmd_data);
	execute_cmd_data.opcode = write_op->opcode;
	execute_cmd_data.write_length = write_op->write_length;
	execute_cmd_data.is_uvdm = write_op->is_uvdm;
	memcpy(execute_cmd_data.write_data, write_op->write_data, OPCODE_DATA_LENGTH);
	execute_cmd_data.seq = OPCODE_RW_SEQ;
	enqueue_usbc_cmd(cmd_queue, &execute_cmd_data);

	/* the messages recevied From USBC. */
	init_usbc_cmd_data(&execute_cmd_data);
	execute_cmd_data.response = write_op->opcode;
	execute_cmd_data.read_length = write_op->read_length;
	execute_cmd_data.is_uvdm = write_op->is_uvdm;
	execute_cmd_data.seq = OPCODE_RW_SEQ;
	enqueue_usbc_cmd(cmd_queue, &execute_cmd_data);

	msg_maxim("RW->R opcode[0x%02x] write_length[%d] read_length[%d]",
		read_op->opcode, read_op->write_length, read_op->read_length);
	msg_maxim("RW->W opcode[0x%02x] write_length[%d] read_length[%d]",
		write_op->opcode, write_op->write_length, write_op->read_length);

	front_usbc_cmd(cmd_queue, &current_cmd);
	if (current_cmd.opcode == read_op->opcode)
		max77705_usbc_cmd_run(usbc_data);
	else {
		msg_maxim("!!! current_cmd.opcode [0x%02x], read_op->opcode[0x%02x]",
			current_cmd.opcode, read_op->opcode);
		if (usbc_data->opcode_stamp != 0 && current_cmd.opcode == OPCODE_NONE) {
			if (time_after(jiffies,
					usbc_data->opcode_stamp + MAX77705_MAX_APDCMD_TIME)) {
				usbc_data->opcode_stamp = 0;
				msg_maxim("error. we will dequeue response data");
				max77705_usbc_dequeue_queue(usbc_data);
				max77705_usbc_cmd_run(usbc_data);
			}
		}
	}

	mutex_unlock(&usbc_data->op_lock);
}

/*
 * max77705_uic_op_send_work_func func - Send OPCode
 * @work: work_struct of max77705_i2c
 *
 * Wait for OPCode response
 */
static void max77705_uic_op_send_work_func(
		struct work_struct *work)
{
	struct max77705_usbc_platform_data *usbc_data;
	struct max77705_opcode opcodes[] = {
		{
			.opcode = OPCODE_BCCTRL1_R,
			.data = { 0, },
			.write_length = 0,
			.read_length = 1,
		},
		{
			.opcode = OPCODE_BCCTRL1_W,
			.data = { 0x20, },
			.write_length = 1,
			.read_length = 0,
		},
		{
			.opcode = OPCODE_BCCTRL2_R,
			.data = { 0, },
			.write_length = 0,
			.read_length = 1,
		},
		{
			.opcode = OPCODE_BCCTRL2_W,
			.data = { 0x10, },
			.write_length = 1,
			.read_length = 0,
		},
		{
			.opcode = OPCODE_CURRENT_SRCCAP,
			.data = { 0, },
			.write_length = 1,
			.read_length = 4,
		},
		{
			.opcode = OPCODE_AFC_HV_W,
			.data = { 0x46, },
			.write_length = 1,
			.read_length = 2,
		},
		{
			.opcode = OPCODE_SRCCAP_REQUEST,
			.data = { 0x00, },
			.write_length = 1,
			.read_length = 32,
		},
	};
	int op_loop;

	usbc_data = container_of(work, struct max77705_usbc_platform_data, op_send_work);

	msg_maxim("IN");
	for (op_loop = 0; op_loop < ARRAY_SIZE(opcodes); op_loop++) {
		if (usbc_data->op_code == opcodes[op_loop].opcode) {
			max77705_i2c_opcode_write(usbc_data, opcodes[op_loop].opcode,
				opcodes[op_loop].write_length, opcodes[op_loop].data);
			break;
		}
	}
	msg_maxim("OUT");
}

static void max77705_reset_ic(struct max77705_usbc_platform_data *usbc_data)
{
	struct max77705_dev *max77705 = usbc_data->max77705;

	//gurantee to block i2c trasaction during ccic reset
	mutex_lock(&max77705->i2c_lock);
	max77705_write_reg_nolock(usbc_data->muic, 0x80, 0x0F);
	msleep(100); /* need 100ms delay */
	mutex_unlock(&max77705->i2c_lock);
}

void max77705_usbc_check_sysmsg(struct max77705_usbc_platform_data *usbc_data, u8 sysmsg)
{
	usbc_cmd_queue_t *cmd_queue = &(usbc_data->usbc_cmd_queue);
	bool is_empty_queue = is_empty_usbc_cmd_queue(cmd_queue);
	usbc_cmd_data cmd_data;
	usbc_cmd_data next_cmd_data;
	u8 next_opcode = 0xFF;
	u8 interrupt;
#if defined(CONFIG_USB_HW_PARAM)
	struct otg_notify *o_notify = get_otg_notify();
#endif
#ifdef CONFIG_USB_NOTIFY_PROC_LOG
	int event;
#endif
	int ret = 0;

	if (usbc_data->shut_down) {
		msg_maxim("IGNORE SYSTEM_MSG IN SHUTDOWN MODE!!");
		return;
	}

	switch (sysmsg) {
	case SYSERROR_NONE:
		break;
	case SYSERROR_BOOT_WDT:
		usbc_data->watchdog_count++;
		msg_maxim("SYSERROR_BOOT_WDT: %d", usbc_data->watchdog_count);
		max77705_usbc_mask_irq(usbc_data);
		max77705_write_reg(usbc_data->muic, REG_UIC_INT_M, REG_UIC_INT_M_INIT);
		max77705_write_reg(usbc_data->muic, REG_CC_INT_M, REG_CC_INT_M_INIT);
		max77705_write_reg(usbc_data->muic, REG_PD_INT_M, REG_PD_INT_M_INIT);
		max77705_write_reg(usbc_data->muic, REG_VDM_INT_M, REG_VDM_INT_M_INIT);
		/* clear UIC_INT to prevent infinite sysmsg irq*/
	        g_usbc_data->max77705->enable_nested_irq = 1;
		max77705_read_reg(usbc_data->muic, MAX77705_USBC_REG_UIC_INT, &interrupt);
		g_usbc_data->max77705->usbc_irq = interrupt & 0xBF; //clear the USBC SYSTEM IRQ
		max77705_usbc_clear_queue(usbc_data);
		usbc_data->is_first_booting = 1;
		max77705_init_opcode(usbc_data, 1);
		max77705_usbc_umask_irq(usbc_data);
#ifdef MAX77705_RAM_TEST
		if(usbc_data->ram_test_enable == MAX77705_RAM_TEST_RETRY_MODE) {
			mdelay(100);
			max77705_verify_ram_bist_write(usbc_data);
		} else {
			usbc_data->ram_test_enable = MAX77705_RAM_TEST_STOP_MODE;
		}
#endif
#ifdef CONFIG_USB_NOTIFY_PROC_LOG
		event = NOTIFY_EXTRA_SYSERROR_BOOT_WDT;
		store_usblog_notify(NOTIFY_EXTRA, (void *)&event, NULL);
#endif
		break;
	case SYSERROR_BOOT_SWRSTREQ:
		break;
	case SYSMSG_BOOT_POR:
		usbc_data->por_count++;
		max77705_usbc_mask_irq(usbc_data);
		max77705_reset_ic(usbc_data);
		max77705_write_reg(usbc_data->muic, REG_UIC_INT_M, REG_UIC_INT_M_INIT);
		max77705_write_reg(usbc_data->muic, REG_CC_INT_M, REG_CC_INT_M_INIT);
		max77705_write_reg(usbc_data->muic, REG_PD_INT_M, REG_PD_INT_M_INIT);
		max77705_write_reg(usbc_data->muic, REG_VDM_INT_M, REG_VDM_INT_M_INIT);
		/* clear UIC_INT to prevent infinite sysmsg irq*/
	        g_usbc_data->max77705->enable_nested_irq = 1;
		max77705_read_reg(usbc_data->muic, MAX77705_USBC_REG_UIC_INT, &interrupt);
		g_usbc_data->max77705->usbc_irq = interrupt & 0xBF; //clear the USBC SYSTEM IRQ
		msg_maxim("SYSERROR_BOOT_POR: %d, UIC_INT:0x%02x", usbc_data->por_count, interrupt);
		max77705_usbc_clear_queue(usbc_data);
		usbc_data->is_first_booting = 1;
		max77705_init_opcode(usbc_data, 1);
		max77705_usbc_umask_irq(usbc_data);
#ifdef CONFIG_USB_NOTIFY_PROC_LOG
		event = NOTIFY_EXTRA_SYSMSG_BOOT_POR;
		store_usblog_notify(NOTIFY_EXTRA, (void *)&event, NULL);
#endif
		break;
	case SYSERROR_HV_NOVBUS:
		break;
	case SYSERROR_HV_FMETHOD_RXPERR:
		break;
	case SYSERROR_HV_FMETHOD_RXBUFOW:
		break;
	case SYSERROR_HV_FMETHOD_RXTFR:
		break;
	case SYSERROR_HV_FMETHOD_MPNACK:
		break;
	case SYSERROR_HV_FMETHOD_RESET_FAIL:
		break;
	case SYSMsg_AFC_Done:
		break;
	case SYSERROR_SYSPOS:
		break;
	case SYSERROR_APCMD_UNKNOWN:
		break;
	case SYSERROR_APCMD_INPROGRESS:
		break;
	case SYSERROR_APCMD_FAIL:

		init_usbc_cmd_data(&cmd_data);
		init_usbc_cmd_data(&next_cmd_data);

		if (front_usbc_cmd(cmd_queue, &next_cmd_data))
			next_opcode = next_cmd_data.response;

		if (!is_empty_queue) {
			copy_usbc_cmd_data(&(usbc_data->last_opcode), &cmd_data);

#ifdef CONFIG_MAX77705_GRL_ENABLE
			if (cmd_data.opcode == OPCODE_GRL_COMMAND || next_opcode == OPCODE_VDM_DISCOVER_SET_VDM_REQ) {
#else
			if (next_opcode == OPCODE_VDM_DISCOVER_SET_VDM_REQ) {
#endif
				usbc_data->opcode_stamp = 0;
				max77705_usbc_dequeue_queue(usbc_data);
				cmd_data.opcode = OPCODE_NONE;
			}

			if ((cmd_data.opcode != OPCODE_NONE) && (cmd_data.opcode == next_opcode)) {
				if (next_opcode != OPCODE_VDM_DISCOVER_SET_VDM_REQ) {
					ret = max77705_i2c_opcode_write(usbc_data,
						cmd_data.opcode,
						cmd_data.write_length,
						cmd_data.write_data);
					if (ret) {
						msg_maxim("i2c write fail. dequeue opcode");
						max77705_usbc_dequeue_queue(usbc_data);
					} else
						msg_maxim("RETRY SUCCESS : %x, %x", cmd_data.opcode, next_opcode);
				} else
					msg_maxim("IGNORE COMMAND : %x, %x", cmd_data.opcode, next_opcode);
			} else {
				msg_maxim("RETRY FAILED : %x, %x", cmd_data.opcode, next_opcode);
			}

		}

#if IS_ENABLED(CONFIG_HV_MUIC_MAX77705_AFC)
		max77705_muic_disable_afc_protocol(usbc_data->muic_data);
#endif
		/* TO DO DEQEUE MSG. */
		break;
#ifdef CONFIG_MAX77705_GRL_ENABLE
	case SYSMSG_SET_GRL:
		max77705_usbc_clear_queue(usbc_data);
		msg_maxim("SYSTEM MESSAGE GRL COMMAND!!!");

		init_usbc_cmd_data(&cmd_data);
		cmd_data.opcode = OPCODE_GRL_COMMAND;
		cmd_data.write_data[0] = 0x1;
		cmd_data.write_length = 0x1;
		cmd_data.read_length = 0x2;
		max77705_usbc_opcode_write(usbc_data, &cmd_data);
//		max77705_set_forcetrimi(usbc_data);

		break;
#endif
	case SYSMSG_CCx_5V_SHORT:
		msg_maxim("CC-VBUS SHORT");
#if defined(CONFIG_USB_HW_PARAM)
		if (o_notify)
			inc_hw_param(o_notify, USB_CCIC_VBUS_CC_SHORT_COUNT);
#endif
#ifdef CONFIG_USB_NOTIFY_PROC_LOG
		event = NOTIFY_EXTRA_SYSMSG_CC_SHORT;
		store_usblog_notify(NOTIFY_EXTRA, (void *)&event, NULL);
#endif
		usbc_data->cc_data->ccistat = CCI_SHORT;
		max77705_notify_rp_current_level(usbc_data);
		break;
	case SYSMSG_SBUx_GND_SHORT:
		msg_maxim("SBU-GND SHORT");
#if defined(CONFIG_USB_HW_PARAM)
		if (o_notify)
			inc_hw_param(o_notify, USB_CCIC_GND_SBU_SHORT_COUNT);
#endif
#ifdef CONFIG_USB_NOTIFY_PROC_LOG
		event = NOTIFY_EXTRA_SYSMSG_SBU_GND_SHORT;
		store_usblog_notify(NOTIFY_EXTRA, (void *)&event, NULL);
#endif
		break;
	case SYSMSG_SBUx_5V_SHORT:
		msg_maxim("SBU-VBUS SHORT");
#if defined(CONFIG_USB_HW_PARAM)
		if (o_notify)
			inc_hw_param(o_notify, USB_CCIC_VBUS_SBU_SHORT_COUNT);
#endif
#ifdef CONFIG_USB_NOTIFY_PROC_LOG
		event = NOTIFY_EXTRA_SYSMSG_SBU_VBUS_SHORT;
		store_usblog_notify(NOTIFY_EXTRA, (void *)&event, NULL);
#endif
		break;
	case SYSMSG_PD_CCx_5V_SHORT:
		msg_maxim("PD_CC-VBUS SHORT");
		usbc_data->pd_data->cc_sbu_short = true;
		break;
	case SYSMSG_PD_SBUx_5V_SHORT:
		msg_maxim("PD_SBU-VBUS SHORT");
		usbc_data->pd_data->cc_sbu_short = true;
		break;
	case SYSMSG_PD_SHORT_NONE:
		msg_maxim("Cable detach");
		usbc_data->pd_data->cc_sbu_short = false;
		break;
	case SYSERROR_DROP5V_SRCRDY:
		msg_maxim("vbus drop during source ready");
		break;
	case SYSERROR_DROP5V_SNKRDY:
		msg_maxim("vbus drop during sink ready");
		break;
	case SYSMSG_PD_GENDER_SHORT:
		msg_maxim("PD_GENDER_SHORT");
		usbc_data->pd_data->cc_sbu_short = true;
		break;
	case SYSERROR_POWER_NEGO:
		if (!usbc_data->last_opcode.is_uvdm) { /* structured vdm */
			if (usbc_data->last_opcode.opcode == OPCODE_VDM_DISCOVER_SET_VDM_REQ) {
				max77705_usbc_opcode_write(usbc_data, &usbc_data->last_opcode);
				msg_maxim("SYSMSG PWR NEGO ERR : VDM request retry");
			}
		} else { /* unstructured vdm */
			usbc_data->uvdm_error = -EACCES;
			msg_maxim("SYSMSG PWR NEGO ERR : UVDM request error - dir : %d",
				usbc_data->is_in_sec_uvdm_out);
			if (usbc_data->is_in_sec_uvdm_out == DIR_OUT)
				complete(&usbc_data->uvdm_longpacket_out_wait);
			else if (usbc_data->is_in_sec_uvdm_out == DIR_IN)
				complete(&usbc_data->uvdm_longpacket_in_wait);
			else
				;
		}
		break;
#if defined(CONFIG_SEC_FACTORY)
	case SYSERROR_FACTORY_RID0:
		factory_execute_monitor(FAC_ABNORMAL_REPEAT_RID0);
		break;
#endif
	case SYSERROR_CCRP_HIGH:
		msg_maxim("CCRP HIGH");
#if IS_ENABLED(CONFIG_PDIC_NOTIFIER)
		if (usbc_data->ccrp_state != 1) {
			usbc_data->ccrp_state = 1;
			max77705_ccic_event_work(usbc_data,
				PDIC_NOTIFY_DEV_BATT, PDIC_NOTIFY_ID_WATER_CABLE,
				PDIC_NOTIFY_ATTACH, 0/*rprd*/, 0);
		}
#endif
		break;
	case SYSERROR_CCRP_LOW:
		msg_maxim("CCRP LOW");
#if IS_ENABLED(CONFIG_PDIC_NOTIFIER)
		if (usbc_data->ccrp_state != 0) {
			usbc_data->ccrp_state = 0;
			max77705_ccic_event_work(usbc_data,
				PDIC_NOTIFY_DEV_BATT, PDIC_NOTIFY_ID_WATER_CABLE,
				PDIC_NOTIFY_DETACH, 0/*rprd*/, 0);
		}
#endif
		break;
	case SYSMSG_10K_TO_22K ... SYSMSG_22K_TO_56K:
		msg_maxim("TypeC earphone is attached");
		usbc_data->pd_data->cc_sbu_short = true;
		max77705_check_pdo(usbc_data);
		break;
	case SYSMSG_56K_TO_22K ... SYSMSG_22K_TO_10K:
		msg_maxim("TypeC earphone is detached");
		usbc_data->pd_data->cc_sbu_short = false;
		max77705_check_pdo(usbc_data);
		break;
	default:
		break;
	}
}


static irqreturn_t max77705_apcmd_irq(int irq, void *data)
{
	struct max77705_usbc_platform_data *usbc_data = data;
	u8 sysmsg = 0;

	msg_maxim("IRQ(%d)_IN", irq);
	max77705_read_reg(usbc_data->muic, REG_USBC_STATUS2, &usbc_data->usbc_status2);
	sysmsg = usbc_data->usbc_status2;
	msg_maxim(" [IN] sysmsg : %d", sysmsg);

	mutex_lock(&usbc_data->op_lock);
	max77705_usbc_cmd_run(usbc_data);
	mutex_unlock(&usbc_data->op_lock);

	if (usbc_data->need_recover) {
		max77705_recover_opcode(usbc_data,
			usbc_data->recover_opcode_list);
		usbc_data->need_recover = false;
	}

	msg_maxim("IRQ(%d)_OUT", irq);

	return IRQ_HANDLED;
}

static irqreturn_t max77705_sysmsg_irq(int irq, void *data)
{
	struct max77705_usbc_platform_data *usbc_data = data;
	u8 sysmsg = 0;
	u8 i = 0;
	u8 raw_data[3] = {0, };
	u8 usbc_status2 = 0;
	u8 dump_reg[10] = {0, };

	for (i = 0; i < 3; i++) {
		usbc_status2 = 0;
		max77705_read_reg(usbc_data->muic, REG_USBC_STATUS2, &usbc_status2);
		raw_data[i] = usbc_status2;
	}
	if((raw_data[0] == raw_data[1]) && (raw_data[0] == raw_data[2])){
		sysmsg = raw_data[0];
	} else {
		max77705_bulk_read(usbc_data->muic, REG_USBC_STATUS1,
				8, dump_reg);
		msg_maxim("[ERROR ]sys_reg, %x, %x, %x", raw_data[0], raw_data[1],raw_data[2]);
		msg_maxim("[ERROR ]dump_reg, %x, %x, %x, %x, %x, %x, %x, %x\n", dump_reg[0], dump_reg[1],
			dump_reg[2], dump_reg[3], dump_reg[4], dump_reg[5], dump_reg[6], dump_reg[7]);
		sysmsg = 0x6D;
	}
	msg_maxim("IRQ(%d)_IN sysmsg: %x", irq, sysmsg);
	max77705_usbc_check_sysmsg(usbc_data, sysmsg);
	usbc_data->sysmsg = sysmsg;
	msg_maxim("IRQ(%d)_OUT sysmsg: %x", irq, sysmsg);

	return IRQ_HANDLED;
}


static irqreturn_t max77705_vdm_identity_irq(int irq, void *data)
{
	struct max77705_usbc_platform_data *usbc_data = data;
	MAX77705_VDM_MSG_IRQ_STATUS_Type VDM_MSG_IRQ_State;

	memset(&VDM_MSG_IRQ_State, 0, sizeof(VDM_MSG_IRQ_State));
	msg_maxim("IRQ(%d)_IN", irq);
	VDM_MSG_IRQ_State.BITS.Vdm_Flag_Discover_ID = 1;
	max77705_receive_alternate_message(usbc_data, &VDM_MSG_IRQ_State);
	msg_maxim("IRQ(%d)_OUT", irq);

	return IRQ_HANDLED;
}

static irqreturn_t max77705_vdm_svids_irq(int irq, void *data)
{
	struct max77705_usbc_platform_data *usbc_data = data;
	MAX77705_VDM_MSG_IRQ_STATUS_Type VDM_MSG_IRQ_State;

	memset(&VDM_MSG_IRQ_State, 0, sizeof(VDM_MSG_IRQ_State));
	msg_maxim("IRQ(%d)_IN", irq);
	VDM_MSG_IRQ_State.BITS.Vdm_Flag_Discover_SVIDs = 1;
	max77705_receive_alternate_message(usbc_data, &VDM_MSG_IRQ_State);
	msg_maxim("IRQ(%d)_OUT", irq);
	return IRQ_HANDLED;
}

static irqreturn_t max77705_vdm_discover_mode_irq(int irq, void *data)
{
	struct max77705_usbc_platform_data *usbc_data = data;
	MAX77705_VDM_MSG_IRQ_STATUS_Type VDM_MSG_IRQ_State;

	memset(&VDM_MSG_IRQ_State, 0, sizeof(VDM_MSG_IRQ_State));
	msg_maxim("IRQ(%d)_IN", irq);
	VDM_MSG_IRQ_State.BITS.Vdm_Flag_Discover_MODEs = 1;
	max77705_receive_alternate_message(usbc_data, &VDM_MSG_IRQ_State);
	msg_maxim("IRQ(%d)_OUT", irq);
	return IRQ_HANDLED;
}

static irqreturn_t max77705_vdm_enter_mode_irq(int irq, void *data)
{
	struct max77705_usbc_platform_data *usbc_data = data;
	MAX77705_VDM_MSG_IRQ_STATUS_Type VDM_MSG_IRQ_State;

	memset(&VDM_MSG_IRQ_State, 0, sizeof(VDM_MSG_IRQ_State));
	msg_maxim("IRQ(%d)_IN", irq);
	VDM_MSG_IRQ_State.BITS.Vdm_Flag_Enter_Mode = 1;
	max77705_receive_alternate_message(usbc_data, &VDM_MSG_IRQ_State);
	msg_maxim("IRQ(%d)_OUT", irq);
	return IRQ_HANDLED;
}

static irqreturn_t max77705_vdm_dp_status_irq(int irq, void *data)
{
	struct max77705_usbc_platform_data *usbc_data = data;
	MAX77705_VDM_MSG_IRQ_STATUS_Type VDM_MSG_IRQ_State;

	memset(&VDM_MSG_IRQ_State, 0, sizeof(VDM_MSG_IRQ_State));
	msg_maxim("IRQ(%d)_IN", irq);
	VDM_MSG_IRQ_State.BITS.Vdm_Flag_DP_Status_Update = 1;
	max77705_receive_alternate_message(usbc_data, &VDM_MSG_IRQ_State);
	msg_maxim("IRQ(%d)_OUT", irq);
	return IRQ_HANDLED;
}

static irqreturn_t max77705_vdm_dp_configure_irq(int irq, void *data)
{
	struct max77705_usbc_platform_data *usbc_data = data;
	MAX77705_VDM_MSG_IRQ_STATUS_Type VDM_MSG_IRQ_State;

	memset(&VDM_MSG_IRQ_State, 0, sizeof(VDM_MSG_IRQ_State));
	msg_maxim("IRQ(%d)_IN", irq);
	VDM_MSG_IRQ_State.BITS.Vdm_Flag_DP_Configure = 1;
	max77705_receive_alternate_message(usbc_data, &VDM_MSG_IRQ_State);
	msg_maxim("IRQ(%d)_OUT", irq);
	return IRQ_HANDLED;
}

static irqreturn_t max77705_vdm_attention_irq(int irq, void *data)
{
	struct max77705_usbc_platform_data *usbc_data = data;
	MAX77705_VDM_MSG_IRQ_STATUS_Type VDM_MSG_IRQ_State;

	memset(&VDM_MSG_IRQ_State, 0, sizeof(VDM_MSG_IRQ_State));
	msg_maxim("IRQ(%d)_IN", irq);
	VDM_MSG_IRQ_State.BITS.Vdm_Flag_Attention = 1;
	max77705_receive_alternate_message(usbc_data, &VDM_MSG_IRQ_State);
	msg_maxim("IRQ(%d)_OUT", irq);
	return IRQ_HANDLED;
}

static irqreturn_t max77705_vir_altmode_irq(int irq, void *data)
{
	struct max77705_usbc_platform_data *usbc_data = data;

	msg_maxim("max77705_vir_altmode_irq");

	if (usbc_data->shut_down) {
		msg_maxim("%s doing shutdown. skip set alternate mode", __func__);
		goto skip;
	}
	
	max77705_set_enable_alternate_mode
		(usbc_data->set_altmode);

skip:	
	return IRQ_HANDLED;
}

int max77705_init_irq_handler(struct max77705_usbc_platform_data *usbc_data)
{
	int ret = 0;

	usbc_data->irq_apcmd = usbc_data->irq_base + MAX77705_USBC_IRQ_APC_INT;
	if (usbc_data->irq_apcmd) {
		ret = request_threaded_irq(usbc_data->irq_apcmd,
			   NULL, max77705_apcmd_irq,
			   0,
			   "usbc-apcmd-irq", usbc_data);
		if (ret) {
			pr_err("%s: Failed to Request IRQ (%d)\n", __func__, ret);
			return ret;
		}
	}

	usbc_data->irq_sysmsg = usbc_data->irq_base + MAX77705_USBC_IRQ_SYSM_INT;
	if (usbc_data->irq_sysmsg) {
		ret = request_threaded_irq(usbc_data->irq_sysmsg,
			   NULL, max77705_sysmsg_irq,
			   0,
			   "usbc-sysmsg-irq", usbc_data);
		if (ret) {
			pr_err("%s: Failed to Request IRQ (%d)\n", __func__, ret);
			return ret;
		}
	}

	usbc_data->irq_vdm0 = usbc_data->irq_base + MAX77705_IRQ_VDM_DISCOVER_ID_INT;
	if (usbc_data->irq_vdm0) {
		ret = request_threaded_irq(usbc_data->irq_vdm0,
			   NULL, max77705_vdm_identity_irq,
			   0,
			   "usbc-vdm0-irq", usbc_data);
		if (ret) {
			pr_err("%s: Failed to Request IRQ (%d)\n", __func__, ret);
			return ret;
		}
	}

	usbc_data->irq_vdm1 = usbc_data->irq_base + MAX77705_IRQ_VDM_DISCOVER_SVIDS_INT;
	if (usbc_data->irq_vdm1) {
		ret = request_threaded_irq(usbc_data->irq_vdm1,
			   NULL, max77705_vdm_svids_irq,
			   0,
			   "usbc-vdm1-irq", usbc_data);
		if (ret) {
			pr_err("%s: Failed to Request IRQ (%d)\n", __func__, ret);
			return ret;
		}
	}

	usbc_data->irq_vdm2 = usbc_data->irq_base + MAX77705_IRQ_VDM_DISCOVER_MODES_INT;
	if (usbc_data->irq_vdm2) {
		ret = request_threaded_irq(usbc_data->irq_vdm2,
			   NULL, max77705_vdm_discover_mode_irq,
			   0,
			   "usbc-vdm2-irq", usbc_data);
		if (ret) {
			pr_err("%s: Failed to Request IRQ (%d)\n", __func__, ret);
			return ret;
		}
	}

	usbc_data->irq_vdm3 = usbc_data->irq_base + MAX77705_IRQ_VDM_ENTER_MODE_INT;
	if (usbc_data->irq_vdm3) {
		ret = request_threaded_irq(usbc_data->irq_vdm3,
			   NULL, max77705_vdm_enter_mode_irq,
			   0,
			   "usbc-vdm3-irq", usbc_data);
		if (ret) {
			pr_err("%s: Failed to Request IRQ (%d)\n", __func__, ret);
			return ret;
		}
	}

	usbc_data->irq_vdm4 = usbc_data->irq_base + MAX77705_IRQ_VDM_DP_STATUS_UPDATE_INT;
	if (usbc_data->irq_vdm4) {
		ret = request_threaded_irq(usbc_data->irq_vdm4,
			   NULL, max77705_vdm_dp_status_irq,
			   0,
			   "usbc-vdm4-irq", usbc_data);
		if (ret) {
			pr_err("%s: Failed to Request IRQ (%d)\n", __func__, ret);
			return ret;
		}
	}

	usbc_data->irq_vdm5 = usbc_data->irq_base + MAX77705_IRQ_VDM_DP_CONFIGURE_INT;
	if (usbc_data->irq_vdm5) {
		ret = request_threaded_irq(usbc_data->irq_vdm5,
			   NULL, max77705_vdm_dp_configure_irq,
			   0,
			   "usbc-vdm5-irq", usbc_data);
		if (ret) {
			pr_err("%s: Failed to Request IRQ (%d)\n", __func__, ret);
			return ret;
		}
	}

	usbc_data->irq_vdm6 = usbc_data->irq_base + MAX77705_IRQ_VDM_ATTENTION_INT;
	if (usbc_data->irq_vdm6) {
		ret = request_threaded_irq(usbc_data->irq_vdm6,
			   NULL, max77705_vdm_attention_irq,
			   0,
			   "usbc-vdm6-irq", usbc_data);
		if (ret) {
			pr_err("%s: Failed to Request IRQ (%d)\n", __func__, ret);
			return ret;
		}
	}

	usbc_data->irq_vir0 = usbc_data->irq_base + MAX77705_VIR_IRQ_ALTERROR_INT;
	if (usbc_data->irq_vir0) {
		ret = request_threaded_irq(usbc_data->irq_vir0,
			   NULL, max77705_vir_altmode_irq,
			   0,
			   "usbc-vir0-irq", usbc_data);
		if (ret) {
			pr_err("%s: Failed to Request IRQ (%d)\n", __func__, ret);
			return ret;
		}
	}

	return ret;
}

static void max77705_usbc_umask_irq(struct max77705_usbc_platform_data *usbc_data)
{
	int ret = 0;
	u8 i2c_data = 0;
	/* Unmask max77705 interrupt */
	ret = max77705_read_reg(usbc_data->i2c, 0x23,
			  &i2c_data);
	if (ret) {
		pr_err("%s fail to read muic reg\n", __func__);
		return;
	}

	i2c_data &= ~((1 << 3));	/* Unmask muic interrupt */
	max77705_write_reg(usbc_data->i2c, 0x23,
			   i2c_data);
}
static void max77705_usbc_mask_irq(struct max77705_usbc_platform_data *usbc_data)
{
	int ret = 0;
	u8 i2c_data = 0;
	/* Unmask max77705 interrupt */
	ret = max77705_read_reg(usbc_data->i2c, 0x23,
			  &i2c_data);
	if (ret) {
		pr_err("%s fail to read muic reg\n", __func__);
		return;
	}

	i2c_data |= ((1 << 3));	/* Unmask muic interrupt */
	max77705_write_reg(usbc_data->i2c, 0x23,
			   i2c_data);
}

static int pdic_handle_usb_external_notifier_notification(struct notifier_block *nb,
				unsigned long action, void *data)
{
	struct max77705_usbc_platform_data *usbpd_data = g_usbc_data;
	int ret = 0;
	int enable = *(int *)data;

	pr_info("%s : action=%lu , enable=%d\n", __func__, action, enable);
	switch (action) {
	case EXTERNAL_NOTIFY_HOSTBLOCK_PRE:
		if (enable) {
			max77705_set_enable_alternate_mode(ALTERNATE_MODE_STOP);
			if (usbpd_data->dp_is_connect)
				max77705_dp_detach(usbpd_data);
		} else {
			if (usbpd_data->dp_is_connect)
				max77705_dp_detach(usbpd_data);
		}
		break;
	case EXTERNAL_NOTIFY_HOSTBLOCK_POST:
		if (enable) {
		} else {
			max77705_set_enable_alternate_mode(ALTERNATE_MODE_START);
		}
		break;
	case EXTERNAL_NOTIFY_DEVICEADD:
		if (enable) {
			usbpd_data->device_add = 1;
			wake_up_interruptible(&usbpd_data->device_add_wait_q);
		}
		break;
	case EXTERNAL_NOTIFY_MDMBLOCK_PRE:
		if (enable) {
			usbpd_data->mdm_block = 1;
			if (usbpd_data->dp_is_connect)
				max77705_dp_detach(usbpd_data);
		}
		break;
	default:
		break;
	}

	return ret;
}

static void delayed_external_notifier_init(struct work_struct *work)
{
	int ret = 0;
	static int retry_count = 1;
	int max_retry_count = 5;
	struct max77705_usbc_platform_data *usbpd_data = g_usbc_data;

	pr_info("%s : %d = times!\n", __func__, retry_count);

	/* Register ccic handler to ccic notifier block list */
	ret = usb_external_notify_register(&usbpd_data->usb_external_notifier_nb,
		pdic_handle_usb_external_notifier_notification, EXTERNAL_NOTIFY_DEV_PDIC);
	if (ret < 0) {
		pr_err("Manager notifier init time is %d.\n", retry_count);
		if (retry_count++ != max_retry_count)
			schedule_delayed_work(&usbpd_data->usb_external_notifier_register_work, msecs_to_jiffies(2000));
		else
			pr_err("fail to init external notifier\n");
	} else
		pr_info("%s : external notifier register done!\n", __func__);
}

#if defined(CONFIG_SEC_FACTORY)
static void factory_check_abnormal_state(struct work_struct *work)
{
	struct max77705_usbc_platform_data *usbpd_data = g_usbc_data;
	int state_cnt = usbpd_data->factory_mode.FAC_Abnormal_Repeat_State;

	msg_maxim("IN ");

	if (state_cnt >= FAC_ABNORMAL_REPEAT_STATE) {
		msg_maxim("Notify the abnormal state [STATE] [ %d]", state_cnt);
		max77705_ccic_event_work(usbpd_data,
			PDIC_NOTIFY_DEV_CCIC, PDIC_NOTIFY_ID_FAC, 1, 0, 0);
	} else
		msg_maxim("[STATE] cnt :  [%d]", state_cnt);
	usbpd_data->factory_mode.FAC_Abnormal_Repeat_State = 0;
	msg_maxim("OUT ");
}

static void factory_check_normal_rid(struct work_struct *work)
{
	struct max77705_usbc_platform_data *usbpd_data = g_usbc_data;
	int rid_cnt = usbpd_data->factory_mode.FAC_Abnormal_Repeat_RID;

	msg_maxim("IN ");

	if (rid_cnt >= FAC_ABNORMAL_REPEAT_RID) {
		msg_maxim("Notify the abnormal state [RID] [ %d]", rid_cnt);
		max77705_ccic_event_work(usbpd_data,
			PDIC_NOTIFY_DEV_CCIC, PDIC_NOTIFY_ID_FAC, 1 << 1, 0, 0);
	} else
		msg_maxim("[RID] cnt :  [%d]", rid_cnt);

	usbpd_data->factory_mode.FAC_Abnormal_Repeat_RID = 0;
	msg_maxim("OUT ");
}

void factory_execute_monitor(int type)
{
	struct max77705_usbc_platform_data *usbpd_data = g_usbc_data;
	uint32_t state_cnt = usbpd_data->factory_mode.FAC_Abnormal_Repeat_State;
	uint32_t rid_cnt = usbpd_data->factory_mode.FAC_Abnormal_Repeat_RID;
	uint32_t rid0_cnt = usbpd_data->factory_mode.FAC_Abnormal_RID0;

	switch (type) {
	case FAC_ABNORMAL_REPEAT_RID0:
		if (!rid0_cnt) {
			msg_maxim("Notify the abnormal state [RID0] [%d]!!", rid0_cnt);
			usbpd_data->factory_mode.FAC_Abnormal_RID0++;
			max77705_ccic_event_work(usbpd_data,
				PDIC_NOTIFY_DEV_CCIC, PDIC_NOTIFY_ID_FAC, 1 << 2, 0, 0);
		} else {
			usbpd_data->factory_mode.FAC_Abnormal_RID0 = 0;
		}
	break;
	case FAC_ABNORMAL_REPEAT_RID:
		if (!rid_cnt) {
			schedule_delayed_work(&usbpd_data->factory_rid_work, msecs_to_jiffies(1000));
			msg_maxim("start the factory_rid_work");
		}
		usbpd_data->factory_mode.FAC_Abnormal_Repeat_RID++;
	break;
	case FAC_ABNORMAL_REPEAT_STATE:
		if (!state_cnt) {
			schedule_delayed_work(&usbpd_data->factory_state_work, msecs_to_jiffies(1000));
			msg_maxim("start the factory_state_work");
		}
		usbpd_data->factory_mode.FAC_Abnormal_Repeat_State++;
	break;
	default:
		msg_maxim("Never Calling [%d]", type);
	break;
	}
}
#endif

#if IS_ENABLED(CONFIG_IF_CB_MANAGER)
static void max77705_usbpd_set_host_on(void *data, int mode)
{
	struct max77705_usbc_platform_data *usbpd_data = data;

	if (!usbpd_data)
		return;

	pr_info("%s : current_set is %d!\n", __func__, mode);
	if (mode) {
		usbpd_data->device_add = 0;
		usbpd_data->detach_done_wait = 0;
		usbpd_data->host_turn_on_event = 1;
		wake_up_interruptible(&usbpd_data->host_turn_on_wait_q);
	} else {
		usbpd_data->device_add = 0;
		usbpd_data->detach_done_wait = 0;
		usbpd_data->host_turn_on_event = 0;
	}
}

struct usbpd_ops ops_usbpd = {
	.usbpd_set_host_on = max77705_usbpd_set_host_on,
};
#endif

static int max77705_usbc_probe(struct platform_device *pdev)
{
	struct max77705_dev *max77705 = dev_get_drvdata(pdev->dev.parent);
	struct max77705_platform_data *pdata = dev_get_platdata(max77705->dev);
	struct max77705_usbc_platform_data *usbc_data = NULL;
	int ret;
#if IS_ENABLED(CONFIG_PDIC_NOTIFIER)
	ppdic_data_t ppdic_data;
	ppdic_sysfs_property_t ppdic_sysfs_prop;
#endif
	struct otg_notify *o_notify = get_otg_notify();
#if IS_ENABLED(CONFIG_IF_CB_MANAGER)
	struct usbpd_dev *usbpd_d;
#endif
	struct max77705_hmd_power_dev *hmd_list;

	msg_maxim("Probing : %d", max77705->irq);
	usbc_data =  kzalloc(sizeof(struct max77705_usbc_platform_data), GFP_KERNEL);
	if (!usbc_data)
		return -ENOMEM;

#if defined(CONFIG_QCOM_IFPMIC_SUSPEND)
	max77705->check_usbc_opcode_queue = check_usbc_opcode_queue;
#endif
	g_usbc_data = usbc_data;
	usbc_data->dev = &pdev->dev;
	usbc_data->max77705 = max77705;
	usbc_data->muic = max77705->muic;
	usbc_data->charger = max77705->charger;
	usbc_data->i2c = max77705->i2c;
	usbc_data->max77705_data = pdata;
	usbc_data->irq_base = pdata->irq_base;

	usbc_data->pd_data = kzalloc(sizeof(struct max77705_pd_data), GFP_KERNEL);
	if (!usbc_data->pd_data)
		return -ENOMEM;

	usbc_data->cc_data = kzalloc(sizeof(struct max77705_cc_data), GFP_KERNEL);
	if (!usbc_data->cc_data)
		return -ENOMEM;

	platform_set_drvdata(pdev, usbc_data);

#if defined(CONFIG_CCIC_MAX77705_DEBUG)
	mxim_debug_init();
	mxim_debug_set_i2c_client(usbc_data->muic);
#endif

	ret = sysfs_create_group(&max77705->dev->kobj, &max77705_attr_grp);
	msg_maxim("created sysfs. ret=%d", ret);

	usbc_data->HW_Revision = 0x0;
	usbc_data->FW_Revision = 0x0;
	usbc_data->plug_attach_done = 0x0;
	usbc_data->cc_data->current_pr = 0xFF;
	usbc_data->pd_data->current_dr = 0xFF;
	usbc_data->cc_data->current_vcon = 0xFF;
	usbc_data->op_code_done = 0x0;
	usbc_data->current_connstat = 0xFF;
	usbc_data->prev_connstat = 0xFF;
	usbc_data->usbc_cmd_queue.front = NULL;
	usbc_data->usbc_cmd_queue.rear = NULL;
	usbc_data->opcode_stamp = 0;
	mutex_init(&usbc_data->op_lock);
#if IS_ENABLED(CONFIG_PDIC_NOTIFIER)
	ppdic_data = devm_kzalloc(usbc_data->dev, sizeof(pdic_data_t), GFP_KERNEL);
	if (!ppdic_data)
		return -ENOMEM;

	ppdic_sysfs_prop = devm_kzalloc(usbc_data->dev, sizeof(pdic_sysfs_property_t), GFP_KERNEL);
	if (!ppdic_sysfs_prop)
		return -ENOMEM;

	ppdic_sysfs_prop->get_property = max77705_sysfs_get_local_prop;
	ppdic_sysfs_prop->set_property = max77705_sysfs_set_prop;
	ppdic_sysfs_prop->property_is_writeable = max77705_sysfs_is_writeable;
	ppdic_sysfs_prop->property_is_writeonly = max77705_sysfs_is_writeonly;
	ppdic_sysfs_prop->properties = max77705_sysfs_properties;
	ppdic_sysfs_prop->num_properties = ARRAY_SIZE(max77705_sysfs_properties);
	ppdic_data->pdic_sysfs_prop = ppdic_sysfs_prop;
	ppdic_data->drv_data = usbc_data;
	ppdic_data->name = "max77705";
	ppdic_data->set_enable_alternate_mode = max77705_set_enable_alternate_mode;
	pdic_core_register_chip(ppdic_data);
	usbc_data->vconn_en = 1;
	usbc_data->cur_rid = RID_OPEN;
	usbc_data->cc_pin_status = NO_DETERMINATION;
#if IS_ENABLED(CONFIG_IF_CB_MANAGER)
	usbpd_d = kzalloc(sizeof(struct usbpd_dev), GFP_KERNEL);
	if (!usbpd_d) {
		pr_err("%s: failed to allocate usbpd data\n", __func__);
		ret = -ENOMEM;
		return ret;
	}

	usbpd_d->ops = &ops_usbpd;
	usbpd_d->data = (void *)usbc_data;
	usbc_data->man = register_usbpd(usbpd_d);
#endif
	usbc_data->typec_cap.revision = USB_TYPEC_REV_1_2;
	usbc_data->typec_cap.pd_revision = 0x300;
	usbc_data->typec_cap.prefer_role = TYPEC_NO_PREFERRED_ROLE;

#if !defined(CONFIG_SUPPORT_USB_TYPEC_OPS)
	usbc_data->typec_cap.pr_set = max77705_pr_set;
	usbc_data->typec_cap.dr_set = max77705_dr_set;
	usbc_data->typec_cap.port_type_set = max77705_port_type_set;
#else
	usbc_data->typec_cap.driver_data = usbc_data;
	usbc_data->typec_cap.ops = &max77705_ops;
#endif

	usbc_data->typec_cap.type = TYPEC_PORT_DRP;
	usbc_data->typec_cap.data = TYPEC_PORT_DRD;

	usbc_data->typec_power_role = TYPEC_SINK;
	usbc_data->typec_try_state_change = TRY_ROLE_SWAP_NONE;

	usbc_data->port = typec_register_port(usbc_data->dev, &usbc_data->typec_cap);
	if (IS_ERR(usbc_data->port))
		pr_err("unable to register typec_register_port\n");
	else
		msg_maxim("success typec_register_port port=%pK", usbc_data->port);
	init_completion(&usbc_data->typec_reverse_completion);

	usbc_data->auto_vbus_en = false;
	usbc_data->is_first_booting = 1;
	usbc_data->pd_support = false;
	usbc_data->ccrp_state = 0;
	usbc_data->set_altmode = 0;
	usbc_data->set_altmode_error = 0;
	usbc_data->need_recover = false;
	usbc_data->op_ctrl1_w = (BIT_CCSrcSnk | BIT_CCSnkSrc | BIT_CCDetEn);
	usbc_data->srcccap_request_retry = false;
#if defined(CONFIG_SUPPORT_SHIP_MODE)
	usbc_data->ship_mode_en = 0;
#endif

	send_otg_notify(o_notify, NOTIFY_EVENT_POWER_SOURCE, 0);
#endif
	init_completion(&usbc_data->op_completion);
	init_completion(&usbc_data->ccic_sysfs_completion);
	init_completion(&usbc_data->psrdy_wait);
	usbc_data->op_wait_queue = create_singlethread_workqueue("op_wait");
	if (usbc_data->op_wait_queue == NULL)
		return -ENOMEM;
	usbc_data->op_send_queue = create_singlethread_workqueue("op_send");
	if (usbc_data->op_send_queue == NULL)
		return -ENOMEM;

	INIT_WORK(&usbc_data->op_send_work, max77705_uic_op_send_work_func);
	INIT_WORK(&usbc_data->cc_open_req_work, max77705_cc_open_work_func);
#if defined(MAX77705_SYS_FW_UPDATE)
	INIT_WORK(&usbc_data->fw_update_work,
			max77705_firmware_update_sysfs_work);
#endif

#if IS_ENABLED(CONFIG_PDIC_NOTIFIER)
	/* Create a work queue for the ccic irq thread */
	usbc_data->ccic_wq
		= create_singlethread_workqueue("ccic_irq_event");
	if (!usbc_data->ccic_wq) {
		pr_err("%s failed to create work queue\n", __func__);
		return -ENOMEM;
	}
#endif
#if defined(CONFIG_SEC_FACTORY)
	INIT_DELAYED_WORK(&usbc_data->factory_state_work,
				factory_check_abnormal_state);
	INIT_DELAYED_WORK(&usbc_data->factory_rid_work,
				factory_check_normal_rid);
#endif
	max77705_get_version_info(usbc_data);
	max77705_init_irq_handler(usbc_data);
	max77705_muic_probe(usbc_data);
	max77705_cc_init(usbc_data);
	max77705_pd_init(usbc_data);
	max77705_write_reg(usbc_data->muic, REG_PD_INT_M, 0x1C);
	max77705_write_reg(usbc_data->muic, REG_VDM_INT_M, 0xFF);
	max77705_init_opcode(usbc_data, 0);
	INIT_DELAYED_WORK(&usbc_data->vbus_hard_reset_work,
				vbus_control_hard_reset);
	/* turn on the VBUS automatically. */
	// max77705_usbc_enable_auto_vbus(usbc_data);
	INIT_DELAYED_WORK(&usbc_data->acc_detach_work, max77705_acc_detach_check);

	pdic_register_switch_device(1);
	INIT_DELAYED_WORK(&usbc_data->usb_external_notifier_register_work,
				  delayed_external_notifier_init);

	init_completion(&usbc_data->uvdm_longpacket_out_wait);
	init_completion(&usbc_data->uvdm_longpacket_in_wait);
#if IS_ENABLED(CONFIG_PDIC_NOTIFIER)
	ppdic_data->fw_data.data = (void *)usbc_data;
	ppdic_data->fw_data.firmware_update = max77705_firmware_update_callback;
	ppdic_data->fw_data.get_prev_fw_size = max77705_get_firmware_size;
	ret = pdic_misc_init(ppdic_data);
	if (ret) {
		pr_err("pdic_misc_init is failed, error %d\n", ret);
	} else {
		ppdic_data->misc_dev->uvdm_read = max77705_sec_uvdm_in_request_message;
		ppdic_data->misc_dev->uvdm_write = max77705_sec_uvdm_out_request_message;
		ppdic_data->misc_dev->uvdm_ready = max77705_sec_uvdm_ready;
		ppdic_data->misc_dev->uvdm_close = max77705_sec_uvdm_close;
		ppdic_data->misc_dev->pps_control = max77705_sec_pps_control;
		usbc_data->ppdic_data = ppdic_data;
	}
#endif

	hmd_list = kzalloc(MAX_NUM_HMD * sizeof(*hmd_list),
				GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(hmd_list)) {
		kfree(hmd_list);
		usbc_data->hmd_list = NULL;
		return -ENOMEM;
	}

	/* add default AR/VR here */
	snprintf(hmd_list[0].hmd_name, NAME_LEN_HMD, "PicoVR");
	hmd_list[0].vid  = 0x2d40;
	hmd_list[0].pid = 0x0000;

	usbc_data->hmd_list = hmd_list;

	mutex_init(&usbc_data->hmd_power_lock);

	/* Register pdic handler to pdic notifier block list */
	ret = usb_external_notify_register(&usbc_data->usb_external_notifier_nb,
		pdic_handle_usb_external_notifier_notification, EXTERNAL_NOTIFY_DEV_PDIC);
	if (ret < 0)
		schedule_delayed_work(&usbc_data->usb_external_notifier_register_work, msecs_to_jiffies(2000));
	else
		pr_info("%s : external notifier register done!\n", __func__);

	max77705->cc_booting_complete = 1;
	max77705_usbc_umask_irq(usbc_data);
	init_waitqueue_head(&usbc_data->host_turn_on_wait_q);
	init_waitqueue_head(&usbc_data->device_add_wait_q);
#if IS_ENABLED(CONFIG_IF_CB_MANAGER)
	max77705_usbpd_set_host_on(usbc_data, 0);
#endif
	usbc_data->host_turn_on_wait_time = 3;

	usbc_data->cc_open_req = 1;
	pdic_manual_ccopen_request(0);

	msg_maxim("probing Complete..");
	return 0;
}

static int max77705_usbc_remove(struct platform_device *pdev)
{
	struct max77705_usbc_platform_data *usbc_data =
		platform_get_drvdata(pdev);
	struct max77705_dev *max77705 = usbc_data->max77705;

#if defined(CONFIG_CCIC_MAX77705_DEBUG)
	mxim_debug_exit();
#endif
	sysfs_remove_group(&max77705->dev->kobj, &max77705_attr_grp);
	kzfree(usbc_data->hmd_list);
	usbc_data->hmd_list = NULL;
	mutex_destroy(&usbc_data->hmd_power_lock);
	mutex_destroy(&usbc_data->op_lock);
	pdic_core_unregister_chip();

	typec_unregister_port(usbc_data->port);

	pdic_register_switch_device(0);
#if IS_ENABLED(CONFIG_PDIC_NOTIFIER)
	if (usbc_data->ppdic_data && usbc_data->ppdic_data->misc_dev)
		pdic_misc_exit();
#endif
	usb_external_notify_unregister(&usbc_data->usb_external_notifier_nb);
	max77705_muic_remove(usbc_data);

	free_irq(usbc_data->irq_apcmd, usbc_data);
	free_irq(usbc_data->irq_sysmsg, usbc_data);
	free_irq(usbc_data->irq_vdm0, usbc_data);
	free_irq(usbc_data->irq_vdm1, usbc_data);
	free_irq(usbc_data->irq_vdm2, usbc_data);
	free_irq(usbc_data->irq_vdm3, usbc_data);
	free_irq(usbc_data->irq_vdm4, usbc_data);
	free_irq(usbc_data->irq_vdm5, usbc_data);
	free_irq(usbc_data->irq_vdm6, usbc_data);
	free_irq(usbc_data->irq_vdm7, usbc_data);
	free_irq(usbc_data->pd_data->irq_pdmsg, usbc_data);
	free_irq(usbc_data->pd_data->irq_datarole, usbc_data);
	free_irq(usbc_data->pd_data->irq_ssacc, usbc_data);
	free_irq(usbc_data->pd_data->irq_fct_id, usbc_data);
	free_irq(usbc_data->cc_data->irq_vconncop, usbc_data);
	free_irq(usbc_data->cc_data->irq_vsafe0v, usbc_data);
	free_irq(usbc_data->cc_data->irq_detabrt, usbc_data);
	free_irq(usbc_data->cc_data->irq_vconnsc, usbc_data);
	free_irq(usbc_data->cc_data->irq_ccpinstat, usbc_data);
	free_irq(usbc_data->cc_data->irq_ccistat, usbc_data);
	free_irq(usbc_data->cc_data->irq_ccvcnstat, usbc_data);
	free_irq(usbc_data->cc_data->irq_ccstat, usbc_data);
#if IS_ENABLED(CONFIG_IF_CB_MANAGER)
	kfree(usbc_data->man->usbpd_d);
#endif
	kfree(usbc_data->cc_data);
	kfree(usbc_data->pd_data);
	kfree(usbc_data);
	return 0;
}

#if defined CONFIG_PM
static int max77705_usbc_suspend(struct device *dev)
{
	struct max77705_usbc_platform_data *usbc_data =
		dev_get_drvdata(dev);

	max77705_muic_suspend(usbc_data);

	return 0;
}

static int max77705_usbc_resume(struct device *dev)
{
	struct max77705_usbc_platform_data *usbc_data =
		dev_get_drvdata(dev);

	max77705_muic_resume(usbc_data);
	if (usbc_data->set_altmode_error) {
		msg_maxim("set alternate mode");
		max77705_set_enable_alternate_mode
			(usbc_data->set_altmode);
	}

	return 0;
}
#else
#define max77705_usbc_suspend NULL
#define max77705_usbc_resume NULL
#endif

static void max77705_usbc_disable_irq(struct max77705_usbc_platform_data *usbc_data)
{
	disable_irq(usbc_data->irq_apcmd);
	disable_irq(usbc_data->irq_sysmsg);
	disable_irq(usbc_data->irq_vdm0);
	disable_irq(usbc_data->irq_vdm1);
	disable_irq(usbc_data->irq_vdm2);
	disable_irq(usbc_data->irq_vdm3);
	disable_irq(usbc_data->irq_vdm4);
	disable_irq(usbc_data->irq_vdm5);
	disable_irq(usbc_data->irq_vdm6);
	disable_irq(usbc_data->irq_vir0);
	disable_irq(usbc_data->pd_data->irq_pdmsg);
	disable_irq(usbc_data->pd_data->irq_psrdy);
	disable_irq(usbc_data->pd_data->irq_datarole);
	disable_irq(usbc_data->pd_data->irq_ssacc);
	disable_irq(usbc_data->pd_data->irq_fct_id);
	disable_irq(usbc_data->cc_data->irq_vconncop);
	disable_irq(usbc_data->cc_data->irq_vsafe0v);
	disable_irq(usbc_data->cc_data->irq_vconnsc);
	disable_irq(usbc_data->cc_data->irq_ccpinstat);
	disable_irq(usbc_data->cc_data->irq_ccistat);
	disable_irq(usbc_data->cc_data->irq_ccvcnstat);
	disable_irq(usbc_data->cc_data->irq_ccstat);
}

static void max77705_usbc_shutdown(struct platform_device *pdev)
{
	struct max77705_usbc_platform_data *usbc_data =
		platform_get_drvdata(pdev);
	struct device_node *np;
	int gpio_dp_sw_oe;
	u8 uic_int = 0;
	u8 uid = 0;

	msg_maxim("max77705 usbc driver shutdown++++");
	if (!usbc_data->muic) {
		msg_maxim("no max77705 i2c client");
		return;
	}
	usbc_data->shut_down = 1;
	max77705_muic_shutdown(usbc_data);
	max77705_usbc_mask_irq(usbc_data);
	/* unmask */
	max77705_write_reg(usbc_data->muic, REG_PD_INT_M, 0xFF);
	max77705_write_reg(usbc_data->muic, REG_CC_INT_M, 0xFF);
	max77705_write_reg(usbc_data->muic, REG_UIC_INT_M, 0xFF);
	max77705_write_reg(usbc_data->muic, REG_VDM_INT_M, 0xFF);

	max77705_usbc_disable_irq(usbc_data);

	max77705_read_reg(usbc_data->muic, REG_USBC_STATUS1, &uid);
	uid = (uid & BIT_UIDADC) >> FFS(BIT_UIDADC);

	/* send the reset command */
	if (usbc_data->current_connstat == WATER)
		msg_maxim("no call the reset function(WATER STATE)");
	else if (usbc_data->cur_rid != RID_OPEN && usbc_data->cur_rid != RID_UNDEFINED)
		msg_maxim("no call the reset function(RID)");
	else if (uid != UID_Open)
		msg_maxim("no call the reset function(UID)");
	else {
#if defined(CONFIG_SUPPORT_SHIP_MODE)
		if (usbc_data->ship_mode_en)
			pr_info("%s: ship_mode_en(%d), skip reset_ic\n",
				__func__, usbc_data->ship_mode_en);
		else
			max77705_reset_ic(usbc_data);
#else
		max77705_reset_ic(usbc_data);
#endif
		if (usbc_data->dp_is_connect) {
			pr_info("aux_sw_oe pin set to high\n");
			np = of_find_node_by_name(NULL, "displayport");
			gpio_dp_sw_oe = of_get_named_gpio(np, "dp,aux_sw_oe", 0);
			gpio_direction_output(gpio_dp_sw_oe, 1);
		}
		max77705_write_reg(usbc_data->muic, REG_PD_INT_M, 0xFF);
		max77705_write_reg(usbc_data->muic, REG_CC_INT_M, 0xFF);
		max77705_write_reg(usbc_data->muic, REG_UIC_INT_M, 0xFF);
		max77705_write_reg(usbc_data->muic, REG_VDM_INT_M, 0xFF);
		max77705_read_reg(usbc_data->muic,
				MAX77705_USBC_REG_UIC_INT, &uic_int);
	}
	msg_maxim("max77705 usbc driver shutdown----");
}

static SIMPLE_DEV_PM_OPS(max77705_usbc_pm_ops, max77705_usbc_suspend,
			 max77705_usbc_resume);

static struct platform_driver max77705_usbc_driver = {
	.driver = {
		.name = "max77705-usbc",
		.owner = THIS_MODULE,
#ifdef CONFIG_PM
		.pm = &max77705_usbc_pm_ops,
#endif
	},
	.shutdown = max77705_usbc_shutdown,
	.probe = max77705_usbc_probe,
	.remove = max77705_usbc_remove,
};

static int __init max77705_usbc_init(void)
{
	msg_maxim("init");
	return platform_driver_register(&max77705_usbc_driver);
}
device_initcall(max77705_usbc_init);

static void __exit max77705_usbc_exit(void)
{
	platform_driver_unregister(&max77705_usbc_driver);
}
module_exit(max77705_usbc_exit);

MODULE_DESCRIPTION("max77705 USBPD driver");
MODULE_LICENSE("GPL");
