/* Copyright (c) 2016, The Linux Foundation. All rights reserved.
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

#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/notifier.h>

#include <asm/app_api.h>

#define APP_SETTING_BIT 	29

static DEFINE_PER_CPU(int, app_setting_applied);

static uint32_t enable;
static int app_setting_set(const char *val, struct kernel_param *kp);
module_param_call(enable, app_setting_set, param_get_uint,
		  &enable, 0644);

void app_setting_enable(void *unused)
{
	set_app_setting_bit(APP_SETTING_BIT);
	this_cpu_write(app_setting_applied, 1);
}

void app_setting_disable(void *unused)
{
	clear_app_setting_bit(APP_SETTING_BIT);
	this_cpu_write(app_setting_applied, 0);
}

static int app_setting_set(const char *val, struct kernel_param *kp)
{
	int ret;

	ret = param_set_uint(val, kp);
	if (ret) {
		pr_err("app_setting: error setting param value %d\n", ret);
		return ret;
	}

	get_online_cpus();
	if (enable)
		on_each_cpu(app_setting_enable, NULL, 1);
	else
		on_each_cpu(app_setting_disable, NULL, 1);
	put_online_cpus();

	return 0;
}

static int app_setting_notifier_callback(struct notifier_block *nfb,
					unsigned long action, void *hcpu)
{
	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_STARTING:
		if (enable && !__this_cpu_read(app_setting_applied))
			app_setting_enable(NULL);
		else if (!enable && __this_cpu_read(app_setting_applied))
			app_setting_disable(NULL);
		break;
	}
	return 0;
}

static struct notifier_block app_setting_notifier = {
	.notifier_call = app_setting_notifier_callback,
};

static int __init app_setting_init(void)
{
	int ret;

	ret = register_hotcpu_notifier(&app_setting_notifier);
	if (ret)
		return ret;

	return 0;
}
module_init(app_setting_init);
