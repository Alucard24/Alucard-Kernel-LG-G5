/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
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

#include <linux/kernel.h>
#include <asm/mach/map.h>
#include <asm/mach/arch.h>
#include "board-dt.h"

static const char *mdmfermium_dt_match[] __initconst = {
	"qcom,mdmfermium",
	NULL
};

static void __init mdmfermium_init(void)
{
	board_dt_populate(NULL);
}

DT_MACHINE_START(MDMFERMIUM_DT,
	"Qualcomm Technologies, Inc. MDM FERMIUM (Flattened Device Tree)")
	.init_machine	= mdmfermium_init,
	.dt_compat	= mdmfermium_dt_match,
MACHINE_END
