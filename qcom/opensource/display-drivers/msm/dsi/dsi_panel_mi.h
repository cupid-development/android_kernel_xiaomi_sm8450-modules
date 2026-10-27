/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 The LineageOS Project
 */

#ifndef _DSI_PANEL_MI_H_
#define _DSI_PANEL_MI_H_

#include <linux/types.h>

#include "dsi_defs.h"

struct dsi_panel;
struct dsi_panel_cmd_set;
struct dsi_parser_utils;

enum dsi_panel_mi_doze {
	DSI_PANEL_MI_DOZE_OFF,
	DSI_PANEL_MI_DOZE_LBM,
	DSI_PANEL_MI_DOZE_HBM,
};

/**
 * struct dsi_panel_mi - Xiaomi specific panel state
 * @hbm_reg53:    The upper half of the backlight range is sent in HBM mode,
 *                which is selected through register 0x53.
 * @hbm_active:   Panel is currently in HBM mode.
 * @last_bl_lvl:  Last backlight level requested by the display driver.
 * @doze_nolp:    Leave doze through the doze HBM/LBM specific NOLP commands.
 * @doze_hbm_threshold: Doze backlight levels above this select doze HBM.
 * @doze:         Panel is in a low power mode.
 * @doze_active:  Doze brightness mode currently set on the panel.
 */
struct dsi_panel_mi {
	bool hbm_reg53;
	bool hbm_active;
	u32 last_bl_lvl;

	bool doze_nolp;
	u32 doze_hbm_threshold;
	bool doze;
	enum dsi_panel_mi_doze doze_active;
};

void dsi_panel_mi_parse(struct dsi_panel *panel);

void dsi_panel_mi_parse_cmd_update(struct dsi_panel_cmd_set *set,
		const char *name, struct dsi_parser_utils *utils);

int dsi_panel_mi_update_cmd_set(struct dsi_panel *panel,
		enum dsi_cmd_set_type type, u8 reg, const u8 *val, size_t len);

bool dsi_panel_mi_set_backlight(struct dsi_panel *panel, u32 *bl_lvl);

void dsi_panel_mi_leave_hbm(struct dsi_panel *panel);

void dsi_panel_mi_reset_state(struct dsi_panel *panel);

bool dsi_panel_mi_has_doze(struct dsi_panel *panel);

int dsi_panel_mi_set_doze(struct dsi_panel *panel, bool doze);

#endif /* _DSI_PANEL_MI_H_ */
