// SPDX-License-Identifier: GPL-2.0-only
/*
 * ap1302.c - driver for AP1302 mezzanine
 *
 * Copyright (C) 2020, Witekio, Inc.
 *
 * This driver can only provide limited feature on AP1302.
 * Still need enhancement
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/media.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>

#define DRIVER_NAME "ap1302"

#define AP1302_FW_WINDOW_SIZE			0x2000
#define AP1302_FW_WINDOW_OFFSET			0x8000

#define AP1302_MIN_WIDTH			24U
#define AP1302_MIN_HEIGHT 			16U
#define AP1302_MAX_WIDTH			4224U
#define AP1302_MAX_HEIGHT 			4092U

#define AP1302_REG_ADV_MASK			0x80000000
#define AP1302_REG_8BIT(n)			((1 << 26) | (n))
#define AP1302_REG_16BIT(n)			((2 << 26) | (n))
#define AP1302_REG_32BIT(n)			((4 << 26) | (n))
#define AP1302_REG_ADV_8BIT(n)			((1 << 26) | (n) | AP1302_REG_ADV_MASK)
#define AP1302_REG_ADV_16BIT(n)			((2 << 26) | (n) | AP1302_REG_ADV_MASK)
#define AP1302_REG_ADV_32BIT(n)			((4 << 26) | (n) | AP1302_REG_ADV_MASK)
#define AP1302_REG_SIZE(n)			(((n) >> 26) & 0x7)
#define AP1302_REG_ADDR(n)			((n) & 0x43ffffff)
#define AP1302_REG_ADV(n)			((n & AP1302_REG_ADV_MASK) ? 1 : 0)
#define AP1302_REG_PAGE_MASK			0x43fff000
#define AP1302_REG_PAGE(n)			((n) & 0x43fff000)

/* Info Registers */
#define AP1302_CHIP_VERSION			AP1302_REG_16BIT(0x0000)
#define AP1302_CHIP_ID				0x0265
#define AP1302_FRAME_CNT			AP1302_REG_16BIT(0x0002)
#define AP1302_ERROR				AP1302_REG_16BIT(0x0006)
#define AP1302_ERR_FILE				AP1302_REG_32BIT(0x0008)
#define AP1302_ERR_LINE				AP1302_REG_16BIT(0x000c)
#define AP1302_SIPM_ERR_0			AP1302_REG_16BIT(0x0014)
#define AP1302_SIPM_ERR_1			AP1302_REG_16BIT(0x0016)
#define AP1302_CHIP_REV				AP1302_REG_16BIT(0x0050)
#define AP1302_CON_BUF(n)			AP1302_REG_16BIT(0x0a2c + (n))
#define AP1302_CON_BUF_SIZE			512

/* Control Registers */
#define AP1302_DZ_TGT_FCT			AP1302_REG_16BIT(0x1010)
#define AP1302_SFX_MODE				AP1302_REG_16BIT(0x1016)
#define AP1302_SFX_MODE_SFX_NORMAL		(0U << 0)
#define AP1302_SFX_MODE_SFX_ALIEN		(1U << 0)
#define AP1302_SFX_MODE_SFX_ANTIQUE		(2U << 0)
#define AP1302_SFX_MODE_SFX_BW			(3U << 0)
#define AP1302_SFX_MODE_SFX_EMBOSS		(4U << 0)
#define AP1302_SFX_MODE_SFX_EMBOSS_COLORED	(5U << 0)
#define AP1302_SFX_MODE_SFX_GRAYSCALE		(6U << 0)
#define AP1302_SFX_MODE_SFX_NEGATIVE		(7U << 0)
#define AP1302_SFX_MODE_SFX_BLUISH		(8U << 0)
#define AP1302_SFX_MODE_SFX_GREENISH		(9U << 0)
#define AP1302_SFX_MODE_SFX_REDISH		(10U << 0)
#define AP1302_SFX_MODE_SFX_POSTERIZE1		(11U << 0)
#define AP1302_SFX_MODE_SFX_POSTERIZE2		(12U << 0)
#define AP1302_SFX_MODE_SFX_SEPIA1		(13U << 0)
#define AP1302_SFX_MODE_SFX_SEPIA2		(14U << 0)
#define AP1302_SFX_MODE_SFX_SKETCH		(15U << 0)
#define AP1302_SFX_MODE_SFX_SOLARIZE		(16U << 0)
#define AP1302_SFX_MODE_SFX_FOGGY		(17U << 0)
#define AP1302_BUBBLE_OUT_FMT			AP1302_REG_16BIT(0x1164)
#define AP1302_BUBBLE_OUT_FMT_FT_YUV		(3U << 4)
#define AP1302_BUBBLE_OUT_FMT_FT_RGB		(4U << 4)
#define AP1302_BUBBLE_OUT_FMT_FT_YUV_JFIF	(5U << 4)
#define AP1302_BUBBLE_OUT_FMT_FST_RGB_888	(0U << 0)
#define AP1302_BUBBLE_OUT_FMT_FST_RGB_565	(1U << 0)
#define AP1302_BUBBLE_OUT_FMT_FST_RGB_555M	(2U << 0)
#define AP1302_BUBBLE_OUT_FMT_FST_RGB_555L	(3U << 0)
#define AP1302_BUBBLE_OUT_FMT_FST_YUV_422	(0U << 0)
#define AP1302_BUBBLE_OUT_FMT_FST_YUV_420	(1U << 0)
#define AP1302_BUBBLE_OUT_FMT_FST_YUV_400	(2U << 0)
#define AP1302_ATOMIC				AP1302_REG_16BIT(0x1184)
#define AP1302_ATOMIC_MODE			BIT(2)
#define AP1302_ATOMIC_FINISH			BIT(1)
#define AP1302_ATOMIC_RECORD			BIT(0)

/*
 * Preview Context Registers (preview_*). AP1302 have support 3 contexts
 * (Preview, Snapshot, Video), however, the only advantage of using contexts
 * is to potentially reduce the number of I2C writes when switching modes.
 * Let's avoid complexity and confusion and just use preview context.
 */
#define AP1302_PREVIEW_WIDTH			AP1302_REG_16BIT(0x2000)
#define AP1302_PREVIEW_HEIGHT			AP1302_REG_16BIT(0x2002)
#define AP1302_PREVIEW_ROI_X0			AP1302_REG_16BIT(0x2004)
#define AP1302_PREVIEW_ROI_Y0			AP1302_REG_16BIT(0x2006)
#define AP1302_PREVIEW_ROI_X1			AP1302_REG_16BIT(0x2008)
#define AP1302_PREVIEW_ROI_Y1			AP1302_REG_16BIT(0x200a)
#define AP1302_PREVIEW_OUT_FMT			AP1302_REG_16BIT(0x2012)
#define AP1302_PREVIEW_OUT_FMT_IPIPE_BYPASS	BIT(13)
#define AP1302_PREVIEW_OUT_FMT_SS		BIT(12)
#define AP1302_PREVIEW_OUT_FMT_FAKE_EN		BIT(11)
#define AP1302_PREVIEW_OUT_FMT_ST_EN		BIT(10)
#define AP1302_PREVIEW_OUT_FMT_IIS_NONE		(0U << 8)
#define AP1302_PREVIEW_OUT_FMT_IIS_POST_VIEW	(1U << 8)
#define AP1302_PREVIEW_OUT_FMT_IIS_VIDEO	(2U << 8)
#define AP1302_PREVIEW_OUT_FMT_IIS_BUBBLE	(3U << 8)
#define AP1302_PREVIEW_OUT_FMT_FT_JPEG_422	(0U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_JPEG_420	(1U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_YUV		(3U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_RGB		(4U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_YUV_JFIF	(5U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_RAW8		(8U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_RAW10		(9U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_RAW12		(10U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_RAW16		(11U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_DNG8		(12U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_DNG10		(13U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_DNG12		(14U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_DNG16		(15U << 4)
#define AP1302_PREVIEW_OUT_FMT_FST_JPEG_ROTATE	BIT(2)
#define AP1302_PREVIEW_OUT_FMT_FST_JPEG_SCAN	(0U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_JPEG_JFIF	(1U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_JPEG_EXIF	(2U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RGB_888	(0U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RGB_565	(1U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RGB_555M	(2U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RGB_555L	(3U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_YUV_422	(0U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_YUV_420	(1U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_YUV_400	(2U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_SENSOR	(0U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_CAPTURE	(1U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_CP	(2U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_BPC	(3U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_IHDR	(4U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_PP	(5U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_DENSH	(6U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_PM	(7U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_GC	(8U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_CURVE	(9U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_CCONV	(10U << 0)
#define AP1302_PREVIEW_S1_SENSOR_MODE		AP1302_REG_16BIT(0x202e)
#define AP1302_PREVIEW_HINF_CTRL		AP1302_REG_16BIT(0x2030)
#define AP1302_PREVIEW_HINF_CTRL_BT656_LE	BIT(15)
#define AP1302_PREVIEW_HINF_CTRL_BT656_16BIT	BIT(14)
#define AP1302_PREVIEW_HINF_CTRL_MUX_DELAY(n)	((n) << 8)
#define AP1302_PREVIEW_HINF_CTRL_LV_POL		BIT(7)
#define AP1302_PREVIEW_HINF_CTRL_FV_POL		BIT(6)
#define AP1302_PREVIEW_HINF_CTRL_MIPI_CONT_CLK	BIT(5)
#define AP1302_PREVIEW_HINF_CTRL_SPOOF		BIT(4)
#define AP1302_PREVIEW_HINF_CTRL_MIPI_MODE	BIT(3)
#define AP1302_PREVIEW_HINF_CTRL_MIPI_LANES(n)	((n) << 0)

/* IQ Registers */
#define AP1302_AE_BV_OFF			AP1302_REG_16BIT(0x5014)
#define AP1302_AWB_CTRL				AP1302_REG_16BIT(0x5100)
#define AP1302_AWB_CTRL_RECALC			BIT(13)
#define AP1302_AWB_CTRL_POSTGAIN		BIT(12)
#define AP1302_AWB_CTRL_UNGAIN			BIT(11)
#define AP1302_AWB_CTRL_CLIP			BIT(10)
#define AP1302_AWB_CTRL_SKY			BIT(9)
#define AP1302_AWB_CTRL_FLASH			BIT(8)
#define AP1302_AWB_CTRL_FACE_OFF		(0U << 6)
#define AP1302_AWB_CTRL_FACE_IGNORE		(1U << 6)
#define AP1302_AWB_CTRL_FACE_CONSTRAINED	(2U << 6)
#define AP1302_AWB_CTRL_FACE_ONLY		(3U << 6)
#define AP1302_AWB_CTRL_IMM			BIT(5)
#define AP1302_AWB_CTRL_IMM1			BIT(4)
#define AP1302_AWB_CTRL_MODE_OFF		(0U << 0)
#define AP1302_AWB_CTRL_MODE_HORIZON		(1U << 0)
#define AP1302_AWB_CTRL_MODE_A			(2U << 0)
#define AP1302_AWB_CTRL_MODE_CWF		(3U << 0)
#define AP1302_AWB_CTRL_MODE_D50		(4U << 0)
#define AP1302_AWB_CTRL_MODE_D65		(5U << 0)
#define AP1302_AWB_CTRL_MODE_D75		(6U << 0)
#define AP1302_AWB_CTRL_MODE_MANUAL		(7U << 0)
#define AP1302_AWB_CTRL_MODE_MEASURE		(8U << 0)
#define AP1302_AWB_CTRL_MODE_AUTO		(15U << 0)
#define AP1302_AWB_CTRL_MODE_MASK		0x000f
#define AP1302_FLICK_CTRL			AP1302_REG_16BIT(0x5440)
#define AP1302_FLICK_CTRL_FREQ(n)		((n) << 8)
#define AP1302_FLICK_CTRL_ETC_IHDR_UP		BIT(6)
#define AP1302_FLICK_CTRL_ETC_DIS		BIT(5)
#define AP1302_FLICK_CTRL_FRC_OVERRIDE_MAX_ET	BIT(4)
#define AP1302_FLICK_CTRL_FRC_OVERRIDE_UPPER_ET	BIT(3)
#define AP1302_FLICK_CTRL_FRC_EN		BIT(2)
#define AP1302_FLICK_CTRL_MODE_DISABLED		(0U << 0)
#define AP1302_FLICK_CTRL_MODE_MANUAL		(1U << 0)
#define AP1302_FLICK_CTRL_MODE_AUTO		(2U << 0)
#define AP1302_SCENE_CTRL			AP1302_REG_16BIT(0x5454)
#define AP1302_SCENE_CTRL_MODE_NORMAL		(0U << 0)
#define AP1302_SCENE_CTRL_MODE_PORTRAIT		(1U << 0)
#define AP1302_SCENE_CTRL_MODE_LANDSCAPE	(2U << 0)
#define AP1302_SCENE_CTRL_MODE_SPORT		(3U << 0)
#define AP1302_SCENE_CTRL_MODE_CLOSE_UP		(4U << 0)
#define AP1302_SCENE_CTRL_MODE_NIGHT		(5U << 0)
#define AP1302_SCENE_CTRL_MODE_TWILIGHT		(6U << 0)
#define AP1302_SCENE_CTRL_MODE_BACKLIGHT	(7U << 0)
#define AP1302_SCENE_CTRL_MODE_HIGH_SENSITIVE	(8U << 0)
#define AP1302_SCENE_CTRL_MODE_NIGHT_PORTRAIT	(9U << 0)
#define AP1302_SCENE_CTRL_MODE_BEACH		(10U << 0)
#define AP1302_SCENE_CTRL_MODE_DOCUMENT		(11U << 0)
#define AP1302_SCENE_CTRL_MODE_PARTY		(12U << 0)
#define AP1302_SCENE_CTRL_MODE_FIREWORKS	(13U << 0)
#define AP1302_SCENE_CTRL_MODE_SUNSET		(14U << 0)
#define AP1302_SCENE_CTRL_MODE_AUTO		(0xffU << 0)

/* System Registers */
#define AP1302_BOOTDATA_STAGE			AP1302_REG_16BIT(0x6002)
#define AP1302_WARNING(n)			AP1302_REG_16BIT(0x6004 + (n) * 2)
#define AP1302_SENSOR_SELECT			AP1302_REG_16BIT(0x600c)
#define AP1302_SENSOR_SELECT_TP_MODE(n)		((n) << 8)
#define AP1302_SENSOR_SELECT_PATTERN_ON		BIT(7)
#define AP1302_SENSOR_SELECT_MODE_3D_ON		BIT(6)
#define AP1302_SENSOR_SELECT_CLOCK		BIT(5)
#define AP1302_SENSOR_SELECT_SINF_MIPI		BIT(4)
#define AP1302_SENSOR_SELECT_YUV		BIT(2)
#define AP1302_SENSOR_SELECT_SENSOR_TP		(0U << 0)
#define AP1302_SENSOR_SELECT_SENSOR(n)		(((n) + 1) << 0)
#define AP1302_SYS_START			AP1302_REG_16BIT(0x601a)
#define AP1302_SYS_START_PLL_LOCK		BIT(15)
#define AP1302_SYS_START_LOAD_OTP		BIT(12)
#define AP1302_SYS_START_RESTART_ERROR		BIT(11)
#define AP1302_SYS_START_STALL_STATUS		BIT(9)
#define AP1302_SYS_START_STALL_EN		BIT(8)
#define AP1302_SYS_START_STALL_MODE_FRAME	(0U << 6)
#define AP1302_SYS_START_STALL_MODE_DISABLED	(1U << 6)
#define AP1302_SYS_START_STALL_MODE_POWER_DOWN	(2U << 6)
#define AP1302_SYS_START_GO			BIT(4)
#define AP1302_SYS_START_PATCH_FUN		BIT(1)
#define AP1302_SYS_START_PLL_INIT		BIT(0)

/* Misc Registers */
#define AP1302_REG_ADV_START			0xe000
#define AP1302_ADVANCED_BASE			AP1302_REG_32BIT(0xf038)
#define AP1302_SIP_CRC				AP1302_REG_16BIT(0xf052)

/* Advanced System Registers */
#define AP1302_ADV_IRQ_SYS_INTE			AP1302_REG_ADV_32BIT(0x00230000)
#define AP1302_ADV_IRQ_SYS_INTE_TEST_COUNT	BIT(25)
#define AP1302_ADV_IRQ_SYS_INTE_HINF_1		BIT(24)
#define AP1302_ADV_IRQ_SYS_INTE_HINF_0		BIT(23)
#define AP1302_ADV_IRQ_SYS_INTE_SINF_B_MIPI_L	(7U << 20)
#define AP1302_ADV_IRQ_SYS_INTE_SINF_B_MIPI	BIT(19)
#define AP1302_ADV_IRQ_SYS_INTE_SINF_A_MIPI_L	(15U << 14)
#define AP1302_ADV_IRQ_SYS_INTE_SINF_A_MIPI	BIT(13)
#define AP1302_ADV_IRQ_SYS_INTE_SINF		BIT(12)
#define AP1302_ADV_IRQ_SYS_INTE_IPIPE_S		BIT(11)
#define AP1302_ADV_IRQ_SYS_INTE_IPIPE_B		BIT(10)
#define AP1302_ADV_IRQ_SYS_INTE_IPIPE_A		BIT(9)
#define AP1302_ADV_IRQ_SYS_INTE_IP		BIT(8)
#define AP1302_ADV_IRQ_SYS_INTE_TIMER		BIT(7)
#define AP1302_ADV_IRQ_SYS_INTE_SIPM		(3U << 6)
#define AP1302_ADV_IRQ_SYS_INTE_SIPS_ADR_RANGE	BIT(5)
#define AP1302_ADV_IRQ_SYS_INTE_SIPS_DIRECT_WRITE	BIT(4)
#define AP1302_ADV_IRQ_SYS_INTE_SIPS_FIFO_WRITE	BIT(3)
#define AP1302_ADV_IRQ_SYS_INTE_SPI		BIT(2)
#define AP1302_ADV_IRQ_SYS_INTE_GPIO_CNT	BIT(1)
#define AP1302_ADV_IRQ_SYS_INTE_GPIO_PIN	BIT(0)

/* Advanced Slave MIPI Registers */
#define AP1302_ADV_CAPTURE_A_FV_CNT		AP1302_REG_ADV_32BIT(0x00490040)

struct ap1302_device;

enum {
	AP1302_PAD_SINK_0,
	AP1302_PAD_SINK_1,
	AP1302_PAD_SOURCE,
	AP1302_PAD_MAX,
};

struct ap1302_format_info {
	unsigned int code;
	u16 out_fmt;
};

struct ap1302_format {
	struct v4l2_mbus_framefmt format;
	const struct ap1302_format_info *info;
};

struct ap1302_size {
	unsigned int width;
	unsigned int height;
};

struct ap1302_sensor_info {
	const char *model;
	const char *name;
	struct ap1302_size resolution;
	u32 format;
	const char * const *supplies;
};

struct ap1302_sensor {
	struct ap1302_device *ap1302;

	struct device_node *of_node;
	struct device *dev;
	unsigned int num_supplies;
	struct regulator_bulk_data *supplies;

	struct v4l2_subdev sd;
	struct media_pad pad;
};

static inline struct ap1302_sensor *to_ap1302_sensor(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ap1302_sensor, sd);
}

struct ap1302_device {
	struct device *dev;
	struct i2c_client *client;

	struct gpio_desc *reset_gpio;
	struct gpio_desc *standby_gpio;
	struct clk *clock;
	struct regmap *regmap16;
	struct regmap *regmap32;
	u32 reg_page;
	u16 crc;

	const struct firmware *fw;

	struct mutex lock;	/* Protects formats */

	struct v4l2_subdev sd;
	struct media_pad pads[AP1302_PAD_MAX];
	struct ap1302_format formats[AP1302_PAD_MAX];
	unsigned int width_factor;

	struct v4l2_ctrl_handler ctrls;

	const struct ap1302_sensor_info *sensor_info;
	struct ap1302_sensor sensors[2];
};

static inline struct ap1302_device *to_ap1302(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ap1302_device, sd);
}

#define AP1302_HEADER_VERSION(a, b, c) (((a) << 16) | ((b) << 8) | (c))

struct __attribute__((__packed__)) ap1302_firmware_header {
	u32 version;
	u32 header_size;
	u16 pll_init_size;
};

#define MAX_FW_LOAD_RETRIES 3

static const struct ap1302_format_info supported_video_formats[] = {
	{
		.code = MEDIA_BUS_FMT_UYVY8_1X16,
		.out_fmt = AP1302_PREVIEW_OUT_FMT_FT_YUV_JFIF
			 | AP1302_PREVIEW_OUT_FMT_FST_YUV_422,
	}, {
		.code = MEDIA_BUS_FMT_UYYVYY8_0_5X24,
		.out_fmt = AP1302_PREVIEW_OUT_FMT_FT_YUV_JFIF
			 | AP1302_PREVIEW_OUT_FMT_FST_YUV_420,
	},
};

/* -----------------------------------------------------------------------------
 * Sensor Info
 */

static const struct ap1302_sensor_info ap1302_sensor_info[] = {
	{
		.model = "onnn,ar0144",
		.name = "ar0144",
		.resolution = { 1280, 800 },
		.format = MEDIA_BUS_FMT_SGRBG12_1X12,
		.supplies = (const char * const[]) {
			"vaa",
			"vddio",
			"vdd",
			NULL,
		},
	}, {
		.model = "onnn,ar0330",
		.name = "ar0330",
		.resolution = { 2304, 1536 },
		.format = MEDIA_BUS_FMT_SGRBG12_1X12,
		.supplies = (const char * const[]) {
			"vddpll",
			"vaa",
			"vdd",
			"vddio",
			NULL,
		},
	}, {
		.model = "onnn,ar1335",
		.name = "ar1335",
		.resolution = { 4208, 3120 },
		.format = MEDIA_BUS_FMT_SGRBG10_1X10,
	},
};

static const struct ap1302_sensor_info ap1302_sensor_info_tpg = {
	.model = "",
	.name = "tpg",
	.resolution = { 1920, 1080 },
};

/* -----------------------------------------------------------------------------
 * Register Configuration
 */

static const struct regmap_config ap1302_reg16_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 2,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,
	.cache_type = REGCACHE_NONE,
};

static const struct regmap_config ap1302_reg32_config = {
	.reg_bits = 16,
	.val_bits = 32,
	.reg_stride = 4,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,
	.cache_type = REGCACHE_NONE,
};

static void ap1302_update_crc(struct ap1302_device *ap1302, u16 addr, u8 val)
{
	u16 crc = ap1302->crc;

	if (addr == AP1302_REG_ADDR(AP1302_SIP_CRC)) {
		crc = (crc & ~0xff00) | (val << 8);
	} else if (addr == AP1302_REG_ADDR(AP1302_SIP_CRC) + 1) {
		crc = (crc & ~0x00ff) | (val << 0);
	} else {
		/* Polynomial: 1 + x^5 + x^12 + x^16 */
		u32 v = (addr << 8) | val;
		unsigned int i;
		for (i = 0; i < 24; i++) {
			unsigned int bit = (v >> 23) & 0x1;
			v <<= 1;
			bit ^= (crc >> 15) & 0x1;
			crc = (crc << 1) ^ ((bit <<  0) |
			                    (bit <<  5) |
			                    (bit << 12));
		}
	}

	ap1302->crc = crc;
}

static int __ap1302_write(struct ap1302_device *ap1302, u32 reg, u32 val)
{
	unsigned int size = AP1302_REG_SIZE(reg);
	u16 addr = AP1302_REG_ADDR(reg);
	int ret;

	switch (size) {
	case 2:
		ret = regmap_write(ap1302->regmap16, addr, val);
		ap1302_update_crc(ap1302, addr+0, (val >>  8) & 0xff);
		ap1302_update_crc(ap1302, addr+1, (val >>  0) & 0xff);
		break;
	case 4:
		ret = regmap_write(ap1302->regmap32, addr, val);
		ap1302_update_crc(ap1302, addr+0, (val >> 24) & 0xff);
		ap1302_update_crc(ap1302, addr+1, (val >> 16) & 0xff);
		ap1302_update_crc(ap1302, addr+2, (val >>  8) & 0xff);
		ap1302_update_crc(ap1302, addr+3, (val >>  0) & 0xff);
		break;
	default:
		return -EINVAL;
	}

	if (ret) {
		dev_err(ap1302->dev, "%s: register 0x%04x %s failed: %d\n",
			__func__, addr, "write", ret);
		return ret;
	}

	return 0;
}

static int ap1302_write(struct ap1302_device *ap1302, u32 reg, u32 val)
{
	int ret;

	if (AP1302_REG_ADV(reg)) {
		u32 addr = AP1302_REG_ADDR(reg);
		u32 page = AP1302_REG_PAGE(addr);
		if (ap1302->reg_page != page) {
			ret = ap1302_write(ap1302, AP1302_ADVANCED_BASE, page);
			if (ret < 0)
				return ret;

			ap1302->reg_page = page;
		}

		reg &= ~AP1302_REG_ADV_MASK;
		reg &= ~AP1302_REG_PAGE_MASK;
		reg += AP1302_REG_ADV_START;
	}

	return __ap1302_write(ap1302, reg, val);
}

static int __ap1302_read(struct ap1302_device *ap1302, u32 reg, u32 *val)
{
	unsigned int size = AP1302_REG_SIZE(reg);
	u16 addr = AP1302_REG_ADDR(reg);
	int ret;

	switch (size) {
	case 2:
		ret = regmap_read(ap1302->regmap16, addr, val);
		break;
	case 4:
		ret = regmap_read(ap1302->regmap32, addr, val);
		break;
	default:
		return -EINVAL;
	}

	if (ret) {
		dev_err(ap1302->dev, "%s: register 0x%04x %s failed: %d\n",
			__func__, addr, "read", ret);
		return ret;
	}

	dev_dbg(ap1302->dev, "%s: R0x%04x = 0x%0*x\n", __func__,
		addr, size * 2, *val);

	return 0;
}

static int ap1302_read(struct ap1302_device *ap1302, u32 reg, u32 *val)
{
	int ret;

	if (AP1302_REG_ADV(reg)) {
		u32 addr = AP1302_REG_ADDR(reg);
		u32 page = AP1302_REG_PAGE(addr);
		if (ap1302->reg_page != page) {
			ret = ap1302_write(ap1302, AP1302_ADVANCED_BASE, page);
			if (ret < 0)
				return ret;

			ap1302->reg_page = page;
		}

		reg &= ~AP1302_REG_ADV_MASK;
		reg &= ~AP1302_REG_PAGE_MASK;
		reg += AP1302_REG_ADV_START;
	}

	return __ap1302_read(ap1302, reg, val);
}

static int ap1302_check_crc(struct ap1302_device *ap1302)
{
	int ret;
	u32 val;
	ret = ap1302_read(ap1302, AP1302_SIP_CRC, &val);
	if (ret)
		return ret;

	if (val != ap1302->crc) {
		dev_warn(ap1302->dev,
			 "CRC mismatch: expected 0x%04x, got 0x%04x\n",
			 ap1302->crc, val);
		ap1302->crc = val;
		return -EAGAIN;
	}

	return 0;
}

/* -----------------------------------------------------------------------------
 * Power Handling
 */

static int ap1302_power_on_sensors(struct ap1302_device *ap1302)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(ap1302->sensors); ++i) {
		struct ap1302_sensor *sensor = &ap1302->sensors[i];

		ret = regulator_bulk_enable(sensor->num_supplies,
					    sensor->supplies);
		if (ret) {
			dev_err(ap1302->dev,
				"Failed to enable supplies for sensor %u\n", i);
			goto error;
		}
	}

	return 0;

error:
	for (; i > 0; --i) {
		struct ap1302_sensor *sensor = &ap1302->sensors[i - 1];

		regulator_bulk_disable(sensor->num_supplies, sensor->supplies);
	}

	return ret;
}

static void ap1302_power_off_sensors(struct ap1302_device *ap1302)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ap1302->sensors); ++i) {
		struct ap1302_sensor *sensor = &ap1302->sensors[i];

		regulator_bulk_disable(sensor->num_supplies, sensor->supplies);
	}
}

static int ap1302_power_on(struct ap1302_device *ap1302)
{
	int ret;

	/* 0. RESET was asserted when getting the GPIO. */

	/* 1. Assert STANDBY. */
	if (ap1302->standby_gpio) {
		gpiod_set_value(ap1302->standby_gpio, 1);
		usleep_range(200, 1000);
	}

	/* 2. Power up the regulators. To be implemented. */

	/* 3. De-assert STANDBY. */
	if (ap1302->standby_gpio) {
		gpiod_set_value(ap1302->standby_gpio, 0);
		usleep_range(200, 1000);
	}

	/* 4. Turn the clock on. */
	ret = clk_prepare_enable(ap1302->clock);
	if (ret < 0) {
		dev_err(ap1302->dev, "Failed to enable clock: %d\n", ret);
		return ret;
	}

	/* 5. De-assert RESET. */
	gpiod_set_value(ap1302->reset_gpio, 0);

	/*
	 * 6. Wait for the AP1302 to initialize. The datasheet doesn't specify
	 * how long this takes.
	 */
	usleep_range(10000, 11000);

	/* SIPS_CRC value is 0xffff after reset. */
	ap1302->crc = 0xffff;

	return 0;
}

static void ap1302_power_off(struct ap1302_device *ap1302)
{
	/* 1. Assert RESET. */
	gpiod_set_value(ap1302->reset_gpio, 1);

	/* 2. Turn the clock off. */
	clk_disable_unprepare(ap1302->clock);

	/* 3. Assert STANDBY. */
	if (ap1302->standby_gpio) {
		gpiod_set_value(ap1302->standby_gpio, 1);
		usleep_range(200, 1000);
	}

	/* 4. Power down the regulators. To be implemented. */

	/* 5. De-assert STANDBY. */
	if (ap1302->standby_gpio) {
		usleep_range(200, 1000);
		gpiod_set_value(ap1302->standby_gpio, 0);
	}
}

/* -----------------------------------------------------------------------------
 * Hardware Configuration
 */

static int ap1302_dump_console(struct ap1302_device *ap1302)
{
	u8 *buffer;
	u8 *endp;
	u8 *p;
	int ret;

	buffer = kmalloc(AP1302_CON_BUF_SIZE + 1, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	ret = regmap_raw_read(ap1302->regmap16, AP1302_CON_BUF(0), buffer,
			      AP1302_CON_BUF_SIZE);
	if (ret < 0) {
		dev_err(ap1302->dev, "Failed to read console buffer: %d\n",
			ret);
		goto done;
	}

	print_hex_dump(KERN_INFO, "console ", DUMP_PREFIX_OFFSET, 16, 1, buffer,
		       AP1302_CON_BUF_SIZE, true);

	buffer[AP1302_CON_BUF_SIZE] = '\0';

	for (p = buffer; p < buffer + AP1302_CON_BUF_SIZE && *p; p = endp + 1) {
		endp = strchrnul(p, '\n');
		*endp = '\0';

		printk(KERN_INFO "console %s\n", p);
	}

	ret = 0;

done:
	kfree(buffer);
	return ret;
}

static int ap1302_configure(struct ap1302_device *ap1302)
{
	const struct ap1302_format *format = &ap1302->formats[AP1302_PAD_SOURCE];
	int ret;

	ret = ap1302_write(ap1302, AP1302_PREVIEW_WIDTH,
			       format->format.width / ap1302->width_factor);
	if (ret < 0)
		return ret;

	ret = ap1302_write(ap1302, AP1302_PREVIEW_HEIGHT,
			       format->format.height);
	if (ret < 0)
		return ret;

	ret = ap1302_write(ap1302, AP1302_PREVIEW_OUT_FMT,
			       format->info->out_fmt);
	if (ret < 0)
		return ret;

	__v4l2_ctrl_handler_setup(&ap1302->ctrls);

	return 0;
}

static int ap1302_stall(struct ap1302_device *ap1302, bool stall)
{
	int ret;

	if (stall) {
		ret = ap1302_write(ap1302, AP1302_SYS_START,
				   AP1302_SYS_START_PLL_LOCK |
				   AP1302_SYS_START_STALL_MODE_DISABLED);
		if (ret < 0)
			return ret;

		ret = ap1302_write(ap1302, AP1302_SYS_START,
				   AP1302_SYS_START_PLL_LOCK |
				   AP1302_SYS_START_STALL_EN |
				   AP1302_SYS_START_STALL_MODE_DISABLED);
		if (ret < 0)
			return ret;

		msleep(200);

		ret = ap1302_write(ap1302, AP1302_ADV_IRQ_SYS_INTE,
				   AP1302_ADV_IRQ_SYS_INTE_SIPM |
				   AP1302_ADV_IRQ_SYS_INTE_SIPS_FIFO_WRITE);
		if (ret < 0)
			return ret;

		return 0;
	} else {
		return ap1302_write(ap1302, AP1302_SYS_START,
				    AP1302_SYS_START_PLL_LOCK |
				    AP1302_SYS_START_STALL_STATUS |
				    AP1302_SYS_START_STALL_EN |
				    AP1302_SYS_START_STALL_MODE_DISABLED);
	}
}

/* -----------------------------------------------------------------------------
  * V4L2 Controls
 */

static u16 ap1302_wb_values[] = {
	AP1302_AWB_CTRL_MODE_OFF,	/* V4L2_WHITE_BALANCE_MANUAL */
	AP1302_AWB_CTRL_MODE_AUTO,	/* V4L2_WHITE_BALANCE_AUTO */
	AP1302_AWB_CTRL_MODE_A,		/* V4L2_WHITE_BALANCE_INCANDESCENT */
	AP1302_AWB_CTRL_MODE_D50,	/* V4L2_WHITE_BALANCE_FLUORESCENT */
	AP1302_AWB_CTRL_MODE_D65,	/* V4L2_WHITE_BALANCE_FLUORESCENT_H */
	AP1302_AWB_CTRL_MODE_HORIZON,	/* V4L2_WHITE_BALANCE_HORIZON */
	AP1302_AWB_CTRL_MODE_D65,	/* V4L2_WHITE_BALANCE_DAYLIGHT */
	AP1302_AWB_CTRL_MODE_AUTO,	/* V4L2_WHITE_BALANCE_FLASH */
	AP1302_AWB_CTRL_MODE_D75,	/* V4L2_WHITE_BALANCE_CLOUDY */
	AP1302_AWB_CTRL_MODE_D75,	/* V4L2_WHITE_BALANCE_SHADE */
};

static int ap1302_set_wb_mode(struct ap1302_device *ap1302, s32 mode)
{
	u32 val;
	int ret;

	ret = ap1302_read(ap1302, AP1302_AWB_CTRL, &val);
	if (ret)
		return ret;

	val &= ~AP1302_AWB_CTRL_MODE_MASK;
	val |= ap1302_wb_values[mode];

	if (mode == V4L2_WHITE_BALANCE_FLASH)
		val |= AP1302_AWB_CTRL_FLASH;
	else
		val &= ~AP1302_AWB_CTRL_FLASH;

	return ap1302_write(ap1302, AP1302_AWB_CTRL, val);
}

static int ap1302_set_zoom(struct ap1302_device *ap1302, s32 val)
{
	return ap1302_write(ap1302, AP1302_DZ_TGT_FCT, val);
}

static u16 ap1302_sfx_values[] = {
	AP1302_SFX_MODE_SFX_NORMAL,	/* V4L2_COLORFX_NONE */
	AP1302_SFX_MODE_SFX_BW,		/* V4L2_COLORFX_BW */
	AP1302_SFX_MODE_SFX_SEPIA1,	/* V4L2_COLORFX_SEPIA */
	AP1302_SFX_MODE_SFX_NEGATIVE,	/* V4L2_COLORFX_NEGATIVE */
	AP1302_SFX_MODE_SFX_EMBOSS,	/* V4L2_COLORFX_EMBOSS */
	AP1302_SFX_MODE_SFX_SKETCH,	/* V4L2_COLORFX_SKETCH */
	AP1302_SFX_MODE_SFX_BLUISH,	/* V4L2_COLORFX_SKY_BLUE */
	AP1302_SFX_MODE_SFX_GREENISH,	/* V4L2_COLORFX_GRASS_GREEN */
	AP1302_SFX_MODE_SFX_REDISH,	/* V4L2_COLORFX_SKIN_WHITEN */
	AP1302_SFX_MODE_SFX_NORMAL,	/* V4L2_COLORFX_VIVID */
	AP1302_SFX_MODE_SFX_NORMAL,	/* V4L2_COLORFX_AQUA */
	AP1302_SFX_MODE_SFX_NORMAL,	/* V4L2_COLORFX_ART_FREEZE */
	AP1302_SFX_MODE_SFX_NORMAL,	/* V4L2_COLORFX_SILHOUETTE */
	AP1302_SFX_MODE_SFX_SOLARIZE,	/* V4L2_COLORFX_SOLARIZATION */
	AP1302_SFX_MODE_SFX_ANTIQUE,	/* V4L2_COLORFX_ANTIQUE */
	AP1302_SFX_MODE_SFX_NORMAL,	/* V4L2_COLORFX_SET_CBCR */
};

static int ap1302_set_special_effect(struct ap1302_device *ap1302, s32 val)
{
	return ap1302_write(ap1302, AP1302_SFX_MODE, ap1302_sfx_values[val]);
}

static u16 ap1302_scene_mode_values[] = {
	AP1302_SCENE_CTRL_MODE_NORMAL,		/* V4L2_SCENE_MODE_NONE */
	AP1302_SCENE_CTRL_MODE_BACKLIGHT,	/* V4L2_SCENE_MODE_BACKLIGHT */
	AP1302_SCENE_CTRL_MODE_BEACH,		/* V4L2_SCENE_MODE_BEACH_SNOW */
	AP1302_SCENE_CTRL_MODE_TWILIGHT,	/* V4L2_SCENE_MODE_CANDLE_LIGHT */
	AP1302_SCENE_CTRL_MODE_NORMAL,		/* V4L2_SCENE_MODE_DAWN_DUSK */
	AP1302_SCENE_CTRL_MODE_NORMAL,		/* V4L2_SCENE_MODE_FALL_COLORS */
	AP1302_SCENE_CTRL_MODE_FIREWORKS,	/* V4L2_SCENE_MODE_FIREWORKS */
	AP1302_SCENE_CTRL_MODE_LANDSCAPE,	/* V4L2_SCENE_MODE_LANDSCAPE */
	AP1302_SCENE_CTRL_MODE_NIGHT,		/* V4L2_SCENE_MODE_NIGHT */
	AP1302_SCENE_CTRL_MODE_PARTY,		/* V4L2_SCENE_MODE_PARTY_INDOOR */
	AP1302_SCENE_CTRL_MODE_PORTRAIT,	/* V4L2_SCENE_MODE_PORTRAIT */
	AP1302_SCENE_CTRL_MODE_SPORT,		/* V4L2_SCENE_MODE_SPORTS */
	AP1302_SCENE_CTRL_MODE_SUNSET,		/* V4L2_SCENE_MODE_SUNSET */
	AP1302_SCENE_CTRL_MODE_DOCUMENT,	/* V4L2_SCENE_MODE_TEXT */
};

static int ap1302_set_scene_mode(struct ap1302_device *ap1302, s32 val)
{
	return ap1302_write(ap1302, AP1302_SCENE_CTRL,
			    ap1302_scene_mode_values[val]);
}

static const u16 ap1302_flicker_values[] = {
	AP1302_FLICK_CTRL_MODE_DISABLED,
	AP1302_FLICK_CTRL_FREQ(50) | AP1302_FLICK_CTRL_MODE_MANUAL,
	AP1302_FLICK_CTRL_FREQ(60) | AP1302_FLICK_CTRL_MODE_MANUAL,
	AP1302_FLICK_CTRL_MODE_AUTO,
};

static int ap1302_set_flicker_freq(struct ap1302_device *ap1302, s32 val)
{
	return ap1302_write(ap1302, AP1302_FLICK_CTRL,
			    ap1302_flicker_values[val]);
}

static int ap1302_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ap1302_device *ap1302 =
		container_of(ctrl->handler, struct ap1302_device, ctrls);

	switch (ctrl->id) {
	case V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE:
		return ap1302_set_wb_mode(ap1302, ctrl->val);

	case V4L2_CID_ZOOM_ABSOLUTE:
		return ap1302_set_zoom(ap1302, ctrl->val);

	case V4L2_CID_COLORFX:
		return ap1302_set_special_effect(ap1302, ctrl->val);

	case V4L2_CID_SCENE_MODE:
		return ap1302_set_scene_mode(ap1302, ctrl->val);

	case V4L2_CID_POWER_LINE_FREQUENCY:
		return ap1302_set_flicker_freq(ap1302, ctrl->val);

	default:
		return -EINVAL;
	}
}

static const struct v4l2_ctrl_ops ap1302_ctrl_ops = {
	.s_ctrl = ap1302_s_ctrl,
};

static const struct v4l2_ctrl_config ap1302_ctrls[] = {
	{
		.ops = &ap1302_ctrl_ops,
		.id = V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE,
		.min = 0,
		.max = 9,
		.def = 0,
	}, {
		.ops = &ap1302_ctrl_ops,
		.id = V4L2_CID_ZOOM_ABSOLUTE,
		.min = 0x0100,
		.max = 0x1000,
		.step = 1,
		.def = 0x0100,
	}, {
		.ops = &ap1302_ctrl_ops,
		.id = V4L2_CID_COLORFX,
		.min = 0,
		.max = 15,
		.def = 0,
		.menu_skip_mask = BIT(15) | BIT(12) | BIT(11) | BIT(10) | BIT(9),
	}, {
		.ops = &ap1302_ctrl_ops,
		.id = V4L2_CID_SCENE_MODE,
		.min = 0,
		.max = 13,
		.def = 0,
		.menu_skip_mask = BIT(5) | BIT(4),
	}, {
		.ops = &ap1302_ctrl_ops,
		.id = V4L2_CID_POWER_LINE_FREQUENCY,
		.min = 0,
		.max = 3,
		.def = 3,
	},
};

static int ap1302_ctrls_init(struct ap1302_device *ap1302)
{
	unsigned int i;
	int ret;

	ret = v4l2_ctrl_handler_init(&ap1302->ctrls, ARRAY_SIZE(ap1302_ctrls));
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(ap1302_ctrls); i++)
		v4l2_ctrl_new_custom(&ap1302->ctrls, &ap1302_ctrls[i], NULL);

	if (ap1302->ctrls.error) {
		ret = ap1302->ctrls.error;
		v4l2_ctrl_handler_free(&ap1302->ctrls);
		return ret;
	}

	/* Use same lock for controls as for everything else. */
	ap1302->ctrls.lock = &ap1302->lock;
	ap1302->sd.ctrl_handler = &ap1302->ctrls;

	return 0;
}

static void ap1302_ctrls_cleanup(struct ap1302_device *ap1302)
{
	v4l2_ctrl_handler_free(&ap1302->ctrls);
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdev Operations
 */

static struct v4l2_mbus_framefmt *
ap1302_get_pad_format(struct ap1302_device *ap1302,
		      struct v4l2_subdev_pad_config *cfg,
		      unsigned int pad, u32 which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_format(&ap1302->sd, cfg, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &ap1302->formats[pad].format;
	default:
		return NULL;
	}
}

static int ap1302_init_cfg(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg)
{
	u32 which = cfg ? V4L2_SUBDEV_FORMAT_TRY : V4L2_SUBDEV_FORMAT_ACTIVE;
	struct ap1302_device *ap1302 = to_ap1302(sd);
	const struct ap1302_sensor_info *info = ap1302->sensor_info;
	unsigned int pad;

	for (pad = 0; pad < ARRAY_SIZE(ap1302->formats); ++pad) {
		struct v4l2_mbus_framefmt *format =
			ap1302_get_pad_format(ap1302, cfg, pad, which);

		format->width = info->resolution.width;
		format->height = info->resolution.height;

		/*
		 * The source pad combines images side by side in multi-sensor
		 * setup.
		 */
		if (pad == AP1302_PAD_SOURCE)
			format->width *= ap1302->width_factor;

		format->field = V4L2_FIELD_NONE;
		format->code = ap1302->formats[pad].info->code;
		format->colorspace = V4L2_COLORSPACE_SRGB;
	}

	return 0;
}

static int ap1302_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct ap1302_device *ap1302 = to_ap1302(sd);

	if (code->pad != AP1302_PAD_SOURCE) {
		/*
		 * On the sink pads, only the format produced by the sensor is
		 * supported.
		 */
		if (code->index)
			return -EINVAL;

		code->code = ap1302->sensor_info->format;
	} else {
		/* On the source pad, multiple formats are supported. */
		if (code->index >= ARRAY_SIZE(supported_video_formats))
			return -EINVAL;

		code->code = supported_video_formats[code->index].code;
	}

	return 0;
}

static int ap1302_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct ap1302_device *ap1302 = to_ap1302(sd);
	unsigned int i;

	if (fse->index)
		return -EINVAL;

	if (fse->pad != AP1302_PAD_SOURCE) {
		/*
		 * On the sink pads, only the size produced by the sensor is
		 * supported.
		 */
		if (fse->code != ap1302->sensor_info->format)
			return -EINVAL;

		fse->min_width = ap1302->sensor_info->resolution.width;
		fse->min_height = ap1302->sensor_info->resolution.height;
		fse->max_width = ap1302->sensor_info->resolution.width;
		fse->max_height = ap1302->sensor_info->resolution.height;
	} else {
		/*
		 * On the source pad, the AP1302 can freely scale within the
		 * scaler's limits.
		 */
		for (i = 0; i < ARRAY_SIZE(supported_video_formats); i++) {
			if (supported_video_formats[i].code == fse->code)
				break;
		}

		if (i >= ARRAY_SIZE(supported_video_formats))
			return -EINVAL;

		fse->min_width = AP1302_MIN_WIDTH * ap1302->width_factor;
		fse->min_height = AP1302_MIN_HEIGHT;
		fse->max_width = AP1302_MAX_WIDTH;
		fse->max_height = AP1302_MAX_HEIGHT;
	}

	return 0;
}

static int ap1302_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct ap1302_device *ap1302 = to_ap1302(sd);
	const struct v4l2_mbus_framefmt *format;

	format = ap1302_get_pad_format(ap1302, cfg, fmt->pad, fmt->which);

	mutex_lock(&ap1302->lock);
	fmt->format = *format;
	mutex_unlock(&ap1302->lock);

	return 0;
}

static int ap1302_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct ap1302_device *ap1302 = to_ap1302(sd);
	const struct ap1302_format_info *info = NULL;
	struct v4l2_mbus_framefmt *format;
	unsigned int i;

	/* Formats on the sink pads can't be changed. */
	if (fmt->pad != AP1302_PAD_SOURCE)
		return ap1302_get_fmt(sd, cfg, fmt);

	format = ap1302_get_pad_format(ap1302, cfg, fmt->pad, fmt->which);

	/* Validate the media bus code, default to the first supported value. */
	for (i = 0; i < ARRAY_SIZE(supported_video_formats); i++) {
		if (supported_video_formats[i].code == fmt->format.code) {
			info = &supported_video_formats[i];
			break;
		}
	}

	if (!info)
		info = &supported_video_formats[0];

	/*
	 * Clamp the size. The width must be a multiple of 4 (or 8 in the
	 * dual-sensor case) and the height a multiple of 2.
	 */
	fmt->format.width = clamp(ALIGN_DOWN(fmt->format.width,
					     4 * ap1302->width_factor),
				  AP1302_MIN_WIDTH * ap1302->width_factor,
				  AP1302_MAX_WIDTH);
	fmt->format.height = clamp(ALIGN_DOWN(fmt->format.height, 2),
				   AP1302_MIN_HEIGHT, AP1302_MAX_HEIGHT);

	mutex_lock(&ap1302->lock);

	format->width = fmt->format.width;
	format->height = fmt->format.height;
	format->code = info->code;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		ap1302->formats[fmt->pad].info = info;

	mutex_unlock(&ap1302->lock);

	fmt->format = *format;

	return 0;
}

static int ap1302_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct ap1302_device *ap1302 = to_ap1302(sd);
	int ret;

	mutex_lock(&ap1302->lock);

	if (enable) {
		ret = ap1302_configure(ap1302);
		if (ret < 0)
			goto done;

		ret = ap1302_stall(ap1302, false);
	} else {
		ret = ap1302_stall(ap1302, true);
	}

done:
	mutex_unlock(&ap1302->lock);

	if (ret < 0)
		dev_err(ap1302->dev, "Failed to %s stream: %d\n",
			enable ? "start" : "stop", ret);

	return ret;
}

static const char * const ap1302_warnings[] = {
	"HINF_BANDWIDTH",
	"FLICKER_DETECTION",
	"FACED_NE",
	"SMILED_NE",
	"HINF_OVERRUN",
	NULL,
	"FRAME_TOO_SMALL",
	"MISSING_PHASES",
	"SPOOF_UNDERRUN",
	"JPEG_NOLAST",
	"NO_IN_FREQ_SPEC",
	"SINF0",
	"SINF1",
	"CAPTURE0",
	"CAPTURE1",
	"ISR_UNHANDLED",
	"INTERLEAVE_SPOOF",
	"INTERLEAVE_BUF",
	"COORD_OUT_OF_RANGE",
	"ICP_CLOCKING",
	"SENSOR_CLOCKING",
	"SENSOR_NO_IHDR",
	"DIVIDE_BY_ZERO",
	"INT0_UNDERRUN",
	"INT1_UNDERRUN",
	"SCRATCHPAD_TOO_BIG",
	"OTP_RECORD_READ",
	"NO_LSC_IN_OTP",
	"GPIO_INT_LOST",
	"NO_PDAF_DATA",
	"FAR_PDAF_ACCESS_SKIP",
	"PDAF_ERROR",
	"ATM_TVI_BOUNDS",
	"SIPM_0_RTY",
	"SIPM_1_TRY",
	"SIPM_0_NO_ACK",
	"SIPM_1_NO_ACK",
	"SMILE_DIS",
	"DVS_DIS",
	"TEST_DIS",
	"SENSOR_LV2LV",
	"SENSOR_FV2FV",
	"FRAME_LOST",
};

static int ap1302_log_status(struct v4l2_subdev *sd)
{
	struct ap1302_device *ap1302 = to_ap1302(sd);
	u16 frame_count_icp;
	u16 frame_count_brac;
	u16 frame_count_hinf;
	u32 warning[4];
	u32 error[3];
	unsigned int i;
	u32 value;
	int ret;

	/* Dump the console buffer. */
	ret = ap1302_dump_console(ap1302);
	if (ret < 0)
		return ret;

	/* Print errors. */
	ret = ap1302_read(ap1302, AP1302_ERROR, &error[0]);
	if (ret < 0)
		return ret;

	ret = ap1302_read(ap1302, AP1302_ERR_FILE, &error[1]);
	if (ret < 0)
		return ret;

	ret = ap1302_read(ap1302, AP1302_ERR_LINE, &error[2]);
	if (ret < 0)
		return ret;

	dev_info(ap1302->dev, "ERROR: 0x%04x (file 0x%08x:%u)\n",
		 error[0], error[1], error[2]);

	ret = ap1302_read(ap1302, AP1302_SIPM_ERR_0, &error[0]);
	if (ret < 0)
		return ret;

	ret = ap1302_read(ap1302, AP1302_SIPM_ERR_1, &error[1]);
	if (ret < 0)
		return ret;

	dev_info(ap1302->dev, "SIPM_ERR [0] 0x%04x [1] 0x%04x\n",
		 error[0], error[1]);

	/* Print warnings. */
	for (i = 0; i < ARRAY_SIZE(warning); ++i) {
		ret = ap1302_read(ap1302, AP1302_WARNING(i), &warning[i]);
		if (ret < 0)
			return ret;
	}

	dev_info(ap1302->dev,
		 "WARNING [0] 0x%04x [1] 0x%04x [2] 0x%04x [3] 0x%04x\n",
		 warning[0], warning[1], warning[2], warning[3]);

	for (i = 0; i < ARRAY_SIZE(ap1302_warnings); ++i) {
		if ((warning[i / 16] & BIT(i % 16)) &&
		    ap1302_warnings[i])
			dev_info(ap1302->dev, "- WARN_%s\n",
				 ap1302_warnings[i]);
	}

	/* Print the frame counter. */
	ret = ap1302_read(ap1302, AP1302_FRAME_CNT, &value);
	if (ret < 0)
		return ret;

	frame_count_hinf = value >> 8;
	frame_count_brac = value & 0xff;

	ret = ap1302_read(ap1302, AP1302_ADV_CAPTURE_A_FV_CNT, &value);
	if (ret < 0)
		return ret;

	frame_count_icp = value & 0xffff;

	dev_info(ap1302->dev, "Frame counters: ICP %u, HINF %u, BRAC %u\n",
		 frame_count_icp, frame_count_hinf, frame_count_brac);

	return 0;
}

static int ap1302_subdev_registered(struct v4l2_subdev *sd)
{
	struct ap1302_device *ap1302 = to_ap1302(sd);
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(ap1302->sensors); ++i) {
		struct ap1302_sensor *sensor = &ap1302->sensors[i];

		if (!sensor->dev)
			continue;

		dev_dbg(ap1302->dev, "registering sensor %u\n", i);

		ret = v4l2_device_register_subdev(sd->v4l2_dev, &sensor->sd);
		if (ret)
			return ret;

		ret = media_create_pad_link(&sensor->sd.entity, 0,
					    &sd->entity, i,
					    MEDIA_LNK_FL_IMMUTABLE |
					    MEDIA_LNK_FL_ENABLED);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct media_entity_operations ap1302_media_ops = {
	.link_validate = v4l2_subdev_link_validate
};

static const struct v4l2_subdev_pad_ops ap1302_pad_ops = {
	.init_cfg = ap1302_init_cfg,
	.enum_mbus_code = ap1302_enum_mbus_code,
	.enum_frame_size = ap1302_enum_frame_size,
	.get_fmt = ap1302_get_fmt,
	.set_fmt = ap1302_set_fmt,
};

static const struct v4l2_subdev_video_ops ap1302_video_ops = {
	.s_stream = ap1302_s_stream,
};

static const struct v4l2_subdev_core_ops ap1302_core_ops = {
	.log_status = ap1302_log_status,
};

static const struct v4l2_subdev_ops ap1302_subdev_ops = {
	.core = &ap1302_core_ops,
	.video = &ap1302_video_ops,
	.pad = &ap1302_pad_ops,
};

static const struct v4l2_subdev_internal_ops ap1302_subdev_internal_ops = {
	.registered = ap1302_subdev_registered,
};

/* -----------------------------------------------------------------------------
 * Sensor
 */

static int ap1302_sensor_enum_mbus_code(struct v4l2_subdev *sd,
					struct v4l2_subdev_pad_config *cfg,
					struct v4l2_subdev_mbus_code_enum *code)
{
	struct ap1302_sensor *sensor = to_ap1302_sensor(sd);
	const struct ap1302_sensor_info *info = sensor->ap1302->sensor_info;

	if (code->index)
		return -EINVAL;

	code->code = info->format;
	return 0;
}

static int ap1302_sensor_enum_frame_size(struct v4l2_subdev *sd,
					 struct v4l2_subdev_pad_config *cfg,
					 struct v4l2_subdev_frame_size_enum *fse)
{
	struct ap1302_sensor *sensor = to_ap1302_sensor(sd);
	const struct ap1302_sensor_info *info = sensor->ap1302->sensor_info;

	if (fse->index)
		return -EINVAL;

	if (fse->code != info->format)
		return -EINVAL;

	fse->min_width = info->resolution.width;
	fse->min_height = info->resolution.height;
	fse->max_width = info->resolution.width;
	fse->max_height = info->resolution.height;

	return 0;
}

static int ap1302_sensor_get_fmt(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_format *fmt)
{
	struct ap1302_sensor *sensor = to_ap1302_sensor(sd);
	const struct ap1302_sensor_info *info = sensor->ap1302->sensor_info;

	memset(&fmt->format, 0, sizeof(fmt->format));

	fmt->format.width = info->resolution.width;
	fmt->format.height = info->resolution.height;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.code = info->format;
	fmt->format.colorspace = V4L2_COLORSPACE_SRGB;

	return 0;
}

static const struct v4l2_subdev_pad_ops ap1302_sensor_pad_ops = {
	.enum_mbus_code = ap1302_sensor_enum_mbus_code,
	.enum_frame_size = ap1302_sensor_enum_frame_size,
	.get_fmt = ap1302_sensor_get_fmt,
	.set_fmt = ap1302_sensor_get_fmt,
};

static const struct v4l2_subdev_ops ap1302_sensor_subdev_ops = {
	.pad = &ap1302_sensor_pad_ops,
};

static int ap1302_sensor_parse_of(struct ap1302_device *ap1302,
				  struct device_node *node)
{
	struct ap1302_sensor *sensor;
	u32 reg;
	int ret;

	/* Retrieve the sensor index from the reg property. */
	ret = of_property_read_u32(node, "reg", &reg);
	if (ret < 0) {
		dev_warn(ap1302->dev,
			 "'reg' property missing in sensor node\n");
		return -EINVAL;
	}

	if (reg >= ARRAY_SIZE(ap1302->sensors)) {
		dev_warn(ap1302->dev, "Out-of-bounds 'reg' value %u\n",
			 reg);
		return -EINVAL;
	}

	sensor = &ap1302->sensors[reg];
	if (sensor->ap1302) {
		dev_warn(ap1302->dev, "Duplicate entry for sensor %u\n", reg);
		return -EINVAL;
	}

	sensor->ap1302 = ap1302;
	sensor->of_node = of_node_get(node);

	return 0;
}

static void ap1302_sensor_dev_release(struct device *dev)
{
	of_node_put(dev->of_node);
	kfree(dev);
}

static int ap1302_sensor_init(struct ap1302_sensor *sensor, unsigned int index)
{
	struct ap1302_device *ap1302 = sensor->ap1302;
	struct v4l2_subdev *sd = &sensor->sd;
	unsigned int i;
	int ret;

	/*
	 * Register a device for the sensor, to support usage of the regulator
	 * API.
	 */
	sensor->dev = kzalloc(sizeof(*sensor->dev), GFP_KERNEL);
	if (!sensor->dev)
		return -ENOMEM;

	sensor->dev->parent = ap1302->dev;
	sensor->dev->of_node = of_node_get(sensor->of_node);
	sensor->dev->release = &ap1302_sensor_dev_release;
	dev_set_name(sensor->dev, "%s-%s.%u", dev_name(ap1302->dev),
		     ap1302->sensor_info->name, index);

	ret = device_register(sensor->dev);
	if (ret < 0) {
		dev_err(ap1302->dev,
			"Failed to register device for sensor %u\n", index);
		goto error;
	}

	/* Retrieve the power supplies for the sensor, if any. */
	if (ap1302->sensor_info->supplies) {
		const char * const *supplies = ap1302->sensor_info->supplies;
		unsigned int num_supplies;

		for (num_supplies = 0; supplies[num_supplies]; ++num_supplies)
			;

		sensor->supplies = devm_kcalloc(ap1302->dev, num_supplies,
						sizeof(*sensor->supplies),
						GFP_KERNEL);
		if (!sensor->supplies) {
			ret = -ENOMEM;
			goto error;
		}

		for (i = 0; i < num_supplies; ++i)
			sensor->supplies[i].supply = supplies[i];

		ret = regulator_bulk_get(sensor->dev, num_supplies,
					 sensor->supplies);
		if (ret < 0) {
			dev_err(ap1302->dev,
				"Failed to get supplies for sensor %u\n", index);
			goto error;
		}

		sensor->num_supplies = i;
	}

	sd->dev = sensor->dev;
	v4l2_subdev_init(sd, &ap1302_sensor_subdev_ops);

	snprintf(sd->name, sizeof(sd->name), "%s %u",
		 ap1302->sensor_info->name, index);

	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sd->entity, 1, &sensor->pad);
	if (ret < 0) {
		dev_err(ap1302->dev,
			"failed to initialize media entity for sensor %u: %d\n",
			index, ret);
		goto error;
	}

	return 0;

error:
	put_device(sensor->dev);
	return ret;
}

static void ap1302_sensor_cleanup(struct ap1302_sensor *sensor)
{
	media_entity_cleanup(&sensor->sd.entity);

	if (sensor->num_supplies)
		regulator_bulk_free(sensor->num_supplies, sensor->supplies);

	put_device(sensor->dev);
	of_node_put(sensor->of_node);
}

/* -----------------------------------------------------------------------------
 * Boot & Firmware Handling
 */

static int ap1302_request_firmware(struct ap1302_device *ap1302)
{
	static const char * const suffixes[] = {
		"",
		"_single",
		"_dual",
	};

	unsigned int num_sensors;
	unsigned int i;
	char name[64];
	int ret;

	for (i = 0, num_sensors = 0; i < ARRAY_SIZE(ap1302->sensors); ++i) {
		if (ap1302->sensors[i].dev)
			num_sensors++;
	}

	ret = snprintf(name, sizeof(name), "ap1302_%s%s_fw.bin",
		       ap1302->sensor_info->name, suffixes[num_sensors]);
	if (ret >= sizeof(name)) {
		dev_err(ap1302->dev, "Firmware name too long\n");
		return -EINVAL;
	}

	dev_dbg(ap1302->dev, "Requesting firmware %s\n", name);

	ret = request_firmware(&ap1302->fw, name, ap1302->dev);
	if (ret) {
		dev_err(ap1302->dev, "Failed to request firmware: %d\n", ret);
		return ret;
	}

	return 0;
}

/*
 * ap1302_write_fw_window() - Write a piece of firmware to the AP1302
 * @win_pos: Firmware load window current position
 * @buf: Firmware data buffer
 * @len: Firmware data length
 *
 * The firmware is loaded through a window in the registers space. Writes are
 * sequential starting at address 0x8000, and must wrap around when reaching
 * 0x9fff. This function write the firmware data stored in @buf to the AP1302,
 * keeping track of the window position in the @win_pos argument.
 */
static int ap1302_write_fw_window(struct ap1302_device *ap1302, const u8 *buf,
				  u32 len, unsigned int *win_pos)
{
	while (len > 0) {
		unsigned int write_addr;
		unsigned int write_size;
		int ret;
		int i;

		/*
		 * Write at most len bytes, from the current position to the
		 * end of the window.
		 */
		write_addr = *win_pos + AP1302_FW_WINDOW_OFFSET;
		write_size = min(len, AP1302_FW_WINDOW_SIZE - *win_pos);

		ret = regmap_raw_write(ap1302->regmap16, write_addr, buf,
				       write_size);
		if (ret)
			return ret;

		for (i = 0; i < write_size; i++)
			ap1302_update_crc(ap1302, write_addr + i, *(buf + i));

		buf += write_size;
		len -= write_size;

		*win_pos += write_size;
		if (*win_pos >= AP1302_FW_WINDOW_SIZE)
			*win_pos = 0;
	}

	return 0;
}

static int ap1302_load_firmware(struct ap1302_device *ap1302)
{
	const struct ap1302_firmware_header *fw_hdr;
	unsigned int fw_size;
	const u8 *fw_data;
	unsigned int win_pos = 0;
	int ret;
	int pll_init_size;

	fw_hdr = (const struct ap1302_firmware_header *)ap1302->fw->data;

	switch (fw_hdr->version) {
	case AP1302_HEADER_VERSION(1, 0, 0):
		fw_data = (u8 *)fw_hdr     + fw_hdr->header_size;
		fw_size = ap1302->fw->size - fw_hdr->header_size;
		pll_init_size = fw_hdr->pll_init_size;
		break;
	default:
		dev_info(ap1302->dev, "Unknown header version %08x, assuming no header.",
		         fw_hdr->version);
		fw_data = ap1302->fw->data;
		fw_size = ap1302->fw->size;
		/* It is safe to assume that PLL configuration is contained
		 * within first 4094B of data. So if pll_init_size is not
		 * explicitly provided we can use this value as a backup. We
		 * should not set this value any larger since crossing the
		 * 4KB page boundary also triggers bootdata execution.
		 */
		pll_init_size = min(4094u, fw_size);
		break;
	}


	/*
	 * Load the PLL initialization settings, set the bootdata stage to 2 to
	 * apply the basic_init_hp settings, and wait 1ms for the PLL to lock.
	 */
	ret = ap1302_write_fw_window(ap1302, fw_data, pll_init_size,
				     &win_pos);
	if (ret)
		return ret;

	ret = ap1302_write(ap1302, AP1302_BOOTDATA_STAGE, 0x0002);
	if (ret)
		return ret;

	usleep_range(1000, 2000);

	/* Load the rest of the bootdata content and verify the CRC. */
	ret = ap1302_write_fw_window(ap1302, fw_data + pll_init_size,
				     fw_size - pll_init_size, &win_pos);
	if (ret)
		return ret;

	msleep(40);

	ret = ap1302_check_crc(ap1302);
	if (ret)
		return ret;

	/*
	 * Write 0xffff to the bootdata_stage register to indicate to the
	 * AP1302 that the whole bootdata content has been loaded.
	 */
	ret = ap1302_write(ap1302, AP1302_BOOTDATA_STAGE, 0xffff);
	if (ret)
		return ret;

	/* The AP1302 starts outputting frames right after boot, stop it. */
	return ap1302_stall(ap1302, true);
}

static int ap1302_detect_chip(struct ap1302_device *ap1302)
{
	unsigned int version;
	unsigned int revision;
	int ret;

	ret = ap1302_read(ap1302, AP1302_CHIP_VERSION, &version);
	if (ret)
		return ret;

	ret = ap1302_read(ap1302, AP1302_CHIP_REV, &revision);
	if (ret)
		return ret;

	if (version != AP1302_CHIP_ID) {
		dev_err(ap1302->dev,
			"Invalid chip version, expected 0x%04x, got 0x%04x\n",
			AP1302_CHIP_ID, version);
		return -EINVAL;
	}

	dev_info(ap1302->dev, "AP1302 revision %u.%u.%u detected\n",
		 (revision & 0xf000) >> 12, (revision & 0x0f00) >> 8,
		 revision & 0x00ff);

	return 0;
}

static int ap1302_hw_init(struct ap1302_device *ap1302)
{
	unsigned int retries;
	int ret;

	/* Request and validate the firmware. */
	ret = ap1302_request_firmware(ap1302);
	if (ret)
		return ret;

	/*
	 * Power the sensors first, as the firmware will access them once it
	 * gets loaded.
	 */
	ret = ap1302_power_on_sensors(ap1302);
	if (ret < 0)
		goto error_firmware;

	/*
	 * Load the firmware, retrying in case of CRC errors. The AP1302 is
	 * reset with a full power cycle between each attempt.
	 */
	for (retries = 0; retries < MAX_FW_LOAD_RETRIES; ++retries) {
		ret = ap1302_power_on(ap1302);
		if (ret < 0)
			goto error_power_sensors;

		ret = ap1302_detect_chip(ap1302);
		if (ret)
			goto error_power;

		ret = ap1302_load_firmware(ap1302);
		if (!ret)
			break;

		if (ret != -EAGAIN)
			goto error_power;

		ap1302_power_off(ap1302);
	}

	if (retries == MAX_FW_LOAD_RETRIES) {
		dev_err(ap1302->dev,
			"Firmware load retries exceeded, aborting\n");
		ret = -ETIMEDOUT;
		goto error_power_sensors;
	}

	return 0;

error_power:
	ap1302_power_off(ap1302);
error_power_sensors:
	ap1302_power_off_sensors(ap1302);
error_firmware:
	release_firmware(ap1302->fw);

	return ret;
}

static void ap1302_hw_cleanup(struct ap1302_device *ap1302)
{
	ap1302_power_off(ap1302);
	ap1302_power_off_sensors(ap1302);
}

/* -----------------------------------------------------------------------------
 * Probe & Remove
 */

static int ap1302_config_v4l2(struct ap1302_device *ap1302)
{
	struct v4l2_subdev *sd;
	unsigned int i;
	int ret;

	sd = &ap1302->sd;
	sd->dev = ap1302->dev;
	v4l2_i2c_subdev_init(sd, ap1302->client, &ap1302_subdev_ops);

	strlcpy(sd->name, DRIVER_NAME, sizeof(sd->name));
	strlcat(sd->name, ".", sizeof(sd->name));
	strlcat(sd->name, dev_name(ap1302->dev), sizeof(sd->name));
	dev_dbg(ap1302->dev, "name %s\n", sd->name);

	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
	sd->internal_ops = &ap1302_subdev_internal_ops;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sd->entity.ops = &ap1302_media_ops;

	for (i = 0; i < ARRAY_SIZE(ap1302->pads); ++i)
		ap1302->pads[i].flags = i == AP1302_PAD_SOURCE
				      ? MEDIA_PAD_FL_SOURCE : MEDIA_PAD_FL_SINK;

	ret = media_entity_pads_init(&sd->entity, ARRAY_SIZE(ap1302->pads),
				     ap1302->pads);
	if (ret < 0) {
		dev_err(ap1302->dev, "media_entity_init failed %d\n", ret);
		return ret;
	}

	for (i = 0; i < ARRAY_SIZE(ap1302->formats); ++i)
		ap1302->formats[i].info = &supported_video_formats[0];

	ret = ap1302_init_cfg(sd, NULL);
	if (ret < 0)
		goto error_media;

	ret = ap1302_ctrls_init(ap1302);
	if (ret < 0)
		goto error_media;

	ret = v4l2_async_register_subdev(sd);
	if (ret < 0) {
		dev_err(ap1302->dev, "v4l2_async_register_subdev failed %d\n", ret);
		goto error_ctrls;
	}

	return 0;

error_ctrls:
	ap1302_ctrls_cleanup(ap1302);
error_media:
	media_entity_cleanup(&sd->entity);
	return ret;
}

static int ap1302_parse_of(struct ap1302_device *ap1302)
{
	struct device_node *sensors;
	struct device_node *node;
	unsigned int num_sensors = 0;
	const char *model;
	unsigned int i;
	int ret;

	/* Clock */
	ap1302->clock = devm_clk_get(ap1302->dev, NULL);
	if (IS_ERR(ap1302->clock)) {
		dev_err(ap1302->dev, "Failed to get clock: %ld\n",
			PTR_ERR(ap1302->clock));
		return PTR_ERR(ap1302->clock);
	}

	/* GPIOs */
	ap1302->reset_gpio = devm_gpiod_get(ap1302->dev, "reset",
					    GPIOD_OUT_HIGH);
	if (IS_ERR(ap1302->reset_gpio)) {
		dev_err(ap1302->dev, "Can't get reset GPIO: %ld\n",
			PTR_ERR(ap1302->reset_gpio));
		return PTR_ERR(ap1302->reset_gpio);
	}

	ap1302->standby_gpio = devm_gpiod_get_optional(ap1302->dev, "standby",
						       GPIOD_OUT_LOW);
	if (IS_ERR(ap1302->standby_gpio)) {
		dev_err(ap1302->dev, "Can't get standby GPIO: %ld\n",
			PTR_ERR(ap1302->standby_gpio));
		return PTR_ERR(ap1302->standby_gpio);
	}

	/* Sensors */
	sensors = of_get_child_by_name(ap1302->dev->of_node, "sensors");
	if (!sensors) {
		dev_err(ap1302->dev, "'sensors' child node not found\n");
		return -EINVAL;
	}

	ret = of_property_read_string(sensors, "onnn,model", &model);
	if (ret < 0) {
		/*
		 * If no sensor is connected, we can still support operation
		 * with the test pattern generator.
		 */
		ap1302->sensor_info = &ap1302_sensor_info_tpg;
		ap1302->width_factor = 1;
		ret = 0;
		goto done;
	}

	for (i = 0; i < ARRAY_SIZE(ap1302_sensor_info); ++i) {
		const struct ap1302_sensor_info *info =
			&ap1302_sensor_info[i];

		if (!strcmp(info->model, model)) {
			ap1302->sensor_info = info;
			break;
		}
	}

	if (!ap1302->sensor_info) {
		dev_warn(ap1302->dev, "Unsupported sensor model %s\n", model);
		ret = -EINVAL;
		goto done;
	}

	for_each_child_of_node(sensors, node) {
		if (of_node_name_eq(node, "sensor")) {
			if (!ap1302_sensor_parse_of(ap1302, node))
				num_sensors++;
		}
	}

	if (!num_sensors) {
		dev_err(ap1302->dev, "No sensor found\n");
		ret = -EINVAL;
		goto done;
	}

	ap1302->width_factor = num_sensors;

done:
	of_node_put(sensors);
	return ret;
}

static void ap1302_cleanup(struct ap1302_device *ap1302)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ap1302->sensors); ++i) {
		struct ap1302_sensor *sensor = &ap1302->sensors[i];

		if (!sensor->ap1302)
			continue;

		ap1302_sensor_cleanup(sensor);
	}

	mutex_destroy(&ap1302->lock);
}

static int ap1302_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct ap1302_device *ap1302;
	unsigned int i;
	int ret;

	ap1302 = devm_kzalloc(&client->dev, sizeof(*ap1302), GFP_KERNEL);
	if (!ap1302)
		return -ENOMEM;

	ap1302->dev = &client->dev;
	ap1302->client = client;

	mutex_init(&ap1302->lock);

	ap1302->regmap16 = devm_regmap_init_i2c(client, &ap1302_reg16_config);
	if (IS_ERR(ap1302->regmap16)) {
		dev_err(ap1302->dev, "regmap16 init failed: %ld\n",
			PTR_ERR(ap1302->regmap16));
		ret = -ENODEV;
		goto error;
	}

	ap1302->regmap32 = devm_regmap_init_i2c(client, &ap1302_reg32_config);
	if (IS_ERR(ap1302->regmap32)) {
		dev_err(ap1302->dev, "regmap32 init failed: %ld\n",
			PTR_ERR(ap1302->regmap32));
		ret = -ENODEV;
		goto error;
	}

	ret = ap1302_parse_of(ap1302);
	if (ret < 0)
		goto error;

	for (i = 0; i < ARRAY_SIZE(ap1302->sensors); ++i) {
		struct ap1302_sensor *sensor = &ap1302->sensors[i];

		if (!sensor->ap1302)
			continue;

		ret = ap1302_sensor_init(sensor, i);
		if (ret < 0)
			goto error;
	}

	ret = ap1302_hw_init(ap1302);
	if (ret)
		goto error;

	ret = ap1302_config_v4l2(ap1302);
	if (ret)
		goto error_hw_cleanup;

	return 0;

error_hw_cleanup:
	ap1302_hw_cleanup(ap1302);
error:
	ap1302_cleanup(ap1302);
	return ret;
}

static int ap1302_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ap1302_device *ap1302 = to_ap1302(sd);

	ap1302_hw_cleanup(ap1302);

	release_firmware(ap1302->fw);

	v4l2_device_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);

	ap1302_ctrls_cleanup(ap1302);

	ap1302_cleanup(ap1302);

	return 0;
}

static const struct of_device_id ap1302_of_id_table[] = {
	{ .compatible = "onnn,ap1302" },
	{ }
};
MODULE_DEVICE_TABLE(of, ap1302_of_id_table);

static struct i2c_driver ap1302_i2c_driver = {
	.driver = {
		.name	= DRIVER_NAME,
		.of_match_table	= ap1302_of_id_table,
	},
	.probe		= ap1302_probe,
	.remove		= ap1302_remove,
};

module_i2c_driver(ap1302_i2c_driver);

MODULE_AUTHOR("Florian Rebaudo <frebaudo@witekio.com>");
MODULE_DESCRIPTION("Driver for ap1302 mezzanine");
MODULE_LICENSE("GPL");
