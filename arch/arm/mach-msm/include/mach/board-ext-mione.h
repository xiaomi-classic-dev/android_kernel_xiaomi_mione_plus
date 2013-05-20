/* arch/arm/mach-msm/include/mach/board-ext-mione.h
 *
 * MIONE board.h extensions
 *
 * Copyright (C) 2013 CyanogenMod
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __ASM_ARCH_MSM_BOARD_EXT_MIONE_H
#define __ASM_ARCH_MSM_BOARD_EXT_MIONE_H

#ifdef CONFIG_MSM_CAMERA
enum msm_camera_csi_data_format {
	CSI_8BIT,
	CSI_10BIT,
	CSI_12BIT,
};

struct msm_camera_csi_params {
	enum msm_camera_csi_data_format data_format;
	uint8_t lane_cnt;
	uint8_t lane_assign;
	uint8_t settle_cnt;
	uint8_t dpcm_scheme;
};

enum camera_vreg_type {
	REG_LDO,
	REG_VS,
	REG_GPIO,
};

struct camera_vreg_t {
	char *reg_name;
	enum camera_vreg_type type;
	int min_voltage;
	int max_voltage;
	int op_mode;
};
#endif /* CONFIG_MSM_CAMERA */

#endif /* __ASM_ARCH_MSM_BOARD_EXT_MIONE_H */
