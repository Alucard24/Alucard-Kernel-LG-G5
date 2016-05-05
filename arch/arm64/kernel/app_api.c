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

#include <linux/bitops.h>
#include <linux/spinlock.h>
#include <linux/export.h>

static spinlock_t spinlock;

void set_app_setting_bit(uint32_t bit)
{
	uint64_t reg;
	unsigned long flags;

	spin_lock_irqsave(&spinlock, flags);
	asm volatile("mrs %0, S3_1_C15_C15_0" : "=r" (reg));
	reg = reg | BIT(bit);
	isb();
	asm volatile("msr S3_1_C15_C15_0, %0" : : "r" (reg));
	isb();
	spin_unlock_irqrestore(&spinlock, flags);
}
EXPORT_SYMBOL(set_app_setting_bit);

void clear_app_setting_bit(uint32_t bit)
{
	uint64_t reg;
	unsigned long flags;

	spin_lock_irqsave(&spinlock, flags);
	asm volatile("mrs %0, S3_1_C15_C15_0" : "=r" (reg));
	reg = reg & ~BIT(bit);
	isb();
	asm volatile("msr S3_1_C15_C15_0, %0" : : "r" (reg));
	isb();
	spin_unlock_irqrestore(&spinlock, flags);
}
EXPORT_SYMBOL(clear_app_setting_bit);

static int __init init_app_api(void)
{
	spin_lock_init(&spinlock);
	return 0;
}
early_initcall(init_app_api);
