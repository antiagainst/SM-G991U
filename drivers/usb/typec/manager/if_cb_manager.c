/*
 * Copyright (C) 2018-2019 Samsung Electronics
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
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <linux/usb/typec/manager/if_cb_manager.h>
#include <linux/slab.h>
#include <linux/module.h>

static struct if_cb_manager *man_core;

struct if_cb_manager *create_alloc_if_cb_manager(void)
{
	man_core = kzalloc(sizeof(struct if_cb_manager), GFP_KERNEL);

	return man_core;
}

struct if_cb_manager *get_if_cb_manager(void)
{
	if (!man_core)
		create_alloc_if_cb_manager();

	return man_core;
}

struct if_cb_manager *register_usb(struct usb_dev *usb)
{
	struct if_cb_manager *man_core;

	man_core = get_if_cb_manager();
	man_core->usb_d = usb;

	return man_core;
}
EXPORT_SYMBOL(register_usb);

struct if_cb_manager *register_muic(struct muic_dev *muic)
{
	struct if_cb_manager *man_core;

	man_core = get_if_cb_manager();
	man_core->muic_d = muic;

	return man_core;
}
EXPORT_SYMBOL(register_muic);

struct if_cb_manager *register_usbpd(struct usbpd_dev *usbpd)
{
	struct if_cb_manager *man_core;

	man_core = get_if_cb_manager();
	man_core->usbpd_d = usbpd;

	return man_core;
}
EXPORT_SYMBOL(register_usbpd);

void usb_set_vbus_current(struct if_cb_manager *man_core, int state)
{
	if (man_core == NULL || man_core->usb_d == NULL ||
			man_core->usb_d->ops == NULL ||
			man_core->usb_d->ops->usb_set_vbus_current == NULL) {
		pr_err("%s : Member of if_cb_manager is NULL\n", __func__);
		return;
	}

	man_core->usb_d->ops->usb_set_vbus_current(
			man_core->usb_d->data, state);
}
EXPORT_SYMBOL(usb_set_vbus_current);

int muic_check_usb_killer(struct if_cb_manager *man_core)
{
	if (man_core == NULL || man_core->muic_d == NULL ||
			man_core->muic_d->ops == NULL ||
			man_core->muic_d->ops->muic_check_usb_killer == NULL) {
			pr_err("%s : Member of if_cb_manager is NULL\n", __func__);
		return 0;
	}

	return man_core->muic_d->ops->muic_check_usb_killer(
			man_core->muic_d->data);
}
EXPORT_SYMBOL(muic_check_usb_killer);

int usbpd_sbu_test_read(struct if_cb_manager *man_core)
{
	if (man_core == NULL || man_core->usbpd_d == NULL ||
			man_core->usbpd_d->ops == NULL ||
			man_core->usbpd_d->ops->usbpd_sbu_test_read == NULL) {
			pr_err("%s : Member of if_cb_manager is NULL\n", __func__);
		return -ENXIO;
	}

	return man_core->usbpd_d->ops->usbpd_sbu_test_read(
			man_core->usbpd_d->data);
}
EXPORT_SYMBOL(usbpd_sbu_test_read);

void usbpd_set_host_on(struct if_cb_manager *man_core, int mode)
{
	if (man_core == NULL || man_core->usbpd_d == NULL ||
			man_core->usbpd_d->ops == NULL ||
			man_core->usbpd_d->ops->usbpd_set_host_on == NULL) {
			pr_err("%s : Member of if_cb_manager is NULL\n", __func__);
		return;
	}

	man_core->usbpd_d->ops->usbpd_set_host_on(
		man_core->usbpd_d->data, mode);
}
EXPORT_SYMBOL(usbpd_set_host_on);

static int __init if_cb_manager_init(void)
{
	if (!man_core)
		create_alloc_if_cb_manager();
	return 0;
}

static void __exit if_cb_manager_exit(void)
{
	kfree(man_core);
	man_core = NULL;
}

module_init(if_cb_manager_init);
module_exit(if_cb_manager_exit);

MODULE_AUTHOR("Samsung USB Team");
MODULE_DESCRIPTION("Interface Callback Manager");
MODULE_LICENSE("GPL");
