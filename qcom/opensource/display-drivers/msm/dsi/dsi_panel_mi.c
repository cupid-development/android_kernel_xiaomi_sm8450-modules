// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 The LineageOS Project
 */

#include <linux/slab.h>
#include <video/mipi_display.h>

#include "dsi_panel.h"
#include "dsi_panel_mi.h"
#include "dsi_panel_mi_alpha.h"

static void dsi_panel_mi_parse_lhbm(struct dsi_panel *panel)
{
	struct dsi_parser_utils *utils = &panel->utils;
	struct dsi_panel_mi *mi = &panel->mi;
	const char *name;
	int i;

	mi->lhbm = utils->read_bool(utils->data, "mi,local-hbm-enabled");
	if (!mi->lhbm)
		return;

	utils->read_u32(utils->data, "mi,local-hbm-ui-ready-delay-num-frame",
			&mi->lhbm_ui_ready_frames);
	utils->read_u32(utils->data, "mi,local-hbm-df-reg-min",
			&mi->lhbm_df_min);
	utils->read_u32(utils->data, "mi,local-hbm-b2-reg-min",
			&mi->lhbm_b2_min);

	if (utils->read_string(utils->data, "mi,local-hbm-alpha-table", &name))
		return;

	for (i = 0; i < ARRAY_SIZE(lhbm_alpha_tables); i++) {
		if (strcmp(name, lhbm_alpha_tables[i].name))
			continue;

		mi->lhbm_alpha = lhbm_alpha_tables[i].alpha;
		mi->lhbm_alpha_size = lhbm_alpha_tables[i].size;
		break;
	}

	if (!mi->lhbm_alpha) {
		DSI_ERR("[%s] unknown local hbm alpha table %s\n",
				panel->name, name);
		return;
	}

	mi->lhbm_alpha_max = mi->lhbm_alpha[mi->lhbm_alpha_size - 1];
	utils->read_u32(utils->data, "mi,local-hbm-alpha-max",
			&mi->lhbm_alpha_max);
	utils->read_u32(utils->data, "mi,local-hbm-alpha-or",
			&mi->lhbm_alpha_or);
}

void dsi_panel_mi_parse(struct dsi_panel *panel)
{
	struct dsi_parser_utils *utils = &panel->utils;
	struct dsi_panel_mi *mi = &panel->mi;

	mi->hbm_reg53 = utils->read_bool(utils->data, "mi,hbm-reg53-enabled");
	mi->doze_nolp = utils->read_bool(utils->data,
			"mi,aod-nolp-command-enabled");

	utils->read_u32(utils->data, "mi,doze-hbm-dbv-level", &mi->doze_hbm_dbv);
	utils->read_u32(utils->data, "mi,doze-lbm-dbv-level", &mi->doze_lbm_dbv);
	if (mi->doze_hbm_dbv && mi->doze_lbm_dbv)
		mi->doze_hbm_threshold = (mi->doze_hbm_dbv + mi->doze_lbm_dbv) / 2;
	else
		mi->doze_hbm_threshold = 100;

	dsi_panel_mi_parse_lhbm(panel);
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

	/* Applied when local HBM is turned off */
	if (mi->lhbm_nits)
		return true;

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

/* Called when the panel is turned on */
void dsi_panel_mi_reset_state(struct dsi_panel *panel)
{
	panel->mi.hbm_active = false;
	panel->mi.doze = false;
	panel->mi.doze_active = DSI_PANEL_MI_DOZE_OFF;
	panel->mi.lhbm_nits = 0;
}

bool dsi_panel_mi_has_doze(struct dsi_panel *panel)
{
	if (!panel->cur_mode || !panel->cur_mode->priv_info)
		return false;

	return panel->cur_mode->priv_info->cmd_sets[DSI_CMD_SET_MI_DOZE_LBM].count;
}

static enum dsi_panel_mi_doze dsi_panel_mi_doze_target(struct dsi_panel_mi *mi)
{
	return mi->last_bl_lvl > mi->doze_hbm_threshold ?
			DSI_PANEL_MI_DOZE_HBM : DSI_PANEL_MI_DOZE_LBM;
}

int dsi_panel_mi_set_doze(struct dsi_panel *panel, bool doze)
{
	struct dsi_panel_mi *mi = &panel->mi;
	enum dsi_panel_mi_doze target = DSI_PANEL_MI_DOZE_OFF;
	enum dsi_cmd_set_type type;
	int rc;

	mi->doze = doze;

	/* Applied when local HBM is turned off */
	if (mi->lhbm_nits)
		return 0;

	if (doze)
		target = dsi_panel_mi_doze_target(mi);

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

static void dsi_panel_mi_put_be(u8 *buf, u32 val, size_t len)
{
	int i;

	for (i = len - 1; i >= 0; i--, val >>= 8)
		buf[i] = val & 0xff;
}

static void dsi_panel_mi_update_param(struct dsi_panel *panel,
		enum dsi_cmd_set_type type, u8 reg, u32 val)
{
	struct dsi_panel_cmd_set *set =
			&panel->cur_mode->priv_info->cmd_sets[type];
	u8 buf[4];
	int i;

	for (i = 0; i < set->update_count; i++) {
		if (set->updates[i].reg != reg ||
		    set->updates[i].len > sizeof(buf))
			continue;

		dsi_panel_mi_put_be(buf, val, set->updates[i].len);
		dsi_panel_mi_update_cmd_set(panel, type, reg, buf,
				set->updates[i].len);
		return;
	}
}

/* DBV of the normal mode backlight range */
static u32 dsi_panel_mi_normal_dbv(struct dsi_panel *panel)
{
	struct dsi_panel_mi *mi = &panel->mi;

	if (!mi->hbm_reg53)
		return mi->last_bl_lvl;

	return min(mi->last_bl_lvl, panel->bl_config.bl_max_level / 2);
}

static int dsi_panel_mi_lhbm_on(struct dsi_panel *panel)
{
	struct dsi_panel_mi *mi = &panel->mi;
	enum dsi_cmd_set_type type;
	u32 dbv, alpha;
	int rc;

	if (mi->doze_active != DSI_PANEL_MI_DOZE_OFF) {
		type = DSI_CMD_SET_MI_LHBM_HLPM_WHITE_1000NIT;
		dbv = mi->doze_active == DSI_PANEL_MI_DOZE_HBM ?
				mi->doze_hbm_dbv : mi->doze_lbm_dbv;
	} else {
		type = DSI_CMD_SET_MI_LHBM_NORMAL_WHITE_1000NIT;
		dbv = dsi_panel_mi_normal_dbv(panel);
	}

	dsi_panel_mi_update_param(panel, type, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
			dbv);

	if (mi->lhbm_alpha) {
		alpha = dbv < mi->lhbm_alpha_size ? mi->lhbm_alpha[dbv] :
				mi->lhbm_alpha_max;
		dsi_panel_mi_update_param(panel, type, 0x87,
				alpha | mi->lhbm_alpha_or);
	}

	if (mi->lhbm_df_min)
		dsi_panel_mi_update_param(panel, type, 0xdf,
				max(dbv * 4, mi->lhbm_df_min));

	if (mi->lhbm_b2_min)
		dsi_panel_mi_update_param(panel, type, 0xb2,
				max(dbv, mi->lhbm_b2_min));

	rc = dsi_panel_tx_cmd_set(panel, type);
	if (rc)
		return rc;

	/* Local HBM commands rewrite 0x53 and leave doze */
	mi->hbm_active = false;
	mi->doze_active = DSI_PANEL_MI_DOZE_OFF;

	return 0;
}

static int dsi_panel_mi_lhbm_off(struct dsi_panel *panel)
{
	struct dsi_panel_mi *mi = &panel->mi;
	u32 normal_max = panel->bl_config.bl_max_level / 2;
	enum dsi_panel_mi_doze doze = DSI_PANEL_MI_DOZE_OFF;
	int rc;

	if (mi->doze)
		doze = dsi_panel_mi_doze_target(mi);

	if (doze != DSI_PANEL_MI_DOZE_OFF) {
		rc = dsi_panel_tx_cmd_set(panel, doze == DSI_PANEL_MI_DOZE_HBM ?
				DSI_CMD_SET_MI_LHBM_OFF_TO_HLPM :
				DSI_CMD_SET_MI_LHBM_OFF_TO_LLPM);
		if (!rc)
			mi->doze_active = doze;
		return rc;
	}

	if (mi->hbm_reg53 && mi->last_bl_lvl > normal_max) {
		dsi_panel_mi_update_param(panel, DSI_CMD_SET_MI_LHBM_OFF_TO_HBM,
				MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
				mi->last_bl_lvl - normal_max - 1);
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LHBM_OFF_TO_HBM);
		if (!rc)
			mi->hbm_active = true;
		return rc;
	}

	dsi_panel_mi_update_param(panel, DSI_CMD_SET_MI_LHBM_OFF_TO_NORMAL,
			MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
			dsi_panel_mi_normal_dbv(panel));
	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LHBM_OFF_TO_NORMAL);
	if (rc || !mi->hbm_reg53)
		return rc;

	/* Restore DDIC dimming, which local HBM turned off */
	return dsi_panel_mi_tx_with_dbv(panel, DSI_CMD_SET_MI_HBM_OFF,
			dsi_panel_mi_normal_dbv(panel));
}

/* Called with panel_lock held */
int dsi_panel_mi_set_lhbm(struct dsi_panel *panel, u32 nits)
{
	struct dsi_panel_mi *mi = &panel->mi;
	int rc;

	if (!mi->lhbm)
		return -ENODEV;

	if (!panel->panel_initialized || !panel->cur_mode ||
	    !panel->cur_mode->priv_info)
		return -EAGAIN;

	if (nits == mi->lhbm_nits)
		return 0;

	rc = nits ? dsi_panel_mi_lhbm_on(panel) :
			dsi_panel_mi_lhbm_off(panel);
	if (rc) {
		DSI_ERR("[%s] failed to set local hbm %u, rc=%d\n",
				panel->name, nits, rc);
		return rc;
	}

	mi->lhbm_nits = nits;

	return 0;
}
