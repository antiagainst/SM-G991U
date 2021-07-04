/*
 * Copyright (C) 2018 Samsung Electronics. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include "fingerprint.h"
#include "qbt2000_common.h"

int qbt2000_set_clk(struct qbt2000_drvdata *drvdata, bool onoff)
{
	int rc = 0;

	if (drvdata->clk_setting->enabled_clk == onoff) {
		pr_err("already %s\n", onoff ? "enabled" : "disabled");
		return rc;
	}

	if (onoff) {
		__pm_stay_awake(drvdata->clk_setting->spi_wake_lock);
		drvdata->clk_setting->enabled_clk = true;
	} else {
		__pm_relax(drvdata->clk_setting->spi_wake_lock);
		drvdata->clk_setting->enabled_clk = false;
	}
	return rc;
}

int qbt2000_register_platform_variable(struct qbt2000_drvdata *drvdata)
{
	return 0;
}

int qbt2000_unregister_platform_variable(struct qbt2000_drvdata *drvdata)
{
	return 0;
}

int fps_resume_set(void) {
	return 0;
}

int qbt2000_pinctrl_register(struct qbt2000_drvdata *drvdata)
{
	drvdata->p = NULL;
	drvdata->pins_poweroff = NULL;
	drvdata->pins_poweron = NULL;

	return 0;
}
