// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 The LineageOS Project
 */

#include <linux/slab.h>
#include <video/mipi_display.h>

#include "dsi_panel.h"
#include "dsi_panel_mi.h"

void dsi_panel_mi_parse(struct dsi_panel *panel)
{
	struct dsi_parser_utils *utils = &panel->utils;
	struct dsi_panel_mi *mi = &panel->mi;
	u32 hbm_dbv, lbm_dbv;

	mi->hbm_reg53 = utils->read_bool(utils->data, "mi,hbm-reg53-enabled");
	mi->doze_nolp = utils->read_bool(utils->data,
			"mi,aod-nolp-command-enabled");

	if (!utils->read_u32(utils->data, "mi,doze-hbm-dbv-level", &hbm_dbv) &&
	    !utils->read_u32(utils->data, "mi,doze-lbm-dbv-level", &lbm_dbv))
		mi->doze_hbm_threshold = (hbm_dbv + lbm_dbv) / 2;
	else
		mi->doze_hbm_threshold = 100;
}

void dsi_panel_mi_parse_cmd_update(struct dsi_panel_cmd_set *set,
		const char *name, struct dsi_parser_utils *utils)
{
	char prop[64];
	u32 *arr;
	int count, i;

	snprintf(prop, sizeof(prop), "%s-update", name);
	count = utils->count_u32_elems(utils->data, prop);
	if (count <= 0 || count % 3)
		return;

	arr = kcalloc(count, sizeof(*arr), GFP_KERNEL);
	if (!arr)
		return;

	if (utils->read_u32_array(utils->data, prop, arr, count))
		goto exit;

	set->updates = kcalloc(count / 3, sizeof(*set->updates), GFP_KERNEL);
	if (!set->updates)
		goto exit;

	for (i = 0; i < count / 3; i++) {
		set->updates[i].reg = arr[i * 3];
		set->updates[i].index = arr[i * 3 + 1];
		set->updates[i].len = arr[i * 3 + 2];
	}
	set->update_count = count / 3;
exit:
	kfree(arr);
}

int dsi_panel_mi_update_cmd_set(struct dsi_panel *panel,
		enum dsi_cmd_set_type type, u8 reg, const u8 *val, size_t len)
{
	struct dsi_panel_cmd_set *set;
	struct dsi_cmd_update *update;
	struct dsi_cmd_desc *cmd;
	u8 *payload;
	int i, rc = -ENOENT;

	if (!panel->cur_mode || !panel->cur_mode->priv_info)
		return -EINVAL;

	set = &panel->cur_mode->priv_info->cmd_sets[type];
	for (i = 0; i < set->update_count; i++) {
		update = &set->updates[i];
		if (update->reg != reg || update->len != len ||
		    update->index >= set->count)
			continue;

		cmd = &set->cmds[update->index];
		payload = (u8 *)cmd->msg.tx_buf;
		if (cmd->msg.tx_len < len + 1 || payload[0] != reg) {
			DSI_ERR("[%s] invalid update of 0x%02x in set %d\n",
					panel->name, reg, type);
			continue;
		}

		memcpy(&payload[1], val, len);
		rc = 0;
	}

	return rc;
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

	/* Doze brightness is part of the doze commands */
	if (mi->doze && dsi_panel_mi_has_doze(panel)) {
		if (*bl_lvl)
			dsi_panel_mi_set_doze(panel, true);
		return true;
	}

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

/* Called whenever the panel itself leaves HBM and doze */
void dsi_panel_mi_reset_state(struct dsi_panel *panel)
{
	panel->mi.hbm_active = false;
	panel->mi.doze = false;
	panel->mi.doze_active = DSI_PANEL_MI_DOZE_OFF;
}

bool dsi_panel_mi_has_doze(struct dsi_panel *panel)
{
	if (!panel->cur_mode || !panel->cur_mode->priv_info)
		return false;

	return panel->cur_mode->priv_info->cmd_sets[DSI_CMD_SET_MI_DOZE_LBM].count;
}

int dsi_panel_mi_set_doze(struct dsi_panel *panel, bool doze)
{
	struct dsi_panel_mi *mi = &panel->mi;
	enum dsi_panel_mi_doze target = DSI_PANEL_MI_DOZE_OFF;
	enum dsi_cmd_set_type type;
	int rc;

	mi->doze = doze;

	if (doze)
		target = mi->last_bl_lvl > mi->doze_hbm_threshold ?
				DSI_PANEL_MI_DOZE_HBM : DSI_PANEL_MI_DOZE_LBM;

	if (target == mi->doze_active)
		return 0;

	switch (target) {
	case DSI_PANEL_MI_DOZE_HBM:
		type = DSI_CMD_SET_MI_DOZE_HBM;
		break;
	case DSI_PANEL_MI_DOZE_LBM:
		type = DSI_CMD_SET_MI_DOZE_LBM;
		break;
	default:
		type = DSI_CMD_SET_NOLP;
		if (mi->doze_nolp)
			type = mi->doze_active == DSI_PANEL_MI_DOZE_HBM ?
					DSI_CMD_SET_MI_DOZE_HBM_NOLP :
					DSI_CMD_SET_MI_DOZE_LBM_NOLP;
		break;
	}

	rc = dsi_panel_tx_cmd_set(panel, type);
	if (rc)
		DSI_ERR("[%s] failed to set doze mode %d, rc=%d\n",
				panel->name, target, rc);

	mi->doze_active = target;

	return rc;
}
