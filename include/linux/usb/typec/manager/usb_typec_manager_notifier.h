/*
 * header file supporting USB Type-C Manager notifier call chain information
 *
 * Copyright (C) 2020 Samsung Electronics
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#ifndef __USB_TYPEC_MANAGER_NOTIFIER_H__
#define __USB_TYPEC_MANAGER_NOTIFIER_H__

#include <linux/muic/common/muic.h>
#include <linux/muic/common/muic_notifier.h>
#if IS_ENABLED(CONFIG_VBUS_NOTIFIER)
#include <linux/vbus_notifier.h>
#endif
#include <linux/usb/typec/common/pdic_notifier.h>

#define TYPEC_MANAGER_MAJ_VERSION 2
#define TYPEC_MANAGER_MIN_VERSION 0

/* USB TypeC Manager notifier call sequence,
 * largest priority number device will be called first. */
typedef enum {
/* MUIC */
	MANAGER_NOTIFY_MUIC_NONE = 0,
	MANAGER_NOTIFY_MUIC_USB,
	MANAGER_NOTIFY_MUIC_OTG,
	MANAGER_NOTIFY_MUIC_CHARGER,
	MANAGER_NOTIFY_MUIC_TIMEOUT_OPEN_DEVICE,
	MANAGER_NOTIFY_MUIC_UART,

/* PDIC */
	MANAGER_NOTIFY_PDIC_INITIAL = 20,
	MANAGER_NOTIFY_PDIC_WACOM, /* Low Priority */
	MANAGER_NOTIFY_PDIC_SENSORHUB,
	MANAGER_NOTIFY_PDIC_USBDP,
	MANAGER_NOTIFY_PDIC_DP,
	MANAGER_NOTIFY_PDIC_SUB_BATTERY,
	MANAGER_NOTIFY_PDIC_BATTERY,
	MANAGER_NOTIFY_PDIC_USB,
	MANAGER_NOTIFY_PDIC_MUIC, /* High Priority */
	MANAGER_NOTIFY_PDIC_DELAY_DONE,
} manager_notifier_device_t;

typedef enum {
	PD_USB_TYPE,
	PD_TA_TYPE,
} pd_usb_state_t;

typedef enum
{
	VBUS_NOTIFIER,
	PDIC_NOTIFIER,
	MUIC_NOTIFIER
}notifier_register;

typedef struct
{
	uint64_t src:4;
	uint64_t dest:4;
	uint64_t id:8;
	uint64_t sub1:16;
	uint64_t sub2:16;
	uint64_t sub3:16;
	void *pd;
} MANAGER_NOTI_TYPEDEF;

struct typec_manager_event_work
{
	struct work_struct typec_manager_work;
	MANAGER_NOTI_TYPEDEF event;
};

struct manager_dp {
	int is_support;
	int attach_state;
	int cable_type;
	int hpd_state;
	int is_connect;
	int hs_connect;
	int check_done;
};

struct manager_water {
	int detected;
	int detOnPowerOff;
	int wVbus_det;
	int report_type;
};

struct typec_manager_muic {
	int attach_state;
	int cable_type;
};

typedef struct _manager_data_t
{
	struct blocking_notifier_head manager_muic_notifier;
	struct blocking_notifier_head manager_notifier;
	struct notifier_block pdic_nb;
	struct notifier_block muic_nb;
#if IS_ENABLED(CONFIG_VBUS_NOTIFIER)
	struct notifier_block vbus_nb;
#endif
#if defined(CONFIG_CABLE_TYPE_NOTIFIER)
	struct	notifier_block cable_type_nb;
#endif

	struct delayed_work manager_init_work;
	struct workqueue_struct *manager_noti_wq;
	struct workqueue_struct *manager_muic_noti_wq;
	struct delayed_work usbenumchk_work;
	struct delayed_work chkforvbus_work;

	struct mutex mo_lock;
	int vbus_state;
	int classified_cable_type;

	int pdic_attach_state;
	int manager_dr_state;
	int pdic_rid_state;
	int alt_is_support;

	int usb_enum_state;
	bool usb_enable_state;

	unsigned long otg_stamp;
	int vbus_by_otg_detection;

	int pd_con_state;
	void *pd;

	char fac_control[16];

	struct typec_manager_muic muic;
#ifdef CONFIG_USE_SECOND_MUIC
	struct typec_manager_muic second_muic;
#endif
	struct manager_water water;
	struct manager_dp dp;
	struct notifier_block manager_external_notifier_nb; 
	struct typec_manager_gadget_ops *gadget_ops;
}manager_data_t;

struct typec_manager_gadget_ops {
	void		*driver_data;
	int		(*get_cmply_link_state)(void *dev);
};

#define PDIC_BATTERY	(1<<0)
#define PDIC_USB	(1<<1)
#define PDIC_DP		(1<<2)
#define PDIC_DELAY_DONE	(1<<3)

/* Timeout to check for USB enumeration */
#define CANCEL_USB_DWORK 0
#define MAX_USB_DWORK_TIME 120000
#define MIN_USB_DWORK_TIME 2000
#ifdef CONFIG_USB_CONFIGFS_F_MBIM
#define BOOT_USB_DWORK_TIME 9000
#else
#define BOOT_USB_DWORK_TIME 12000
#endif

/* Timeout for USB off when Vbus is in LOW state */
#define VBUS_USB_OFF_TIMEOUT 1000

/* Time to check whether it is VBUS by OTG to prevent moisture popup error */
#define OTG_VBUS_CHECK_TIME 300

/* Time to retry when Notifier registration fails */
#define NOTIFIER_REG_RETRY 2000

#define MANAGER_NOTIFIER_BLOCK(name)	\
	struct notifier_block (name)

#define SET_MANAGER_NOTIFIER_BLOCK(nb, fn, dev) do {	\
		(nb)->notifier_call = (fn);		\
		(nb)->priority = (dev);			\
	} while (0)

#define DESTROY_MANAGER_NOTIFIER_BLOCK(nb)			\
		SET_MANAGER_NOTIFIER_BLOCK(nb, NULL, -1)

/* pdic notifier register/unregister API
 * for used any where want to receive pdic attached device attach/detach. */
int manager_notifier_register(struct notifier_block *nb,
		notifier_fn_t notifier, manager_notifier_device_t listener);
int manager_notifier_unregister(struct notifier_block *nb);

void manager_notifier_usbdp_support(void);
void set_usb_enumeration_state(int state);
void set_usb_enable_state(void);
void probe_typec_manager_gadget_ops (struct typec_manager_gadget_ops *ops);
#endif /* __USB_TYPEC_MANAGER_NOTIFIER_H__ */
