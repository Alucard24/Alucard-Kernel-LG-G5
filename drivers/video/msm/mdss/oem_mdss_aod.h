/* Copyright (c) 2012-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef OEM_MDSS_AOD_H
#define OEM_MDSS_AOD_H

#include <linux/of_device.h>
#include <linux/module.h>
#include "mdss_dsi.h"
#include "mdss_panel.h"
#include "mdss_fb.h"

/* Enum for parse aod command set */
enum aod_cmd_type {
	AOD_PANEL_NOT_CHNAGE = -1,
	AOD_PANEL_CMD_U3_TO_U2 = 0,
	AOD_PANEL_CMD_U2_TO_U3,
	/* Add cmd mode here if need */
	AOD_PANEL_CMD_NUM,
	AOD_PANEL_CMD_NONE
};

/* Enum for current and next panel mode */
enum aod_panel_mode {
	AOD_PANEL_MODE_U0_BLANK = 0,
	AOD_PANEL_MODE_U2_UNBLANK,
	AOD_PANEL_MODE_U2_BLANK,
	AOD_PANEL_MODE_U3_UNBLANK,
	AOD_PANEL_MODE_MAX
};

/* Enum for deside command to send */
enum aod_cmd_status {
	ON_CMD = 0,
	ON_AND_AOD,
	AOD_CMD_ENABLE,
	AOD_CMD_DISABLE,
	OFF_CMD,
	CMD_SKIP
};

enum aod_return_type {
	AOD_RETURN_SUCCESS = 0,
	AOD_RETURN_ERROR_NOT_INIT,
	AOD_RETURN_ERROR_NO_SCENARIO,
	AOD_RETURN_ERROR_NOT_NORMAL_BOOT,
	AOD_RETURN_ERROR_UNKNOWN,
	AOD_RETURN_ERROR_SEND_CMD,
	AOD_RETURN_ERROR_MEMORY = 12,
};

enum aod_backlight_type {
	AOD_BACKLIGHT_PRIMARY = 0,
	AOD_BACKLIGHT_SECONDARY,
	AOD_BACKLIGHT_BOTH,
	AOD_BACKLIGHT_NONE,
};

enum aod_keep_u2_type {
	AOD_MOVE_TO_U3 = 0,
	AOD_KEEP_U2,
	AOD_NO_DECISION,
};
int oem_mdss_aod_decide_status(struct msm_fb_data_type *mfd, int blank_mode);
int oem_mdss_aod_init(struct device_node *node, struct mdss_dsi_ctrl_pdata *ctrl_pdata);
int oem_mdss_aod_cmd_send(struct mdss_dsi_ctrl_pdata *ctrl_pdata, int cmd);
void oem_mdss_aod_set_backlight_mode(int mode);
#endif
