// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 The LineageOS Project
 */

#include <video/mipi_display.h>

#include "dsi_panel.h"
#include "dsi_panel_mi.h"

void dsi_panel_mi_parse(struct dsi_panel *panel)
{
	struct dsi_parser_utils *utils = &panel->utils;

	panel->mi.hbm_reg53 = utils->read_bool(utils->data,
			"mi,hbm-reg53-enabled");
}

int dsi_panel_mi_update_cmd_set(struct dsi_panel *panel,
		enum dsi_cmd_set_type type, u8 reg, const u8 *val, size_t len)
{
	struct dsi_panel_cmd_set *set;
	struct dsi_cmd_desc *cmd;
	u8 *payload;
	int i;

	if (!panel->cur_mode || !panel->cur_mode->priv_info)
		return -EINVAL;

	set = &panel->cur_mode->priv_info->cmd_sets[type];
	for (i = 0; i < set->count; i++) {
		cmd = &set->cmds[i];
		payload = (u8 *)cmd->msg.tx_buf;
		if (cmd->msg.tx_len != len + 1 || payload[0] != reg)
			continue;

		memcpy(&payload[1], val, len);
		return 0;
	}

	return -ENOENT;
}

static int dsi_panel_mi_tx_with_dbv(struct dsi_panel *panel,
		enum dsi_cmd_set_type type, u32 dbv)
{
	u8 buf[2] = { dbv >> 8, dbv & 0xff };
	int rc;

	rc = dsi_panel_mi_update_cmd_set(panel, type,
			MIPI_DCS_SET_DISPLAY_BRIGHTNESS, buf, sizeof(buf));
	if (rc)
		return rc;

	return dsi_panel_tx_cmd_set(panel, type);
}

/*
 * Returns true if the backlight level was already sent along with an HBM mode
 * switch, otherwise bl_lvl is updated to the DBV for the current mode.
 */
bool dsi_panel_mi_set_backlight(struct dsi_panel *panel, u32 *bl_lvl)
{
	struct dsi_panel_mi *mi = &panel->mi;
	u32 normal_max = panel->bl_config.bl_max_level / 2;
	bool hbm = *bl_lvl > normal_max;
	u32 dbv = hbm ? *bl_lvl - normal_max - 1 : *bl_lvl;

	mi->last_bl_lvl = *bl_lvl;

	if (!mi->hbm_reg53)
		return false;

	if (hbm != mi->hbm_active) {
		if (!dsi_panel_mi_tx_with_dbv(panel, hbm ? DSI_CMD_SET_MI_HBM_ON :
				DSI_CMD_SET_MI_HBM_OFF, dbv)) {
			mi->hbm_active = hbm;
			return true;
		}

		DSI_ERR("[%s] failed to switch hbm %s\n", panel->name,
				hbm ? "on" : "off");
	}

	if (mi->hbm_active)
		*bl_lvl = hbm ? dbv : 0;
	else
		*bl_lvl = min(*bl_lvl, normal_max);

	return false;
}

void dsi_panel_mi_leave_hbm(struct dsi_panel *panel)
{
	struct dsi_panel_mi *mi = &panel->mi;

	if (!mi->hbm_active)
		return;

	if (dsi_panel_mi_tx_with_dbv(panel, DSI_CMD_SET_MI_HBM_OFF,
			panel->bl_config.bl_max_level / 2))
		DSI_ERR("[%s] failed to leave hbm\n", panel->name);

	mi->hbm_active = false;
}

/* Called whenever the panel itself resets 0x53 */
void dsi_panel_mi_reset_state(struct dsi_panel *panel)
{
	panel->mi.hbm_active = false;
}
