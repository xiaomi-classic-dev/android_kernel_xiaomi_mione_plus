/* Copyright (c) 2012 The Linux Foundation. All rights reserved.
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

#include <asm/mach-types.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/mfd/pmic8901.h>
#include <mach/board.h>
#include <mach/board-msm8660.h>
#include <mach/gpiomux.h>
#include <mach/msm_bus_board.h>
#include <mach/camera.h>
#include "devices-msm8x60.h"
#include "devices.h"

/* #define GPIO_EXT_CAMIF_PWR_EN1 (PM8901_MPP_BASE + PM8901_MPPS + 13) */
#define GPIO_EXT_CAMIF_PWR_EN1 144
#define GPIO_WEB_CAMIF_STANDBY1 (PM8901_MPP_BASE + PM8901_MPPS + 60)
#ifdef CONFIG_MSM_CAMERA_FLASH
#define VFE_CAMIF_TIMER1_GPIO 29
#define VFE_CAMIF_TIMER2_GPIO 30
#define VFE_CAMIF_TIMER3_GPIO_INT 31
#define FUSION_VFE_CAMIF_TIMER1_GPIO 42

static struct msm_camera_sensor_flash_src msm_flash_src = {
	.flash_sr_type = MSM_CAMERA_FLASH_SRC_EXT,
/*      ._fsrc.ext_driver_src.led_en = VFE_CAMIF_TIMER1_GPIO,
	._fsrc.ext_driver_src.led_flash_en = VFE_CAMIF_TIMER2_GPIO, */
	._fsrc.ext_driver_src.flash_id = MAM_CAMERA_EXT_LED_FLASH_SC628A,
};
static struct msm_camera_sensor_strobe_flash_data strobe_flash_xenon = {
	.flash_trigger = VFE_CAMIF_TIMER2_GPIO,
	.flash_charge = VFE_CAMIF_TIMER1_GPIO,
	.flash_charge_done = VFE_CAMIF_TIMER3_GPIO_INT,
	.flash_recharge_duration = 50000,
	.irq = MSM_GPIO_TO_INT(VFE_CAMIF_TIMER3_GPIO_INT),
};
#endif

static struct msm_bus_vectors cam_init_vectors[] = {
	{
		.src = MSM_BUS_MASTER_VFE,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 0,
		.ib  = 0,
	},
	{
		.src = MSM_BUS_MASTER_VFE,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 0,
		.ib  = 0,
	},
	{
		.src = MSM_BUS_MASTER_VPE,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 0,
		.ib  = 0,
	},
	{
		.src = MSM_BUS_MASTER_VPE,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 0,
		.ib  = 0,
	},
	{
		.src = MSM_BUS_MASTER_JPEG_ENC,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 0,
		.ib  = 0,
	},
	{
		.src = MSM_BUS_MASTER_JPEG_ENC,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 0,
		.ib  = 0,
	},
};

static struct msm_bus_vectors cam_preview_vectors[] = {
	{
		.src = MSM_BUS_MASTER_VFE,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 0,
		.ib  = 0,
	},
	{
		.src = MSM_BUS_MASTER_VFE,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 283115520,
		.ib  = 452984832,
	},
	{
		.src = MSM_BUS_MASTER_VPE,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 0,
		.ib  = 0,
	},
	{
		.src = MSM_BUS_MASTER_VPE,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 0,
		.ib  = 0,
	},
	{
		.src = MSM_BUS_MASTER_JPEG_ENC,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 0,
		.ib  = 0,
	},
	{
		.src = MSM_BUS_MASTER_JPEG_ENC,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 0,
		.ib  = 0,
	},
};

static struct msm_bus_vectors cam_video_vectors[] = {
	{
		.src = MSM_BUS_MASTER_VFE,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 283115520,
		.ib  = 452984832,
	},
	{
		.src = MSM_BUS_MASTER_VFE,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 283115520,
		.ib  = 452984832,
	},
	{
		.src = MSM_BUS_MASTER_VPE,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 319610880,
		.ib  = 511377408,
	},
	{
		.src = MSM_BUS_MASTER_VPE,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 0,
		.ib  = 0,
	},
	{
		.src = MSM_BUS_MASTER_JPEG_ENC,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 0,
		.ib  = 0,
	},
	{
		.src = MSM_BUS_MASTER_JPEG_ENC,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 0,
		.ib  = 0,
	},
};

static struct msm_bus_vectors cam_snapshot_vectors[] = {
	{
		.src = MSM_BUS_MASTER_VFE,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 566231040,
		.ib  = 905969664,
	},
	{
		.src = MSM_BUS_MASTER_VFE,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 69984000,
		.ib  = 111974400,
	},
	{
		.src = MSM_BUS_MASTER_VPE,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 0,
		.ib  = 0,
	},
	{
		.src = MSM_BUS_MASTER_VPE,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 0,
		.ib  = 0,
	},
	{
		.src = MSM_BUS_MASTER_JPEG_ENC,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 320864256,
		.ib  = 513382810,
	},
	{
		.src = MSM_BUS_MASTER_JPEG_ENC,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 320864256,
		.ib  = 513382810,
	},
};

static struct msm_bus_vectors cam_zsl_vectors[] = {
	{
		.src = MSM_BUS_MASTER_VFE,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 566231040,
		.ib  = 905969664,
	},
	{
		.src = MSM_BUS_MASTER_VFE,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 706199040,
		.ib  = 1129918464,
	},
	{
		.src = MSM_BUS_MASTER_VPE,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 0,
		.ib  = 0,
	},
	{
		.src = MSM_BUS_MASTER_VPE,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 0,
		.ib  = 0,
	},
	{
		.src = MSM_BUS_MASTER_JPEG_ENC,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 320864256,
		.ib  = 513382810,
	},
	{
		.src = MSM_BUS_MASTER_JPEG_ENC,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 320864256,
		.ib  = 513382810,
	},
};

static struct msm_bus_vectors cam_stereo_video_vectors[] = {
	{
		.src = MSM_BUS_MASTER_VFE,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 212336640,
		.ib  = 339738624,
	},
	{
		.src = MSM_BUS_MASTER_VFE,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 25090560,
		.ib  = 40144896,
	},
	{
		.src = MSM_BUS_MASTER_VPE,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 239708160,
		.ib  = 383533056,
	},
	{
		.src = MSM_BUS_MASTER_VPE,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 79902720,
		.ib  = 127844352,
	},
	{
		.src = MSM_BUS_MASTER_JPEG_ENC,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 0,
		.ib  = 0,
	},
	{
		.src = MSM_BUS_MASTER_JPEG_ENC,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 0,
		.ib  = 0,
	},
};

static struct msm_bus_vectors cam_stereo_snapshot_vectors[] = {
	{
		.src = MSM_BUS_MASTER_VFE,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 0,
		.ib  = 0,
	},
	{
		.src = MSM_BUS_MASTER_VFE,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 300902400,
		.ib  = 481443840,
	},
	{
		.src = MSM_BUS_MASTER_VPE,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 230307840,
		.ib  = 368492544,
	},
	{
		.src = MSM_BUS_MASTER_VPE,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 245113344,
		.ib  = 392181351,
	},
	{
		.src = MSM_BUS_MASTER_JPEG_ENC,
		.dst = MSM_BUS_SLAVE_SMI,
		.ab  = 106536960,
		.ib  = 170459136,
	},
	{
		.src = MSM_BUS_MASTER_JPEG_ENC,
		.dst = MSM_BUS_SLAVE_EBI_CH0,
		.ab  = 106536960,
		.ib  = 170459136,
	},
};

static struct msm_bus_paths cam_bus_client_config[] = {
	{
		ARRAY_SIZE(cam_init_vectors),
		cam_init_vectors,
	},
	{
		ARRAY_SIZE(cam_preview_vectors),
		cam_zsl_vectors,
	},
	{
		ARRAY_SIZE(cam_video_vectors),
		cam_zsl_vectors,
	},
	{
		ARRAY_SIZE(cam_snapshot_vectors),
		cam_snapshot_vectors,
	},
	{
		ARRAY_SIZE(cam_zsl_vectors),
		cam_zsl_vectors,
	},
	{
		ARRAY_SIZE(cam_stereo_video_vectors),
		cam_stereo_video_vectors,
	},
	{
		ARRAY_SIZE(cam_stereo_snapshot_vectors),
		cam_stereo_snapshot_vectors,
	},
};

static struct msm_bus_scale_pdata cam_bus_client_pdata = {
		cam_bus_client_config,
		ARRAY_SIZE(cam_bus_client_config),
		.name = "msm_camera",
};

static struct msm_camera_device_platform_data msm_camera_csi_device_data[] = {
	{
		.csid_core = 0,
		.is_vpe    = 1,
		.cam_bus_scale_table = &cam_bus_client_pdata,
		.ioclk = {
			.vfe_clk_rate =	266667000,
		},
	},
	{
		.csid_core = 1,
		.is_vpe    = 1,
		.cam_bus_scale_table = &cam_bus_client_pdata,
		.ioclk = {
			.vfe_clk_rate =	228570000,
		},
	},
};
static struct camera_vreg_t msm_8x60_back_cam_vreg[] = {
	{"cam_vana", REG_LDO, 2850000, 2850000, -1},
	{"cam_vio", REG_VS, 0, 0, 0},
	{"cam_vvcm", REG_LDO, 2800000, 2800000, -1},
};

static struct camera_vreg_t msm_8x60_back_cam_vreg1[] = {
	{"cam_vana", REG_LDO, 2850000, 2850000, -1},
	{"cam_vdig", REG_LDO, 1200000, 1200000, -1},
	{"cam_vio", REG_VS, 0, 0, 0},
	{"cam_vvcm", REG_LDO, 2800000, 2800000, -1},
};

static struct camera_vreg_t msm_8x60_front_cam_vreg[] = {
	{"cam_vana", REG_LDO, 2850000, 2850000, -1},
	{"cam_vdig", REG_LDO, 1200000, 1200000, -1},
	{"cam_vio", REG_VS, 0, 0, 0},
};

static struct gpio msm8x60_common_cam_gpio[] = {
	{32, GPIOF_DIR_IN, "CAMIF_MCLK"},
	{47, GPIOF_DIR_IN, "CAMIF_I2C_DATA"},
	{48, GPIOF_DIR_IN, "CAMIF_I2C_CLK"},
/*	{105, GPIOF_DIR_IN, "STANDBY"}, */
};

static struct gpio msm8x60_back_cam_gpio[] = {
	{GPIO_EXT_CAMIF_PWR_EN1, GPIOF_DIR_OUT, "CAMIF_PWR_EN"},
/*	{106, GPIOF_DIR_OUT, "CAM_RESET"}, */
};

static struct msm_gpio_set_tbl msm8x60_back_cam_gpio_set_tbl[] = {
	{GPIO_EXT_CAMIF_PWR_EN1, GPIOF_OUT_INIT_LOW, 10000},
	{GPIO_EXT_CAMIF_PWR_EN1, GPIOF_OUT_INIT_HIGH, 5000},
/*	{106, GPIOF_OUT_INIT_LOW, 1000}, */
/*	{106, GPIOF_OUT_INIT_HIGH, 4000}, */
};

static struct msm_camera_gpio_conf msm_8x60_back_cam_gpio_conf = {
	.cam_gpio_common_tbl = msm8x60_common_cam_gpio,
	.cam_gpio_common_tbl_size = ARRAY_SIZE(msm8x60_common_cam_gpio),
	.cam_gpio_req_tbl = msm8x60_back_cam_gpio,
	.cam_gpio_req_tbl_size = ARRAY_SIZE(msm8x60_back_cam_gpio),
	.cam_gpio_set_tbl = msm8x60_back_cam_gpio_set_tbl,
	.cam_gpio_set_tbl_size = ARRAY_SIZE(msm8x60_back_cam_gpio_set_tbl),
};


static struct i2c_board_info imx074_actuator_i2c_info = {
	I2C_BOARD_INFO("msm_actuator", 0x11),
};

static struct msm_actuator_info imx074_actuator_info = {
	.board_info     = &imx074_actuator_i2c_info,
	.cam_name   = MSM_ACTUATOR_MAIN_CAM_0,
	.bus_id         = MSM_GSBI4_QUP_I2C_BUS_ID,
	.vcm_enable     = 0,
};

static struct msm_camera_sensor_flash_data flash_imx074 = {
	.flash_type	= MSM_CAMERA_FLASH_LED,
#ifdef CONFIG_MSM_CAMERA_FLASH
	.flash_src	= &msm_flash_src,
#endif
};

static struct msm_camera_sensor_platform_info sensor_board_info_imx074 = {
	.mount_angle	= 180,
	.cam_vreg = msm_8x60_back_cam_vreg,
	.num_vreg = ARRAY_SIZE(msm_8x60_back_cam_vreg),
	.gpio_conf = &msm_8x60_back_cam_gpio_conf,
};

static struct msm_camera_sensor_info msm_camera_sensor_imx074_data = {
	.sensor_name	= "imx074",
	.pdata	= &msm_camera_csi_device_data[0],
	.flash_data	= &flash_imx074,
	.strobe_flash_data = &strobe_flash_xenon,
	.sensor_platform_info = &sensor_board_info_imx074,
	.csi_if	= 1,
	.camera_type = BACK_CAMERA_2D,
	.actuator_info = &imx074_actuator_info
};

static struct i2c_board_info mt9e013_actuator_i2c_info = {
	I2C_BOARD_INFO("msm_actuator", 0x5C),
};

static struct msm_actuator_info mt9e013_actuator_info = {
	.board_info = &mt9e013_actuator_i2c_info,
	.cam_name = MSM_ACTUATOR_MAIN_CAM_2,
	.bus_id = MSM_GSBI4_QUP_I2C_BUS_ID,
	.vcm_enable = 0,
};

static struct msm_camera_sensor_flash_data flash_mt9e013 = {
	.flash_type = MSM_CAMERA_FLASH_LED,
	.flash_src = &msm_flash_src,

};

static struct msm_camera_sensor_platform_info sensor_board_info_mt9e013 = {
	.mount_angle = 90,
	.cam_vreg = msm_8x60_back_cam_vreg,
	.num_vreg = ARRAY_SIZE(msm_8x60_back_cam_vreg),
	.gpio_conf = &msm_8x60_back_cam_gpio_conf,
};

static struct msm_camera_sensor_info msm_camera_sensor_mt9e013_data = {
	.sensor_name	= "mt9e013",
	.pdata	= &msm_camera_csi_device_data[0],
	.flash_data	= &flash_mt9e013,
	.sensor_platform_info = &sensor_board_info_mt9e013,
	.csi_if	= 1,
	.camera_type = BACK_CAMERA_2D,
	.actuator_info = &mt9e013_actuator_info
};

static struct i2c_board_info s5k3h2_actuator_i2c_info = {
	I2C_BOARD_INFO("msm_actuator", 0x18),
};

static struct msm_actuator_info s5k3h2_actuator_info = {
	.board_info = &s5k3h2_actuator_i2c_info,
	.cam_name = MSM_ACTUATOR_MAIN_CAM_1,
	.bus_id = MSM_GSBI4_QUP_I2C_BUS_ID,
	.vcm_enable = 0,
};

static struct msm_camera_sensor_flash_data flash_s5k3h2 = {
	.flash_type = MSM_CAMERA_FLASH_LED,
	.flash_src = &msm_flash_src,
};

static struct msm_camera_sensor_platform_info sensor_board_info_s5k3h2 = {
	.mount_angle = 90,
	.cam_vreg = msm_8x60_back_cam_vreg1,
	.num_vreg = ARRAY_SIZE(msm_8x60_back_cam_vreg1),
	.gpio_conf = &msm_8x60_back_cam_gpio_conf,
};

static struct msm_camera_sensor_info msm_camera_sensor_s5k3h2_data = {
	.sensor_name = "s5k3h2",
	.pdata = &msm_camera_csi_device_data[0],
	.flash_data = &flash_s5k3h2,
	.sensor_platform_info = &sensor_board_info_s5k3h2,
	.csi_if = 1,
	.camera_type = BACK_CAMERA_2D,
	.actuator_info = &s5k3h2_actuator_info
};

static struct msm_camera_sensor_flash_data flash_imx105 = {
	.flash_type = MSM_CAMERA_FLASH_LED,
	.flash_src = &msm_flash_src,
};

static struct i2c_board_info imx105_actuator_i2c_info = {
	I2C_BOARD_INFO("msm_actuator", 0x12),
};

static struct msm_actuator_info imx105_actuator_info = {
	.board_info = &imx105_actuator_i2c_info,
	.cam_name = MSM_ACTUATOR_MAIN_CAM_0,
	.bus_id = MSM_GSBI4_QUP_I2C_BUS_ID,
	.vcm_enable = 0,
};

static struct msm_camera_sensor_platform_info sensor_board_info_imx105 = {
	.mount_angle = 90,
	.cam_vreg = msm_8x60_back_cam_vreg1,
	.num_vreg = ARRAY_SIZE(msm_8x60_back_cam_vreg1),
	.gpio_conf = &msm_8x60_back_cam_gpio_conf,
};

static struct msm_camera_sensor_info msm_camera_sensor_imx105_data = {
	.sensor_name = "imx105",
	.pdata = &msm_camera_csi_device_data[0],
	.flash_data = &flash_imx105,
	.sensor_platform_info = &sensor_board_info_imx105,
	.csi_if = 1,
	.camera_type = BACK_CAMERA_2D,
	.actuator_info = &imx105_actuator_info
};

static struct gpio ov7692_cam_gpio[] = {
	{GPIO_WEB_CAMIF_STANDBY1, GPIOF_DIR_OUT, "CAM_EN"},
};

static struct msm_gpio_set_tbl ov7692_cam_gpio_set_tbl[] = {
	{GPIO_WEB_CAMIF_STANDBY1, GPIOF_OUT_INIT_LOW, 10000},
};

static struct msm_camera_gpio_conf ov7692_cam_gpio_conf = {
	.cam_gpio_common_tbl = msm8x60_common_cam_gpio,
	.cam_gpio_common_tbl_size = ARRAY_SIZE(msm8x60_common_cam_gpio),
	.cam_gpio_req_tbl = ov7692_cam_gpio,
	.cam_gpio_req_tbl_size = ARRAY_SIZE(ov7692_cam_gpio),
	.cam_gpio_set_tbl = ov7692_cam_gpio_set_tbl,
	.cam_gpio_set_tbl_size = ARRAY_SIZE(ov7692_cam_gpio_set_tbl),
};

static struct msm_camera_sensor_flash_data flash_ov7692 = {
	.flash_type	= MSM_CAMERA_FLASH_NONE,
};

static struct msm_camera_sensor_platform_info sensor_board_info_ov7692 = {
	.mount_angle	= 0,
	.cam_vreg = msm_8x60_back_cam_vreg,
	.num_vreg = ARRAY_SIZE(msm_8x60_back_cam_vreg),
	.gpio_conf = &ov7692_cam_gpio_conf,
};

static struct msm_camera_sensor_info msm_camera_sensor_ov7692_data = {
	.sensor_name	= "ov7692",
	.pdata	= &msm_camera_csi_device_data[1],
	.flash_data	= &flash_ov7692,
	.sensor_platform_info = &sensor_board_info_ov7692,
	.csi_if	= 1,
	.camera_type = FRONT_CAMERA_2D,
};

static struct gpio msm_8x60_front_cam_gpio[] = {
	{139, GPIOF_DIR_OUT, "CAM_EN"},
};

static struct msm_gpio_set_tbl msm_8x60_front_cam_gpio_set_tbl[] = {
	{139, GPIOF_OUT_INIT_LOW, 10000},
	{139, GPIOF_OUT_INIT_HIGH, 10000},
};

static struct msm_camera_gpio_conf msm_8x60_front_cam_gpio_conf = {
	.cam_gpio_common_tbl = msm8x60_common_cam_gpio,
	.cam_gpio_common_tbl_size = ARRAY_SIZE(msm8x60_common_cam_gpio),
	.cam_gpio_req_tbl = msm_8x60_front_cam_gpio,
	.cam_gpio_req_tbl_size = ARRAY_SIZE(msm_8x60_front_cam_gpio),
	.cam_gpio_set_tbl = msm_8x60_front_cam_gpio_set_tbl,
	.cam_gpio_set_tbl_size = ARRAY_SIZE(msm_8x60_front_cam_gpio_set_tbl),
};

static struct msm_camera_sensor_flash_data flash_imx132 = {
	.flash_type = MSM_CAMERA_FLASH_NONE,
};

static struct msm_camera_sensor_platform_info sensor_board_info_imx132 = {
	.mount_angle = 270,
	.cam_vreg = msm_8x60_front_cam_vreg,
	.num_vreg = ARRAY_SIZE(msm_8x60_front_cam_vreg),
	.gpio_conf = &msm_8x60_front_cam_gpio_conf,
};

static struct msm_camera_sensor_info msm_camera_sensor_imx132_data = {
	.sensor_name = "imx132",
	.pdata = &msm_camera_csi_device_data[1],
	.flash_data = &flash_imx132,
	.sensor_platform_info = &sensor_board_info_imx132,
	.csi_if = 1,
	.camera_type = FRONT_CAMERA_2D,
};

static struct platform_device msm_camera_server = {
	.name = "msm_cam_server",
	.id = 0,
};

void __init msm8x60_init_cam(void)
{
	platform_device_register(&msm_camera_server);
	platform_device_register(&msm_device_csic0);
	platform_device_register(&msm_device_csic1);
	platform_device_register(&msm_device_vfe);
	platform_device_register(&msm_device_vpe);
}

#ifdef CONFIG_I2C
static struct i2c_board_info msm8x60_camera_i2c_boardinfo[] = {
	{
	I2C_BOARD_INFO("imx074", 0x1A),
	.platform_data = &msm_camera_sensor_imx074_data,
	},
	{
	I2C_BOARD_INFO("mt9e013", 0x6C),
	.platform_data = &msm_camera_sensor_mt9e013_data,
	},
	{
	I2C_BOARD_INFO("s5k3h2", 0x6E),
	.platform_data = &msm_camera_sensor_s5k3h2_data,
	},
	{
	I2C_BOARD_INFO("imx105", 0x20),
	.platform_data = &msm_camera_sensor_imx105_data,
	},
	{
	I2C_BOARD_INFO("imx132", 0x36),
	.platform_data = &msm_camera_sensor_imx132_data,
	},
	{
	I2C_BOARD_INFO("ov7692", 0x78),
	.platform_data = &msm_camera_sensor_ov7692_data,
	},
};

struct msm_camera_board_info msm8x60_camera_board_info = {
	.board_info = msm8x60_camera_i2c_boardinfo,
	.num_i2c_board_info = ARRAY_SIZE(msm8x60_camera_i2c_boardinfo),
};
#endif
