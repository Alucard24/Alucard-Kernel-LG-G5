/*
 * Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
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

/*
	Based on cpuboost and created by Alucard24@XDA.
*/

#define pr_fmt(fmt) "alu-t-boost: " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/time.h>

/*
 * debug = 1 will print all
 */
static unsigned int debug = 0;
module_param_named(debug_mask, debug, uint, 0644);

#define dprintk(msg...)		\
do {				\
	if (debug)		\
		pr_info(msg);	\
} while (0)

static struct workqueue_struct *touch_boost_wq;
static struct delayed_work input_boost_rem;
static struct work_struct input_boost_work;

static bool input_boost_enabled;

static unsigned int input_boost_ms = 40;
module_param(input_boost_ms, uint, 0644);

static u64 last_input_time;

static unsigned int min_input_interval = 150;
module_param(min_input_interval, uint, 0644);

static struct min_cpu_limit {
	uint32_t input_boost_freq[4];
	uint32_t user_boost_freq_lock[4];
} limit = {
	.input_boost_freq[0] = 0,
	.input_boost_freq[1] = 0,
	.input_boost_freq[2] = 0,
	.input_boost_freq[3] = 0,
	.user_boost_freq_lock[0] = 0,
	.user_boost_freq_lock[1] = 0,
	.user_boost_freq_lock[2] = 0,
	.user_boost_freq_lock[3] = 0,
};

static int set_input_boost_freq(const char *buf, const struct kernel_param *kp)
{
	int i, ntokens = 0;
	unsigned int val, cpu;
	const char *cp = buf;
	bool enabled = false;

	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	/* single number: apply to all CPUs */
	if (!ntokens) {
		if (sscanf(buf, "%u\n", &val) != 1)
			return -EINVAL;
		for_each_possible_cpu(i)
			limit.input_boost_freq[i] = val;
		goto check_enable;
	}

	/* CPU:value pair */
	if (!(ntokens % 2))
		return -EINVAL;

	cp = buf;
	for (i = 0; i < ntokens; i += 2) {
		if (sscanf(cp, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu > num_possible_cpus())
			return -EINVAL;

		limit.input_boost_freq[cpu] = val;
		cp = strchr(cp, ' ');
		cp++;
	}

check_enable:
	for_each_possible_cpu(i) {
		if (limit.input_boost_freq[i]) {
			enabled = true;
			break;
		}
	}
	input_boost_enabled = enabled;

	return 0;
}

static int get_input_boost_freq(char *buf, const struct kernel_param *kp)
{
	int cnt = 0, cpu;

	for_each_possible_cpu(cpu) {
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt,
				"%d:%u ", cpu, limit.input_boost_freq[cpu]);
	}
	cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
	return cnt;
}

static const struct kernel_param_ops param_ops_input_boost_freq = {
	.set = set_input_boost_freq,
	.get = get_input_boost_freq,
};
module_param_cb(input_boost_freq, &param_ops_input_boost_freq, NULL, 0644);

static void do_input_boost_rem(struct work_struct *work)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		if (limit.user_boost_freq_lock[cpu] > 0) {
			dprintk("Removing input boost for CPU%u\n", cpu);
			set_cpu_min_lock(cpu, 0);
			limit.user_boost_freq_lock[cpu] = 0;
		}
	}
}

static void do_input_boost(struct work_struct *work)
{
	unsigned int cpu;

	cancel_delayed_work_sync(&input_boost_rem);

	for_each_possible_cpu(cpu) {
		/* Save user boost lock */
		limit.user_boost_freq_lock[cpu] = limit.input_boost_freq[cpu];

		if (limit.user_boost_freq_lock[cpu] > 0) {
			dprintk("Input boost for CPU%u\n", cpu);
			set_cpu_min_lock(cpu, limit.user_boost_freq_lock[cpu]);
		}
	}

	queue_delayed_work(touch_boost_wq,
			&input_boost_rem,
			msecs_to_jiffies(input_boost_ms));
}

static void touchboost_input_event(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	u64 now;

	if (!input_boost_enabled)
		return;

	now = ktime_to_us(ktime_get());
	if ((now - last_input_time) < (min_input_interval * USEC_PER_MSEC))
		return;

	if (work_pending(&input_boost_work))
		return;

	dprintk("Input boost for input event.\n");

	queue_work(touch_boost_wq, &input_boost_work);
	last_input_time = ktime_to_us(ktime_get());
}

static int touchboost_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = handler->name;

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void touchboost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id touchboost_ids[] = {
	/* multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) },
	},
	/* touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ },
};

static struct input_handler touchboost_input_handler = {
	.event          = touchboost_input_event,
	.connect        = touchboost_input_connect,
	.disconnect     = touchboost_input_disconnect,
	.name           = "alu-t-boost",
	.id_table       = touchboost_ids,
};


static int touch_boost_init(void)
{
	int ret;

	touch_boost_wq = alloc_workqueue("touch_boost_wq", WQ_HIGHPRI, 0);
	if (!touch_boost_wq)
		return -EFAULT;

	INIT_WORK(&input_boost_work, do_input_boost);
	INIT_DELAYED_WORK(&input_boost_rem, do_input_boost_rem);

	ret = input_register_handler(&touchboost_input_handler);
	if (ret)
		pr_err("Cannot register touchboost input handler.\n");

	return ret;
}
late_initcall(touch_boost_init);
