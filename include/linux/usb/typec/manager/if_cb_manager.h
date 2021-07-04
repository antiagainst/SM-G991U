/*
 * Copyright (C) 2018-2019 Samsung Electronics
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 *
 */

#ifndef __IF_CB_MANAGER_H__
#define __IF_CB_MANAGER_H__

struct usb_ops {
	void (*usb_set_vbus_current)(void *data, int state);
};

struct muic_ops {
	int (*muic_check_usb_killer)(void *data);
	void (*muic_set_bypass)(void *data, int enable);
};

struct usbpd_ops {
	int (*usbpd_sbu_test_read)(void *data);
	void (*usbpd_set_host_on)(void *data, int mode);
};

struct usb_dev {
	const struct usb_ops *ops;
	void *data;
};

struct muic_dev {
	const struct muic_ops *ops;
	void *data;
};

struct usbpd_dev {
	const struct usbpd_ops *ops;
	void *data;
};

struct if_cb_manager {
	struct usb_dev *usb_d;
	struct muic_dev *muic_d;
	struct usbpd_dev *usbpd_d;
};

extern struct if_cb_manager *register_usb(struct usb_dev *usb);
extern struct if_cb_manager *register_muic(struct muic_dev *muic);
extern struct if_cb_manager *register_usbpd(struct usbpd_dev *usbpd);
extern void usb_set_vbus_current(struct if_cb_manager *man_core, int state);
extern int muic_check_usb_killer(struct if_cb_manager *man_core);
extern void muic_set_bypass(struct if_cb_manager *man_core, int enable);
extern int usbpd_sbu_test_read(struct if_cb_manager *man_core);
extern void usbpd_set_host_on(struct if_cb_manager *man_core, int mode);

#endif /* __IF_CB_MANAGER_H__ */
