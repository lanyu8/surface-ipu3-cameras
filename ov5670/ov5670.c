// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017 Intel Corporation.

#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#define OV5670_REG_CHIP_ID		0x300a
#define OV5670_CHIP_ID			0x005670

#define OV5670_REG_MODE_SELECT		0x0100
#define OV5670_MODE_STANDBY		0x00
#define OV5670_MODE_STREAMING		0x01

#define OV5670_REG_SOFTWARE_RST		0x0103
#define OV5670_SOFTWARE_RST		0x01

/* vertical-timings from sensor */
#define OV5670_REG_VTS			0x380e
#define OV5670_VTS_30FPS		0x0808 /* default for 30 fps */
#define OV5670_VTS_MAX			0xffff

/* horizontal-timings from sensor */
#define OV5670_REG_HTS			0x380c

/*
 * Pixels-per-line(PPL) = Time-per-line * pixel-rate
 * In OV5670, Time-per-line = HTS/SCLK.
 * HTS is fixed for all resolutions, not recommended to change.
 */
#define OV5670_FIXED_PPL		2724	/* Pixels per line */

/* Exposure controls from sensor */
#define OV5670_REG_EXPOSURE		0x3500
#define	OV5670_EXPOSURE_MIN		4
#define	OV5670_EXPOSURE_STEP		1

/* Analog gain controls from sensor */
#define OV5670_REG_ANALOG_GAIN		0x3508
#define	ANALOG_GAIN_MIN			0
#define	ANALOG_GAIN_MAX			8191
#define	ANALOG_GAIN_STEP		1
#define	ANALOG_GAIN_DEFAULT		128

/* Digital gain controls from sensor */
#define OV5670_REG_R_DGTL_GAIN		0x5032
#define OV5670_REG_G_DGTL_GAIN		0x5034
#define OV5670_REG_B_DGTL_GAIN		0x5036
#define OV5670_DGTL_GAIN_MIN		0
#define OV5670_DGTL_GAIN_MAX		4095
#define OV5670_DGTL_GAIN_STEP		1
#define OV5670_DGTL_GAIN_DEFAULT	1024

/* Test Pattern Control */
#define OV5670_REG_TEST_PATTERN		0x4303
#define OV5670_TEST_PATTERN_ENABLE	BIT(3)
#define OV5670_REG_TEST_PATTERN_CTRL	0x4320

#define OV5670_REG_VALUE_08BIT		1
#define OV5670_REG_VALUE_16BIT		2
#define OV5670_REG_VALUE_24BIT		3

/* Initial number of frames to skip to avoid possible garbage */
#define OV5670_NUM_OF_SKIP_FRAMES	2

struct ov5670_reg {
	u16 address;
	u8 val;
};

struct ov5670_reg_list {
	u32 num_of_regs;
	const struct ov5670_reg *regs;
};

struct ov5670_link_freq_config {
	u32 pixel_rate;
	const struct ov5670_reg_list reg_list;
};

struct ov5670_mode {
	/* Frame width in pixels */
	u32 width;

	/* Frame height in pixels */
	u32 height;

	/* Default vertical timining size */
	u32 vts_def;

	/* Min vertical timining size */
	u32 vts_min;

	/* Link frequency needed for this resolution */
	u32 link_freq_index;

	/* Sensor register settings for this resolution */
	const struct ov5670_reg_list reg_list;
};

static const struct ov5670_reg mipi_data_rate_840mbps[] = {
	{0x0300, 0x04},
	{0x0301, 0x00},
	{0x0302, 0x84},
	{0x0303, 0x00},
	{0x0304, 0x03},
	{0x0305, 0x01},
	{0x0306, 0x01},
	{0x030a, 0x00},
	{0x030b, 0x00},
	{0x030c, 0x00},
	{0x030d, 0x26},
	{0x030e, 0x00},
	{0x030f, 0x06},
	{0x0312, 0x01},
	{0x3031, 0x0a},
};

static const struct ov5670_reg mode_2592x1944_regs[] = {
	{0x3000, 0x00},
	{0x3002, 0x21},
	{0x3005, 0xf0},
	{0x3007, 0x00},
	{0x3015, 0x0f},
	{0x3018, 0x32},
	{0x301a, 0xf0},
	{0x301b, 0xf0},
	{0x301c, 0xf0},
	{0x301d, 0xf0},
	{0x301e, 0xf0},
	{0x3030, 0x00},
	{0x3031, 0x0a},
	{0x303c, 0xff},
	{0x303e, 0xff},
	{0x3040, 0xf0},
	{0x3041, 0x00},
	{0x3042, 0xf0},
	{0x3106, 0x11},
	{0x3500, 0x00},
	{0x3501, 0x80},
	{0x3502, 0x00},
	{0x3503, 0x04},
	{0x3504, 0x03},
	{0x3505, 0x83},
	{0x3508, 0x04},
	{0x3509, 0x00},
	{0x350e, 0x04},
	{0x350f, 0x00},
	{0x3510, 0x00},
	{0x3511, 0x02},
	{0x3512, 0x00},
	{0x3601, 0xc8},
	{0x3610, 0x88},
	{0x3612, 0x48},
	{0x3614, 0x5b},
	{0x3615, 0x96},
	{0x3621, 0xd0},
	{0x3622, 0x00},
	{0x3623, 0x00},
	{0x3633, 0x13},
	{0x3634, 0x13},
	{0x3635, 0x13},
	{0x3636, 0x13},
	{0x3645, 0x13},
	{0x3646, 0x82},
	{0x3650, 0x00},
	{0x3652, 0xff},
	{0x3655, 0x20},
	{0x3656, 0xff},
	{0x365a, 0xff},
	{0x365e, 0xff},
	{0x3668, 0x00},
	{0x366a, 0x07},
	{0x366e, 0x10},
	{0x366d, 0x00},
	{0x366f, 0x80},
	{0x3700, 0x28},
	{0x3701, 0x10},
	{0x3702, 0x3a},
	{0x3703, 0x19},
	{0x3704, 0x10},
	{0x3705, 0x00},
	{0x3706, 0x66},
	{0x3707, 0x08},
	{0x3708, 0x34},
	{0x3709, 0x40},
	{0x370a, 0x01},
	{0x370b, 0x1b},
	{0x3714, 0x24},
	{0x371a, 0x3e},
	{0x3733, 0x00},
	{0x3734, 0x00},
	{0x373a, 0x05},
	{0x373b, 0x06},
	{0x373c, 0x0a},
	{0x373f, 0xa0},
	{0x3755, 0x00},
	{0x3758, 0x00},
	{0x375b, 0x0e},
	{0x3766, 0x5f},
	{0x3768, 0x00},
	{0x3769, 0x22},
	{0x3773, 0x08},
	{0x3774, 0x1f},
	{0x3776, 0x06},
	{0x37a0, 0x88},
	{0x37a1, 0x5c},
	{0x37a7, 0x88},
	{0x37a8, 0x70},
	{0x37aa, 0x88},
	{0x37ab, 0x48},
	{0x37b3, 0x66},
	{0x37c2, 0x04},
	{0x37c5, 0x00},
	{0x37c8, 0x00},
	{0x3800, 0x00},
	{0x3801, 0x0c},
	{0x3802, 0x00},
	{0x3803, 0x04},
	{0x3804, 0x0a},
	{0x3805, 0x33},
	{0x3806, 0x07},
	{0x3807, 0xa3},
	{0x3808, 0x0a},
	{0x3809, 0x20},
	{0x380a, 0x07},
	{0x380b, 0x98},
	{0x380c, 0x06},
	{0x380d, 0x90},
	{0x380e, 0x08},
	{0x380f, 0x08},
	{0x3811, 0x04},
	{0x3813, 0x02},
	{0x3814, 0x01},
	{0x3815, 0x01},
	{0x3816, 0x00},
	{0x3817, 0x00},
	{0x3818, 0x00},
	{0x3819, 0x00},
	{0x3820, 0x84},
	{0x3821, 0x46},
	{0x3822, 0x48},
	{0x3826, 0x00},
	{0x3827, 0x08},
	{0x382a, 0x01},
	{0x382b, 0x01},
	{0x3830, 0x08},
	{0x3836, 0x02},
	{0x3837, 0x00},
	{0x3838, 0x10},
	{0x3841, 0xff},
	{0x3846, 0x48},
	{0x3861, 0x00},
	{0x3862, 0x04},
	{0x3863, 0x06},
	{0x3a11, 0x01},
	{0x3a12, 0x78},
	{0x3b00, 0x00},
	{0x3b02, 0x00},
	{0x3b03, 0x00},
	{0x3b04, 0x00},
	{0x3b05, 0x00},
	{0x3c00, 0x89},
	{0x3c01, 0xab},
	{0x3c02, 0x01},
	{0x3c03, 0x00},
	{0x3c04, 0x00},
	{0x3c05, 0x03},
	{0x3c06, 0x00},
	{0x3c07, 0x05},
	{0x3c0c, 0x00},
	{0x3c0d, 0x00},
	{0x3c0e, 0x00},
	{0x3c0f, 0x00},
	{0x3c40, 0x00},
	{0x3c41, 0xa3},
	{0x3c43, 0x7d},
	{0x3c45, 0xd7},
	{0x3c47, 0xfc},
	{0x3c50, 0x05},
	{0x3c52, 0xaa},
	{0x3c54, 0x71},
	{0x3c56, 0x80},
	{0x3d85, 0x17},
	{0x3f03, 0x00},
	{0x3f0a, 0x00},
	{0x3f0b, 0x00},
	{0x4001, 0x60},
	{0x4009, 0x0d},
	{0x4020, 0x00},
	{0x4021, 0x00},
	{0x4022, 0x00},
	{0x4023, 0x00},
	{0x4024, 0x00},
	{0x4025, 0x00},
	{0x4026, 0x00},
	{0x4027, 0x00},
	{0x4028, 0x00},
	{0x4029, 0x00},
	{0x402a, 0x00},
	{0x402b, 0x00},
	{0x402c, 0x00},
	{0x402d, 0x00},
	{0x402e, 0x00},
	{0x402f, 0x00},
	{0x4040, 0x00},
	{0x4041, 0x03},
	{0x4042, 0x00},
	{0x4043, 0x7A},
	{0x4044, 0x00},
	{0x4045, 0x7A},
	{0x4046, 0x00},
	{0x4047, 0x7A},
	{0x4048, 0x00},
	{0x4049, 0x7A},
	{0x4307, 0x30},
	{0x4500, 0x58},
	{0x4501, 0x04},
	{0x4502, 0x40},
	{0x4503, 0x10},
	{0x4508, 0xaa},
	{0x4509, 0xaa},
	{0x450a, 0x00},
	{0x450b, 0x00},
	{0x4600, 0x01},
	{0x4601, 0x03},
	{0x4700, 0xa4},
	{0x4800, 0x4c},
	{0x4816, 0x53},
	{0x481f, 0x40},
	{0x4837, 0x13},
	{0x5000, 0x56},
	{0x5001, 0x01},
	{0x5002, 0x28},
	{0x5004, 0x0c},
	{0x5006, 0x0c},
	{0x5007, 0xe0},
	{0x5008, 0x01},
	{0x5009, 0xb0},
	{0x5901, 0x00},
	{0x5a01, 0x00},
	{0x5a03, 0x00},
	{0x5a04, 0x0c},
	{0x5a05, 0xe0},
	{0x5a06, 0x09},
	{0x5a07, 0xb0},
	{0x5a08, 0x06},
	{0x5e00, 0x00},
	{0x3734, 0x40},
	{0x5b00, 0x01},
	{0x5b01, 0x10},
	{0x5b02, 0x01},
	{0x5b03, 0xdb},
	{0x3d8c, 0x71},
	{0x3d8d, 0xea},
	{0x4017, 0x08},
	{0x3618, 0x2a},
	{0x5780, 0x3e},
	{0x5781, 0x0f},
	{0x5782, 0x44},
	{0x5783, 0x02},
	{0x5784, 0x01},
	{0x5785, 0x01},
	{0x5786, 0x00},
	{0x5787, 0x04},
	{0x5788, 0x02},
	{0x5789, 0x0f},
	{0x578a, 0xfd},
	{0x578b, 0xf5},
	{0x578c, 0xf5},
	{0x578d, 0x03},
	{0x578e, 0x08},
	{0x578f, 0x0c},
	{0x5790, 0x08},
	{0x5791, 0x06},
	{0x5792, 0x00},
	{0x5793, 0x52},
	{0x5794, 0xa3},
	{0x3503, 0x00},
	{0x5045, 0x05},
	{0x4003, 0x40},
	{0x5048, 0x40}
};

static const struct ov5670_reg mode_1296x972_regs[] = {
	{0x3000, 0x00},
	{0x3002, 0x21},
	{0x3005, 0xf0},
	{0x3007, 0x00},
	{0x3015, 0x0f},
	{0x3018, 0x32},
	{0x301a, 0xf0},
	{0x301b, 0xf0},
	{0x301c, 0xf0},
	{0x301d, 0xf0},
	{0x301e, 0xf0},
	{0x3030, 0x00},
	{0x3031, 0x0a},
	{0x303c, 0xff},
	{0x303e, 0xff},
	{0x3040, 0xf0},
	{0x3041, 0x00},
	{0x3042, 0xf0},
	{0x3106, 0x11},
	{0x3500, 0x00},
	{0x3501, 0x80},
	{0x3502, 0x00},
	{0x3503, 0x04},
	{0x3504, 0x03},
	{0x3505, 0x83},
	{0x3508, 0x07},
	{0x3509, 0x80},
	{0x350e, 0x04},
	{0x350f, 0x00},
	{0x3510, 0x00},
	{0x3511, 0x02},
	{0x3512, 0x00},
	{0x3601, 0xc8},
	{0x3610, 0x88},
	{0x3612, 0x48},
	{0x3614, 0x5b},
	{0x3615, 0x96},
	{0x3621, 0xd0},
	{0x3622, 0x00},
	{0x3623, 0x00},
	{0x3633, 0x13},
	{0x3634, 0x13},
	{0x3635, 0x13},
	{0x3636, 0x13},
	{0x3645, 0x13},
	{0x3646, 0x82},
	{0x3650, 0x00},
	{0x3652, 0xff},
	{0x3655, 0x20},
	{0x3656, 0xff},
	{0x365a, 0xff},
	{0x365e, 0xff},
	{0x3668, 0x00},
	{0x366a, 0x07},
	{0x366e, 0x08},
	{0x366d, 0x00},
	{0x366f, 0x80},
	{0x3700, 0x28},
	{0x3701, 0x10},
	{0x3702, 0x3a},
	{0x3703, 0x19},
	{0x3704, 0x10},
	{0x3705, 0x00},
	{0x3706, 0x66},
	{0x3707, 0x08},
	{0x3708, 0x34},
	{0x3709, 0x40},
	{0x370a, 0x01},
	{0x370b, 0x1b},
	{0x3714, 0x24},
	{0x371a, 0x3e},
	{0x3733, 0x00},
	{0x3734, 0x00},
	{0x373a, 0x05},
	{0x373b, 0x06},
	{0x373c, 0x0a},
	{0x373f, 0xa0},
	{0x3755, 0x00},
	{0x3758, 0x00},
	{0x375b, 0x0e},
	{0x3766, 0x5f},
	{0x3768, 0x00},
	{0x3769, 0x22},
	{0x3773, 0x08},
	{0x3774, 0x1f},
	{0x3776, 0x06},
	{0x37a0, 0x88},
	{0x37a1, 0x5c},
	{0x37a7, 0x88},
	{0x37a8, 0x70},
	{0x37aa, 0x88},
	{0x37ab, 0x48},
	{0x37b3, 0x66},
	{0x37c2, 0x04},
	{0x37c5, 0x00},
	{0x37c8, 0x00},
	{0x3800, 0x00},
	{0x3801, 0x0c},
	{0x3802, 0x00},
	{0x3803, 0x04},
	{0x3804, 0x0a},
	{0x3805, 0x33},
	{0x3806, 0x07},
	{0x3807, 0xa3},
	{0x3808, 0x05},
	{0x3809, 0x10},
	{0x380a, 0x03},
	{0x380b, 0xcc},
	{0x380c, 0x06},
	{0x380d, 0x90},
	{0x380e, 0x08},
	{0x380f, 0x08},
	{0x3811, 0x04},
	{0x3813, 0x04},
	{0x3814, 0x03},
	{0x3815, 0x01},
	{0x3816, 0x00},
	{0x3817, 0x00},
	{0x3818, 0x00},
	{0x3819, 0x00},
	{0x3820, 0x94},
	{0x3821, 0x47},
	{0x3822, 0x48},
	{0x3826, 0x00},
	{0x3827, 0x08},
	{0x382a, 0x03},
	{0x382b, 0x01},
	{0x3830, 0x08},
	{0x3836, 0x02},
	{0x3837, 0x00},
	{0x3838, 0x10},
	{0x3841, 0xff},
	{0x3846, 0x48},
	{0x3861, 0x00},
	{0x3862, 0x04},
	{0x3863, 0x06},
	{0x3a11, 0x01},
	{0x3a12, 0x78},
	{0x3b00, 0x00},
	{0x3b02, 0x00},
	{0x3b03, 0x00},
	{0x3b04, 0x00},
	{0x3b05, 0x00},
	{0x3c00, 0x89},
	{0x3c01, 0xab},
	{0x3c02, 0x01},
	{0x3c03, 0x00},
	{0x3c04, 0x00},
	{0x3c05, 0x03},
	{0x3c06, 0x00},
	{0x3c07, 0x05},
	{0x3c0c, 0x00},
	{0x3c0d, 0x00},
	{0x3c0e, 0x00},
	{0x3c0f, 0x00},
	{0x3c40, 0x00},
	{0x3c41, 0xa3},
	{0x3c43, 0x7d},
	{0x3c45, 0xd7},
	{0x3c47, 0xfc},
	{0x3c50, 0x05},
	{0x3c52, 0xaa},
	{0x3c54, 0x71},
	{0x3c56, 0x80},
	{0x3d85, 0x17},
	{0x3f03, 0x00},
	{0x3f0a, 0x00},
	{0x3f0b, 0x00},
	{0x4001, 0x60},
	{0x4009, 0x05},
	{0x4020, 0x00},
	{0x4021, 0x00},
	{0x4022, 0x00},
	{0x4023, 0x00},
	{0x4024, 0x00},
	{0x4025, 0x00},
	{0x4026, 0x00},
	{0x4027, 0x00},
	{0x4028, 0x00},
	{0x4029, 0x00},
	{0x402a, 0x00},
	{0x402b, 0x00},
	{0x402c, 0x00},
	{0x402d, 0x00},
	{0x402e, 0x00},
	{0x402f, 0x00},
	{0x4040, 0x00},
	{0x4041, 0x03},
	{0x4042, 0x00},
	{0x4043, 0x7A},
	{0x4044, 0x00},
	{0x4045, 0x7A},
	{0x4046, 0x00},
	{0x4047, 0x7A},
	{0x4048, 0x00},
	{0x4049, 0x7A},
	{0x4307, 0x30},
	{0x4500, 0x58},
	{0x4501, 0x04},
	{0x4502, 0x48},
	{0x4503, 0x10},
	{0x4508, 0x55},
	{0x4509, 0x55},
	{0x450a, 0x00},
	{0x450b, 0x00},
	{0x4600, 0x00},
	{0x4601, 0x81},
	{0x4700, 0xa4},
	{0x4800, 0x4c},
	{0x4816, 0x53},
	{0x481f, 0x40},
	{0x4837, 0x13},
	{0x5000, 0x56},
	{0x5001, 0x01},
	{0x5002, 0x28},
	{0x5004, 0x0c},
	{0x5006, 0x0c},
	{0x5007, 0xe0},
	{0x5008, 0x01},
	{0x5009, 0xb0},
	{0x5901, 0x00},
	{0x5a01, 0x00},
	{0x5a03, 0x00},
	{0x5a04, 0x0c},
	{0x5a05, 0xe0},
	{0x5a06, 0x09},
	{0x5a07, 0xb0},
	{0x5a08, 0x06},
	{0x5e00, 0x00},
	{0x3734, 0x40},
	{0x5b00, 0x01},
	{0x5b01, 0x10},
	{0x5b02, 0x01},
	{0x5b03, 0xdb},
	{0x3d8c, 0x71},
	{0x3d8d, 0xea},
	{0x4017, 0x10},
	{0x3618, 0x2a},
	{0x5780, 0x3e},
	{0x5781, 0x0f},
	{0x5782, 0x44},
	{0x5783, 0x02},
	{0x5784, 0x01},
	{0x5785, 0x01},
	{0x5786, 0x00},
	{0x5787, 0x04},
	{0x5788, 0x02},
	{0x5789, 0x0f},
	{0x578a, 0xfd},
	{0x578b, 0xf5},
	{0x578c, 0xf5},
	{0x578d, 0x03},
	{0x578e, 0x08},
	{0x578f, 0x0c},
	{0x5790, 0x08},
	{0x5791, 0x04},
	{0x5792, 0x00},
	{0x5793, 0x52},
	{0x5794, 0xa3},
	{0x3503, 0x00},
	{0x5045, 0x05},
	{0x4003, 0x40},
	{0x5048, 0x40}
};

static const struct ov5670_reg mode_648x486_regs[] = {
	{0x3000, 0x00},
	{0x3002, 0x21},
	{0x3005, 0xf0},
	{0x3007, 0x00},
	{0x3015, 0x0f},
	{0x3018, 0x32},
	{0x301a, 0xf0},
	{0x301b, 0xf0},
	{0x301c, 0xf0},
	{0x301d, 0xf0},
	{0x301e, 0xf0},
	{0x3030, 0x00},
	{0x3031, 0x0a},
	{0x303c, 0xff},
	{0x303e, 0xff},
	{0x3040, 0xf0},
	{0x3041, 0x00},
	{0x3042, 0xf0},
	{0x3106, 0x11},
	{0x3500, 0x00},
	{0x3501, 0x80},
	{0x3502, 0x00},
	{0x3503, 0x04},
	{0x3504, 0x03},
	{0x3505, 0x83},
	{0x3508, 0x04},
	{0x3509, 0x00},
	{0x350e, 0x04},
	{0x350f, 0x00},
	{0x3510, 0x00},
	{0x3511, 0x02},
	{0x3512, 0x00},
	{0x3601, 0xc8},
	{0x3610, 0x88},
	{0x3612, 0x48},
	{0x3614, 0x5b},
	{0x3615, 0x96},
	{0x3621, 0xd0},
	{0x3622, 0x00},
	{0x3623, 0x04},
	{0x3633, 0x13},
	{0x3634, 0x13},
	{0x3635, 0x13},
	{0x3636, 0x13},
	{0x3645, 0x13},
	{0x3646, 0x82},
	{0x3650, 0x00},
	{0x3652, 0xff},
	{0x3655, 0x20},
	{0x3656, 0xff},
	{0x365a, 0xff},
	{0x365e, 0xff},
	{0x3668, 0x00},
	{0x366a, 0x07},
	{0x366e, 0x08},
	{0x366d, 0x00},
	{0x366f, 0x80},
	{0x3700, 0x28},
	{0x3701, 0x10},
	{0x3702, 0x3a},
	{0x3703, 0x19},
	{0x3704, 0x10},
	{0x3705, 0x00},
	{0x3706, 0x66},
	{0x3707, 0x08},
	{0x3708, 0x34},
	{0x3709, 0x40},
	{0x370a, 0x01},
	{0x370b, 0x1b},
	{0x3714, 0x24},
	{0x371a, 0x3e},
	{0x3733, 0x00},
	{0x3734, 0x00},
	{0x373a, 0x05},
	{0x373b, 0x06},
	{0x373c, 0x0a},
	{0x373f, 0xa0},
	{0x3755, 0x00},
	{0x3758, 0x00},
	{0x375b, 0x0e},
	{0x3766, 0x5f},
	{0x3768, 0x00},
	{0x3769, 0x22},
	{0x3773, 0x08},
	{0x3774, 0x1f},
	{0x3776, 0x06},
	{0x37a0, 0x88},
	{0x37a1, 0x5c},
	{0x37a7, 0x88},
	{0x37a8, 0x70},
	{0x37aa, 0x88},
	{0x37ab, 0x48},
	{0x37b3, 0x66},
	{0x37c2, 0x04},
	{0x37c5, 0x00},
	{0x37c8, 0x00},
	{0x3800, 0x00},
	{0x3801, 0x0c},
	{0x3802, 0x00},
	{0x3803, 0x04},
	{0x3804, 0x0a},
	{0x3805, 0x33},
	{0x3806, 0x07},
	{0x3807, 0xa3},
	{0x3808, 0x02},
	{0x3809, 0x88},
	{0x380a, 0x01},
	{0x380b, 0xe6},
	{0x380c, 0x06},
	{0x380d, 0x90},
	{0x380e, 0x08},
	{0x380f, 0x08},
	{0x3811, 0x04},
	{0x3813, 0x02},
	{0x3814, 0x07},
	{0x3815, 0x01},
	{0x3816, 0x00},
	{0x3817, 0x00},
	{0x3818, 0x00},
	{0x3819, 0x00},
	{0x3820, 0x94},
	{0x3821, 0xc6},
	{0x3822, 0x48},
	{0x3826, 0x00},
	{0x3827, 0x08},
	{0x382a, 0x07},
	{0x382b, 0x01},
	{0x3830, 0x08},
	{0x3836, 0x02},
	{0x3837, 0x00},
	{0x3838, 0x10},
	{0x3841, 0xff},
	{0x3846, 0x48},
	{0x3861, 0x00},
	{0x3862, 0x04},
	{0x3863, 0x06},
	{0x3a11, 0x01},
	{0x3a12, 0x78},
	{0x3b00, 0x00},
	{0x3b02, 0x00},
	{0x3b03, 0x00},
	{0x3b04, 0x00},
	{0x3b05, 0x00},
	{0x3c00, 0x89},
	{0x3c01, 0xab},
	{0x3c02, 0x01},
	{0x3c03, 0x00},
	{0x3c04, 0x00},
	{0x3c05, 0x03},
	{0x3c06, 0x00},
	{0x3c07, 0x05},
	{0x3c0c, 0x00},
	{0x3c0d, 0x00},
	{0x3c0e, 0x00},
	{0x3c0f, 0x00},
	{0x3c40, 0x00},
	{0x3c41, 0xa3},
	{0x3c43, 0x7d},
	{0x3c45, 0xd7},
	{0x3c47, 0xfc},
	{0x3c50, 0x05},
	{0x3c52, 0xaa},
	{0x3c54, 0x71},
	{0x3c56, 0x80},
	{0x3d85, 0x17},
	{0x3f03, 0x00},
	{0x3f0a, 0x00},
	{0x3f0b, 0x00},
	{0x4001, 0x60},
	{0x4009, 0x05},
	{0x4020, 0x00},
	{0x4021, 0x00},
	{0x4022, 0x00},
	{0x4023, 0x00},
	{0x4024, 0x00},
	{0x4025, 0x00},
	{0x4026, 0x00},
	{0x4027, 0x00},
	{0x4028, 0x00},
	{0x4029, 0x00},
	{0x402a, 0x00},
	{0x402b, 0x00},
	{0x402c, 0x00},
	{0x402d, 0x00},
	{0x402e, 0x00},
	{0x402f, 0x00},
	{0x4040, 0x00},
	{0x4041, 0x03},
	{0x4042, 0x00},
	{0x4043, 0x7A},
	{0x4044, 0x00},
	{0x4045, 0x7A},
	{0x4046, 0x00},
	{0x4047, 0x7A},
	{0x4048, 0x00},
	{0x4049, 0x7A},
	{0x4307, 0x30},
	{0x4500, 0x58},
	{0x4501, 0x04},
	{0x4502, 0x40},
	{0x4503, 0x10},
	{0x4508, 0x55},
	{0x4509, 0x55},
	{0x450a, 0x02},
	{0x450b, 0x00},
	{0x4600, 0x00},
	{0x4601, 0x40},
	{0x4700, 0xa4},
	{0x4800, 0x4c},
	{0x4816, 0x53},
	{0x481f, 0x40},
	{0x4837, 0x13},
	{0x5000, 0x56},
	{0x5001, 0x01},
	{0x5002, 0x28},
	{0x5004, 0x0c},
	{0x5006, 0x0c},
	{0x5007, 0xe0},
	{0x5008, 0x01},
	{0x5009, 0xb0},
	{0x5901, 0x00},
	{0x5a01, 0x00},
	{0x5a03, 0x00},
	{0x5a04, 0x0c},
	{0x5a05, 0xe0},
	{0x5a06, 0x09},
	{0x5a07, 0xb0},
	{0x5a08, 0x06},
	{0x5e00, 0x00},
	{0x3734, 0x40},
	{0x5b00, 0x01},
	{0x5b01, 0x10},
	{0x5b02, 0x01},
	{0x5b03, 0xdb},
	{0x3d8c, 0x71},
	{0x3d8d, 0xea},
	{0x4017, 0x10},
	{0x3618, 0x2a},
	{0x5780, 0x3e},
	{0x5781, 0x0f},
	{0x5782, 0x44},
	{0x5783, 0x02},
	{0x5784, 0x01},
	{0x5785, 0x01},
	{0x5786, 0x00},
	{0x5787, 0x04},
	{0x5788, 0x02},
	{0x5789, 0x0f},
	{0x578a, 0xfd},
	{0x578b, 0xf5},
	{0x578c, 0xf5},
	{0x578d, 0x03},
	{0x578e, 0x08},
	{0x578f, 0x0c},
	{0x5790, 0x08},
	{0x5791, 0x06},
	{0x5792, 0x00},
	{0x5793, 0x52},
	{0x5794, 0xa3},
	{0x3503, 0x00},
	{0x5045, 0x05},
	{0x4003, 0x40},
	{0x5048, 0x40}
};

static const struct ov5670_reg mode_2560x1440_regs[] = {
	{0x3000, 0x00},
	{0x3002, 0x21},
	{0x3005, 0xf0},
	{0x3007, 0x00},
	{0x3015, 0x0f},
	{0x3018, 0x32},
	{0x301a, 0xf0},
	{0x301b, 0xf0},
	{0x301c, 0xf0},
	{0x301d, 0xf0},
	{0x301e, 0xf0},
	{0x3030, 0x00},
	{0x3031, 0x0a},
	{0x303c, 0xff},
	{0x303e, 0xff},
	{0x3040, 0xf0},
	{0x3041, 0x00},
	{0x3042, 0xf0},
	{0x3106, 0x11},
	{0x3500, 0x00},
	{0x3501, 0x80},
	{0x3502, 0x00},
	{0x3503, 0x04},
	{0x3504, 0x03},
	{0x3505, 0x83},
	{0x3508, 0x04},
	{0x3509, 0x00},
	{0x350e, 0x04},
	{0x350f, 0x00},
	{0x3510, 0x00},
	{0x3511, 0x02},
	{0x3512, 0x00},
	{0x3601, 0xc8},
	{0x3610, 0x88},
	{0x3612, 0x48},
	{0x3614, 0x5b},
	{0x3615, 0x96},
	{0x3621, 0xd0},
	{0x3622, 0x00},
	{0x3623, 0x00},
	{0x3633, 0x13},
	{0x3634, 0x13},
	{0x3635, 0x13},
	{0x3636, 0x13},
	{0x3645, 0x13},
	{0x3646, 0x82},
	{0x3650, 0x00},
	{0x3652, 0xff},
	{0x3655, 0x20},
	{0x3656, 0xff},
	{0x365a, 0xff},
	{0x365e, 0xff},
	{0x3668, 0x00},
	{0x366a, 0x07},
	{0x366e, 0x10},
	{0x366d, 0x00},
	{0x366f, 0x80},
	{0x3700, 0x28},
	{0x3701, 0x10},
	{0x3702, 0x3a},
	{0x3703, 0x19},
	{0x3704, 0x10},
	{0x3705, 0x00},
	{0x3706, 0x66},
	{0x3707, 0x08},
	{0x3708, 0x34},
	{0x3709, 0x40},
	{0x370a, 0x01},
	{0x370b, 0x1b},
	{0x3714, 0x24},
	{0x371a, 0x3e},
	{0x3733, 0x00},
	{0x3734, 0x00},
	{0x373a, 0x05},
	{0x373b, 0x06},
	{0x373c, 0x0a},
	{0x373f, 0xa0},
	{0x3755, 0x00},
	{0x3758, 0x00},
	{0x375b, 0x0e},
	{0x3766, 0x5f},
	{0x3768, 0x00},
	{0x3769, 0x22},
	{0x3773, 0x08},
	{0x3774, 0x1f},
	{0x3776, 0x06},
	{0x37a0, 0x88},
	{0x37a1, 0x5c},
	{0x37a7, 0x88},
	{0x37a8, 0x70},
	{0x37aa, 0x88},
	{0x37ab, 0x48},
	{0x37b3, 0x66},
	{0x37c2, 0x04},
	{0x37c5, 0x00},
	{0x37c8, 0x00},
	{0x3800, 0x00},
	{0x3801, 0x0c},
	{0x3802, 0x00},
	{0x3803, 0x04},
	{0x3804, 0x0a},
	{0x3805, 0x33},
	{0x3806, 0x07},
	{0x3807, 0xa3},
	{0x3808, 0x0a},
	{0x3809, 0x00},
	{0x380a, 0x05},
	{0x380b, 0xa0},
	{0x380c, 0x06},
	{0x380d, 0x90},
	{0x380e, 0x08},
	{0x380f, 0x08},
	{0x3811, 0x04},
	{0x3813, 0x02},
	{0x3814, 0x01},
	{0x3815, 0x01},
	{0x3816, 0x00},
	{0x3817, 0x00},
	{0x3818, 0x00},
	{0x3819, 0x00},
	{0x3820, 0x84},
	{0x3821, 0x46},
	{0x3822, 0x48},
	{0x3826, 0x00},
	{0x3827, 0x08},
	{0x382a, 0x01},
	{0x382b, 0x01},
	{0x3830, 0x08},
	{0x3836, 0x02},
	{0x3837, 0x00},
	{0x3838, 0x10},
	{0x3841, 0xff},
	{0x3846, 0x48},
	{0x3861, 0x00},
	{0x3862, 0x04},
	{0x3863, 0x06},
	{0x3a11, 0x01},
	{0x3a12, 0x78},
	{0x3b00, 0x00},
	{0x3b02, 0x00},
	{0x3b03, 0x00},
	{0x3b04, 0x00},
	{0x3b05, 0x00},
	{0x3c00, 0x89},
	{0x3c01, 0xab},
	{0x3c02, 0x01},
	{0x3c03, 0x00},
	{0x3c04, 0x00},
	{0x3c05, 0x03},
	{0x3c06, 0x00},
	{0x3c07, 0x05},
	{0x3c0c, 0x00},
	{0x3c0d, 0x00},
	{0x3c0e, 0x00},
	{0x3c0f, 0x00},
	{0x3c40, 0x00},
	{0x3c41, 0xa3},
	{0x3c43, 0x7d},
	{0x3c45, 0xd7},
	{0x3c47, 0xfc},
	{0x3c50, 0x05},
	{0x3c52, 0xaa},
	{0x3c54, 0x71},
	{0x3c56, 0x80},
	{0x3d85, 0x17},
	{0x3f03, 0x00},
	{0x3f0a, 0x00},
	{0x3f0b, 0x00},
	{0x4001, 0x60},
	{0x4009, 0x0d},
	{0x4020, 0x00},
	{0x4021, 0x00},
	{0x4022, 0x00},
	{0x4023, 0x00},
	{0x4024, 0x00},
	{0x4025, 0x00},
	{0x4026, 0x00},
	{0x4027, 0x00},
	{0x4028, 0x00},
	{0x4029, 0x00},
	{0x402a, 0x00},
	{0x402b, 0x00},
	{0x402c, 0x00},
	{0x402d, 0x00},
	{0x402e, 0x00},
	{0x402f, 0x00},
	{0x4040, 0x00},
	{0x4041, 0x03},
	{0x4042, 0x00},
	{0x4043, 0x7A},
	{0x4044, 0x00},
	{0x4045, 0x7A},
	{0x4046, 0x00},
	{0x4047, 0x7A},
	{0x4048, 0x00},
	{0x4049, 0x7A},
	{0x4307, 0x30},
	{0x4500, 0x58},
	{0x4501, 0x04},
	{0x4502, 0x40},
	{0x4503, 0x10},
	{0x4508, 0xaa},
	{0x4509, 0xaa},
	{0x450a, 0x00},
	{0x450b, 0x00},
	{0x4600, 0x01},
	{0x4601, 0x00},
	{0x4700, 0xa4},
	{0x4800, 0x4c},
	{0x4816, 0x53},
	{0x481f, 0x40},
	{0x4837, 0x13},
	{0x5000, 0x56},
	{0x5001, 0x01},
	{0x5002, 0x28},
	{0x5004, 0x0c},
	{0x5006, 0x0c},
	{0x5007, 0xe0},
	{0x5008, 0x01},
	{0x5009, 0xb0},
	{0x5901, 0x00},
	{0x5a01, 0x00},
	{0x5a03, 0x00},
	{0x5a04, 0x0c},
	{0x5a05, 0xe0},
	{0x5a06, 0x09},
	{0x5a07, 0xb0},
	{0x5a08, 0x06},
	{0x5e00, 0x00},
	{0x3734, 0x40},
	{0x5b00, 0x01},
	{0x5b01, 0x10},
	{0x5b02, 0x01},
	{0x5b03, 0xdb},
	{0x3d8c, 0x71},
	{0x3d8d, 0xea},
	{0x4017, 0x08},
	{0x3618, 0x2a},
	{0x5780, 0x3e},
	{0x5781, 0x0f},
	{0x5782, 0x44},
	{0x5783, 0x02},
	{0x5784, 0x01},
	{0x5785, 0x01},
	{0x5786, 0x00},
	{0x5787, 0x04},
	{0x5788, 0x02},
	{0x5789, 0x0f},
	{0x578a, 0xfd},
	{0x578b, 0xf5},
	{0x578c, 0xf5},
	{0x578d, 0x03},
	{0x578e, 0x08},
	{0x578f, 0x0c},
	{0x5790, 0x08},
	{0x5791, 0x06},
	{0x5792, 0x00},
	{0x5793, 0x52},
	{0x5794, 0xa3},
	{0x5045, 0x05},
	{0x4003, 0x40},
	{0x5048, 0x40}
};

static const struct ov5670_reg mode_1280x720_regs[] = {
	{0x3000, 0x00},
	{0x3002, 0x21},
	{0x3005, 0xf0},
	{0x3007, 0x00},
	{0x3015, 0x0f},
	{0x3018, 0x32},
	{0x301a, 0xf0},
	{0x301b, 0xf0},
	{0x301c, 0xf0},
	{0x301d, 0xf0},
	{0x301e, 0xf0},
	{0x3030, 0x00},
	{0x3031, 0x0a},
	{0x303c, 0xff},
	{0x303e, 0xff},
	{0x3040, 0xf0},
	{0x3041, 0x00},
	{0x3042, 0xf0},
	{0x3106, 0x11},
	{0x3500, 0x00},
	{0x3501, 0x80},
	{0x3502, 0x00},
	{0x3503, 0x04},
	{0x3504, 0x03},
	{0x3505, 0x83},
	{0x3508, 0x04},
	{0x3509, 0x00},
	{0x350e, 0x04},
	{0x350f, 0x00},
	{0x3510, 0x00},
	{0x3511, 0x02},
	{0x3512, 0x00},
	{0x3601, 0xc8},
	{0x3610, 0x88},
	{0x3612, 0x48},
	{0x3614, 0x5b},
	{0x3615, 0x96},
	{0x3621, 0xd0},
	{0x3622, 0x00},
	{0x3623, 0x00},
	{0x3633, 0x13},
	{0x3634, 0x13},
	{0x3635, 0x13},
	{0x3636, 0x13},
	{0x3645, 0x13},
	{0x3646, 0x82},
	{0x3650, 0x00},
	{0x3652, 0xff},
	{0x3655, 0x20},
	{0x3656, 0xff},
	{0x365a, 0xff},
	{0x365e, 0xff},
	{0x3668, 0x00},
	{0x366a, 0x07},
	{0x366e, 0x08},
	{0x366d, 0x00},
	{0x366f, 0x80},
	{0x3700, 0x28},
	{0x3701, 0x10},
	{0x3702, 0x3a},
	{0x3703, 0x19},
	{0x3704, 0x10},
	{0x3705, 0x00},
	{0x3706, 0x66},
	{0x3707, 0x08},
	{0x3708, 0x34},
	{0x3709, 0x40},
	{0x370a, 0x01},
	{0x370b, 0x1b},
	{0x3714, 0x24},
	{0x371a, 0x3e},
	{0x3733, 0x00},
	{0x3734, 0x00},
	{0x373a, 0x05},
	{0x373b, 0x06},
	{0x373c, 0x0a},
	{0x373f, 0xa0},
	{0x3755, 0x00},
	{0x3758, 0x00},
	{0x375b, 0x0e},
	{0x3766, 0x5f},
	{0x3768, 0x00},
	{0x3769, 0x22},
	{0x3773, 0x08},
	{0x3774, 0x1f},
	{0x3776, 0x06},
	{0x37a0, 0x88},
	{0x37a1, 0x5c},
	{0x37a7, 0x88},
	{0x37a8, 0x70},
	{0x37aa, 0x88},
	{0x37ab, 0x48},
	{0x37b3, 0x66},
	{0x37c2, 0x04},
	{0x37c5, 0x00},
	{0x37c8, 0x00},
	{0x3800, 0x00},
	{0x3801, 0x0c},
	{0x3802, 0x00},
	{0x3803, 0x04},
	{0x3804, 0x0a},
	{0x3805, 0x33},
	{0x3806, 0x07},
	{0x3807, 0xa3},
	{0x3808, 0x05},
	{0x3809, 0x00},
	{0x380a, 0x02},
	{0x380b, 0xd0},
	{0x380c, 0x06},
	{0x380d, 0x90},
	{0x380e, 0x08},
	{0x380f, 0x08},
	{0x3811, 0x04},
	{0x3813, 0x02},
	{0x3814, 0x03},
	{0x3815, 0x01},
	{0x3816, 0x00},
	{0x3817, 0x00},
	{0x3818, 0x00},
	{0x3819, 0x00},
	{0x3820, 0x94},
	{0x3821, 0x47},
	{0x3822, 0x48},
	{0x3826, 0x00},
	{0x3827, 0x08},
	{0x382a, 0x03},
	{0x382b, 0x01},
	{0x3830, 0x08},
	{0x3836, 0x02},
	{0x3837, 0x00},
	{0x3838, 0x10},
	{0x3841, 0xff},
	{0x3846, 0x48},
	{0x3861, 0x00},
	{0x3862, 0x04},
	{0x3863, 0x06},
	{0x3a11, 0x01},
	{0x3a12, 0x78},
	{0x3b00, 0x00},
	{0x3b02, 0x00},
	{0x3b03, 0x00},
	{0x3b04, 0x00},
	{0x3b05, 0x00},
	{0x3c00, 0x89},
	{0x3c01, 0xab},
	{0x3c02, 0x01},
	{0x3c03, 0x00},
	{0x3c04, 0x00},
	{0x3c05, 0x03},
	{0x3c06, 0x00},
	{0x3c07, 0x05},
	{0x3c0c, 0x00},
	{0x3c0d, 0x00},
	{0x3c0e, 0x00},
	{0x3c0f, 0x00},
	{0x3c40, 0x00},
	{0x3c41, 0xa3},
	{0x3c43, 0x7d},
	{0x3c45, 0xd7},
	{0x3c47, 0xfc},
	{0x3c50, 0x05},
	{0x3c52, 0xaa},
	{0x3c54, 0x71},
	{0x3c56, 0x80},
	{0x3d85, 0x17},
	{0x3f03, 0x00},
	{0x3f0a, 0x00},
	{0x3f0b, 0x00},
	{0x4001, 0x60},
	{0x4009, 0x05},
	{0x4020, 0x00},
	{0x4021, 0x00},
	{0x4022, 0x00},
	{0x4023, 0x00},
	{0x4024, 0x00},
	{0x4025, 0x00},
	{0x4026, 0x00},
	{0x4027, 0x00},
	{0x4028, 0x00},
	{0x4029, 0x00},
	{0x402a, 0x00},
	{0x402b, 0x00},
	{0x402c, 0x00},
	{0x402d, 0x00},
	{0x402e, 0x00},
	{0x402f, 0x00},
	{0x4040, 0x00},
	{0x4041, 0x03},
	{0x4042, 0x00},
	{0x4043, 0x7A},
	{0x4044, 0x00},
	{0x4045, 0x7A},
	{0x4046, 0x00},
	{0x4047, 0x7A},
	{0x4048, 0x00},
	{0x4049, 0x7A},
	{0x4307, 0x30},
	{0x4500, 0x58},
	{0x4501, 0x04},
	{0x4502, 0x48},
	{0x4503, 0x10},
	{0x4508, 0x55},
	{0x4509, 0x55},
	{0x450a, 0x00},
	{0x450b, 0x00},
	{0x4600, 0x00},
	{0x4601, 0x80},
	{0x4700, 0xa4},
	{0x4800, 0x4c},
	{0x4816, 0x53},
	{0x481f, 0x40},
	{0x4837, 0x13},
	{0x5000, 0x56},
	{0x5001, 0x01},
	{0x5002, 0x28},
	{0x5004, 0x0c},
	{0x5006, 0x0c},
	{0x5007, 0xe0},
	{0x5008, 0x01},
	{0x5009, 0xb0},
	{0x5901, 0x00},
	{0x5a01, 0x00},
	{0x5a03, 0x00},
	{0x5a04, 0x0c},
	{0x5a05, 0xe0},
	{0x5a06, 0x09},
	{0x5a07, 0xb0},
	{0x5a08, 0x06},
	{0x5e00, 0x00},
	{0x3734, 0x40},
	{0x5b00, 0x01},
	{0x5b01, 0x10},
	{0x5b02, 0x01},
	{0x5b03, 0xdb},
	{0x3d8c, 0x71},
	{0x3d8d, 0xea},
	{0x4017, 0x10},
	{0x3618, 0x2a},
	{0x5780, 0x3e},
	{0x5781, 0x0f},
	{0x5782, 0x44},
	{0x5783, 0x02},
	{0x5784, 0x01},
	{0x5785, 0x01},
	{0x5786, 0x00},
	{0x5787, 0x04},
	{0x5788, 0x02},
	{0x5789, 0x0f},
	{0x578a, 0xfd},
	{0x578b, 0xf5},
	{0x578c, 0xf5},
	{0x578d, 0x03},
	{0x578e, 0x08},
	{0x578f, 0x0c},
	{0x5790, 0x08},
	{0x5791, 0x06},
	{0x5792, 0x00},
	{0x5793, 0x52},
	{0x5794, 0xa3},
	{0x3503, 0x00},
	{0x5045, 0x05},
	{0x4003, 0x40},
	{0x5048, 0x40}
};

static const struct ov5670_reg mode_640x360_regs[] = {
	{0x3000, 0x00},
	{0x3002, 0x21},
	{0x3005, 0xf0},
	{0x3007, 0x00},
	{0x3015, 0x0f},
	{0x3018, 0x32},
	{0x301a, 0xf0},
	{0x301b, 0xf0},
	{0x301c, 0xf0},
	{0x301d, 0xf0},
	{0x301e, 0xf0},
	{0x3030, 0x00},
	{0x3031, 0x0a},
	{0x303c, 0xff},
	{0x303e, 0xff},
	{0x3040, 0xf0},
	{0x3041, 0x00},
	{0x3042, 0xf0},
	{0x3106, 0x11},
	{0x3500, 0x00},
	{0x3501, 0x80},
	{0x3502, 0x00},
	{0x3503, 0x04},
	{0x3504, 0x03},
	{0x3505, 0x83},
	{0x3508, 0x04},
	{0x3509, 0x00},
	{0x350e, 0x04},
	{0x350f, 0x00},
	{0x3510, 0x00},
	{0x3511, 0x02},
	{0x3512, 0x00},
	{0x3601, 0xc8},
	{0x3610, 0x88},
	{0x3612, 0x48},
	{0x3614, 0x5b},
	{0x3615, 0x96},
	{0x3621, 0xd0},
	{0x3622, 0x00},
	{0x3623, 0x04},
	{0x3633, 0x13},
	{0x3634, 0x13},
	{0x3635, 0x13},
	{0x3636, 0x13},
	{0x3645, 0x13},
	{0x3646, 0x82},
	{0x3650, 0x00},
	{0x3652, 0xff},
	{0x3655, 0x20},
	{0x3656, 0xff},
	{0x365a, 0xff},
	{0x365e, 0xff},
	{0x3668, 0x00},
	{0x366a, 0x07},
	{0x366e, 0x08},
	{0x366d, 0x00},
	{0x366f, 0x80},
	{0x3700, 0x28},
	{0x3701, 0x10},
	{0x3702, 0x3a},
	{0x3703, 0x19},
	{0x3704, 0x10},
	{0x3705, 0x00},
	{0x3706, 0x66},
	{0x3707, 0x08},
	{0x3708, 0x34},
	{0x3709, 0x40},
	{0x370a, 0x01},
	{0x370b, 0x1b},
	{0x3714, 0x24},
	{0x371a, 0x3e},
	{0x3733, 0x00},
	{0x3734, 0x00},
	{0x373a, 0x05},
	{0x373b, 0x06},
	{0x373c, 0x0a},
	{0x373f, 0xa0},
	{0x3755, 0x00},
	{0x3758, 0x00},
	{0x375b, 0x0e},
	{0x3766, 0x5f},
	{0x3768, 0x00},
	{0x3769, 0x22},
	{0x3773, 0x08},
	{0x3774, 0x1f},
	{0x3776, 0x06},
	{0x37a0, 0x88},
	{0x37a1, 0x5c},
	{0x37a7, 0x88},
	{0x37a8, 0x70},
	{0x37aa, 0x88},
	{0x37ab, 0x48},
	{0x37b3, 0x66},
	{0x37c2, 0x04},
	{0x37c5, 0x00},
	{0x37c8, 0x00},
	{0x3800, 0x00},
	{0x3801, 0x0c},
	{0x3802, 0x00},
	{0x3803, 0x04},
	{0x3804, 0x0a},
	{0x3805, 0x33},
	{0x3806, 0x07},
	{0x3807, 0xa3},
	{0x3808, 0x02},
	{0x3809, 0x80},
	{0x380a, 0x01},
	{0x380b, 0x68},
	{0x380c, 0x06},
	{0x380d, 0x90},
	{0x380e, 0x08},
	{0x380f, 0x08},
	{0x3811, 0x04},
	{0x3813, 0x02},
	{0x3814, 0x07},
	{0x3815, 0x01},
	{0x3816, 0x00},
	{0x3817, 0x00},
	{0x3818, 0x00},
	{0x3819, 0x00},
	{0x3820, 0x94},
	{0x3821, 0xc6},
	{0x3822, 0x48},
	{0x3826, 0x00},
	{0x3827, 0x08},
	{0x382a, 0x07},
	{0x382b, 0x01},
	{0x3830, 0x08},
	{0x3836, 0x02},
	{0x3837, 0x00},
	{0x3838, 0x10},
	{0x3841, 0xff},
	{0x3846, 0x48},
	{0x3861, 0x00},
	{0x3862, 0x04},
	{0x3863, 0x06},
	{0x3a11, 0x01},
	{0x3a12, 0x78},
	{0x3b00, 0x00},
	{0x3b02, 0x00},
	{0x3b03, 0x00},
	{0x3b04, 0x00},
	{0x3b05, 0x00},
	{0x3c00, 0x89},
	{0x3c01, 0xab},
	{0x3c02, 0x01},
	{0x3c03, 0x00},
	{0x3c04, 0x00},
	{0x3c05, 0x03},
	{0x3c06, 0x00},
	{0x3c07, 0x05},
	{0x3c0c, 0x00},
	{0x3c0d, 0x00},
	{0x3c0e, 0x00},
	{0x3c0f, 0x00},
	{0x3c40, 0x00},
	{0x3c41, 0xa3},
	{0x3c43, 0x7d},
	{0x3c45, 0xd7},
	{0x3c47, 0xfc},
	{0x3c50, 0x05},
	{0x3c52, 0xaa},
	{0x3c54, 0x71},
	{0x3c56, 0x80},
	{0x3d85, 0x17},
	{0x3f03, 0x00},
	{0x3f0a, 0x00},
	{0x3f0b, 0x00},
	{0x4001, 0x60},
	{0x4009, 0x05},
	{0x4020, 0x00},
	{0x4021, 0x00},
	{0x4022, 0x00},
	{0x4023, 0x00},
	{0x4024, 0x00},
	{0x4025, 0x00},
	{0x4026, 0x00},
	{0x4027, 0x00},
	{0x4028, 0x00},
	{0x4029, 0x00},
	{0x402a, 0x00},
	{0x402b, 0x00},
	{0x402c, 0x00},
	{0x402d, 0x00},
	{0x402e, 0x00},
	{0x402f, 0x00},
	{0x4040, 0x00},
	{0x4041, 0x03},
	{0x4042, 0x00},
	{0x4043, 0x7A},
	{0x4044, 0x00},
	{0x4045, 0x7A},
	{0x4046, 0x00},
	{0x4047, 0x7A},
	{0x4048, 0x00},
	{0x4049, 0x7A},
	{0x4307, 0x30},
	{0x4500, 0x58},
	{0x4501, 0x04},
	{0x4502, 0x40},
	{0x4503, 0x10},
	{0x4508, 0x55},
	{0x4509, 0x55},
	{0x450a, 0x02},
	{0x450b, 0x00},
	{0x4600, 0x00},
	{0x4601, 0x40},
	{0x4700, 0xa4},
	{0x4800, 0x4c},
	{0x4816, 0x53},
	{0x481f, 0x40},
	{0x4837, 0x13},
	{0x5000, 0x56},
	{0x5001, 0x01},
	{0x5002, 0x28},
	{0x5004, 0x0c},
	{0x5006, 0x0c},
	{0x5007, 0xe0},
	{0x5008, 0x01},
	{0x5009, 0xb0},
	{0x5901, 0x00},
	{0x5a01, 0x00},
	{0x5a03, 0x00},
	{0x5a04, 0x0c},
	{0x5a05, 0xe0},
	{0x5a06, 0x09},
	{0x5a07, 0xb0},
	{0x5a08, 0x06},
	{0x5e00, 0x00},
	{0x3734, 0x40},
	{0x5b00, 0x01},
	{0x5b01, 0x10},
	{0x5b02, 0x01},
	{0x5b03, 0xdb},
	{0x3d8c, 0x71},
	{0x3d8d, 0xea},
	{0x4017, 0x10},
	{0x3618, 0x2a},
	{0x5780, 0x3e},
	{0x5781, 0x0f},
	{0x5782, 0x44},
	{0x5783, 0x02},
	{0x5784, 0x01},
	{0x5785, 0x01},
	{0x5786, 0x00},
	{0x5787, 0x04},
	{0x5788, 0x02},
	{0x5789, 0x0f},
	{0x578a, 0xfd},
	{0x578b, 0xf5},
	{0x578c, 0xf5},
	{0x578d, 0x03},
	{0x578e, 0x08},
	{0x578f, 0x0c},
	{0x5790, 0x08},
	{0x5791, 0x06},
	{0x5792, 0x00},
	{0x5793, 0x52},
	{0x5794, 0xa3},
	{0x3503, 0x00},
	{0x5045, 0x05},
	{0x4003, 0x40},
	{0x5048, 0x40}
};

static const char * const ov5670_test_pattern_menu[] = {
	"Disabled",
	"Vertical Color Bar Type 1",
};

/* Supported link frequencies */
#define OV5670_LINK_FREQ_422MHZ		422400000
#define OV5670_LINK_FREQ_422MHZ_INDEX	0
static const struct ov5670_link_freq_config link_freq_configs[] = {
	{
		/* pixel_rate = link_freq * 2 * nr_of_lanes / bits_per_sample */
		.pixel_rate = (OV5670_LINK_FREQ_422MHZ * 2 * 2) / 10,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mipi_data_rate_840mbps),
			.regs = mipi_data_rate_840mbps,
		}
	}
};

static const s64 link_freq_menu_items[] = {
	OV5670_LINK_FREQ_422MHZ
};

/*
 * OV5670 sensor supports following resolutions with full FOV:
 * 4:3  ==> {2592x1944, 1296x972, 648x486}
 * 16:9 ==> {2560x1440, 1280x720, 640x360}
 */
static const struct ov5670_mode supported_modes[] = {
	{
		.width = 2592,
		.height = 1944,
		.vts_def = OV5670_VTS_30FPS,
		.vts_min = OV5670_VTS_30FPS,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_2592x1944_regs),
			.regs = mode_2592x1944_regs,
		},
		.link_freq_index = OV5670_LINK_FREQ_422MHZ_INDEX,
	},
	{
		.width = 1296,
		.height = 972,
		.vts_def = OV5670_VTS_30FPS,
		.vts_min = 996,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_1296x972_regs),
			.regs = mode_1296x972_regs,
		},
		.link_freq_index = OV5670_LINK_FREQ_422MHZ_INDEX,
	},
	{
		.width = 648,
		.height = 486,
		.vts_def = OV5670_VTS_30FPS,
		.vts_min = 516,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_648x486_regs),
			.regs = mode_648x486_regs,
		},
		.link_freq_index = OV5670_LINK_FREQ_422MHZ_INDEX,
	},
	{
		.width = 2560,
		.height = 1440,
		.vts_def = OV5670_VTS_30FPS,
		.vts_min = OV5670_VTS_30FPS,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_2560x1440_regs),
			.regs = mode_2560x1440_regs,
		},
		.link_freq_index = OV5670_LINK_FREQ_422MHZ_INDEX,
	},
	{
		.width = 1280,
		.height = 720,
		.vts_def = OV5670_VTS_30FPS,
		.vts_min = 1020,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_1280x720_regs),
			.regs = mode_1280x720_regs,
		},
		.link_freq_index = OV5670_LINK_FREQ_422MHZ_INDEX,
	},
	{
		.width = 640,
		.height = 360,
		.vts_def = OV5670_VTS_30FPS,
		.vts_min = 510,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_640x360_regs),
			.regs = mode_640x360_regs,
		},
		.link_freq_index = OV5670_LINK_FREQ_422MHZ_INDEX,
	}
};

/* GPIOs provided by tps68470-gpio */
static struct gpiod_lookup_table ov5670_pmic_gpios = {
	.dev_id = "i2c-INT3479:00",
	.table = {
		/* TODO: Not sure what exactly are needed. Just add all. */
		GPIO_LOOKUP_IDX("tps68470-gpio", 0, "gpio.0", 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX("tps68470-gpio", 1, "gpio.1", 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX("tps68470-gpio", 2, "gpio.2", 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX("tps68470-gpio", 3, "gpio.3", 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX("tps68470-gpio", 4, "gpio.4", 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX("tps68470-gpio", 5, "gpio.5", 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX("tps68470-gpio", 6, "gpio.6", 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX("tps68470-gpio", 7, "s_enable", 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX("tps68470-gpio", 8, "s_idle", 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX("tps68470-gpio", 9, "s_resetn", 0, GPIO_ACTIVE_HIGH),
		{ },
	},
};

static const char * const ov5670_supply_names[] = {
	/* dummy regulators */
	// "dovdd",	/* Digital I/O power */
	// "avdd",		/* Analog power */
	// "dvdd",		/* Digital core power */

	/* regulators provided by tps68470-regulator */
	"CORE",
	"ANA",
	"VCM",
	"VIO",
	"VSIO",
	"AUX1",
	"AUX2",
};
#define OV5670_NUM_SUPPLIES ARRAY_SIZE(ov5670_supply_names)

struct ov5670 {
	struct v4l2_subdev sd;
	struct media_pad pad;

	struct v4l2_ctrl_handler ctrl_handler;
	/* V4L2 Controls */
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *exposure;

	/* Current mode */
	const struct ov5670_mode *cur_mode;

	/* To serialize asynchronus callbacks */
	struct mutex mutex;

	/* Streaming on/off */
	bool streaming;

	/* dependent device (PMIC) */
	struct device *dep_dev;

	/* GPIOs defined in dep_dev _CRS. Surface devices have three gpio pins.
	 * TODO: do not hard-code that there are three, for the other devices? */
	struct gpio_desc *xshutdn;
	struct gpio_desc *pwdnb;
	struct gpio_desc *led_gpio;

	/* GPIOs provided by tps68470-gpio */
	/* TODO: Not sure what exactly are needed. Just add all. */
	struct gpio_desc *gpio0;
	struct gpio_desc *gpio1;
	struct gpio_desc *gpio2;
	struct gpio_desc *gpio3;
	struct gpio_desc *gpio4;
	struct gpio_desc *gpio5;
	struct gpio_desc *gpio6;
	struct gpio_desc *s_enable;
	struct gpio_desc *s_idle;
	struct gpio_desc *s_resetn;

	struct regulator_bulk_data supplies[OV5670_NUM_SUPPLIES];
	bool regulator_enabled;

	struct clk *xvclk;
	u32 xvclk_freq;
	bool clk_enabled;
};

#define to_ov5670(_sd)	container_of(_sd, struct ov5670, sd)

/* Read registers up to 4 at a time */
static int ov5670_read_reg(struct ov5670 *ov5670, u16 reg, unsigned int len,
			   u32 *val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov5670->sd);
	struct i2c_msg msgs[2];
	u8 *data_be_p;
	__be32 data_be = 0;
	__be16 reg_addr_be = cpu_to_be16(reg);
	int ret;

	if (len > 4)
		return -EINVAL;

	data_be_p = (u8 *)&data_be;
	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = 2;
	msgs[0].buf = (u8 *)&reg_addr_be;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = &data_be_p[4 - len];

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*val = be32_to_cpu(data_be);

	return 0;
}

/* Write registers up to 4 at a time */
static int ov5670_write_reg(struct ov5670 *ov5670, u16 reg, unsigned int len,
			    u32 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov5670->sd);
	int buf_i;
	int val_i;
	u8 buf[6];
	u8 *val_p;
	__be32 tmp;

	if (len > 4)
		return -EINVAL;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;

	tmp = cpu_to_be32(val);
	val_p = (u8 *)&tmp;
	buf_i = 2;
	val_i = 4 - len;

	while (val_i < 4)
		buf[buf_i++] = val_p[val_i++];

	if (i2c_master_send(client, buf, len + 2) != len + 2)
		return -EIO;

	return 0;
}

/* Write a list of registers */
static int ov5670_write_regs(struct ov5670 *ov5670,
			     const struct ov5670_reg *regs, unsigned int len)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov5670->sd);
	unsigned int i;
	int ret;

	for (i = 0; i < len; i++) {
		ret = ov5670_write_reg(ov5670, regs[i].address, 1, regs[i].val);
		if (ret) {
			dev_err_ratelimited(
				&client->dev,
				"Failed to write reg 0x%4.4x. error = %d\n",
				regs[i].address, ret);

			return ret;
		}
	}

	return 0;
}

static int ov5670_write_reg_list(struct ov5670 *ov5670,
				 const struct ov5670_reg_list *r_list)
{
	return ov5670_write_regs(ov5670, r_list->regs, r_list->num_of_regs);
}

/* Open sub-device */
static int ov5670_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct ov5670 *ov5670 = to_ov5670(sd);
	struct v4l2_mbus_framefmt *try_fmt =
				v4l2_subdev_get_try_format(sd, fh->pad, 0);

	mutex_lock(&ov5670->mutex);

	/* Initialize try_fmt */
	try_fmt->width = ov5670->cur_mode->width;
	try_fmt->height = ov5670->cur_mode->height;
	try_fmt->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	try_fmt->field = V4L2_FIELD_NONE;

	/* No crop or compose */
	mutex_unlock(&ov5670->mutex);

	return 0;
}

static int ov5670_update_digital_gain(struct ov5670 *ov5670, u32 d_gain)
{
	int ret;

	ret = ov5670_write_reg(ov5670, OV5670_REG_R_DGTL_GAIN,
			       OV5670_REG_VALUE_16BIT, d_gain);
	if (ret)
		return ret;

	ret = ov5670_write_reg(ov5670, OV5670_REG_G_DGTL_GAIN,
			       OV5670_REG_VALUE_16BIT, d_gain);
	if (ret)
		return ret;

	return ov5670_write_reg(ov5670, OV5670_REG_B_DGTL_GAIN,
				OV5670_REG_VALUE_16BIT, d_gain);
}

static int ov5670_enable_test_pattern(struct ov5670 *ov5670, u32 pattern)
{
	u32 val;
	int ret;

	/* Set the bayer order that we support */
	ret = ov5670_write_reg(ov5670, OV5670_REG_TEST_PATTERN_CTRL,
			       OV5670_REG_VALUE_08BIT, 0);
	if (ret)
		return ret;

	ret = ov5670_read_reg(ov5670, OV5670_REG_TEST_PATTERN,
			      OV5670_REG_VALUE_08BIT, &val);
	if (ret)
		return ret;

	if (pattern)
		val |= OV5670_TEST_PATTERN_ENABLE;
	else
		val &= ~OV5670_TEST_PATTERN_ENABLE;

	return ov5670_write_reg(ov5670, OV5670_REG_TEST_PATTERN,
				OV5670_REG_VALUE_08BIT, val);
}

/* Initialize control handlers */
static int ov5670_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov5670 *ov5670 = container_of(ctrl->handler,
					     struct ov5670, ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&ov5670->sd);
	s64 max;
	int ret = 0;

	/* Propagate change of current control to all related controls */
	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		/* Update max exposure while meeting expected vblanking */
		max = ov5670->cur_mode->height + ctrl->val - 8;
		__v4l2_ctrl_modify_range(ov5670->exposure,
					 ov5670->exposure->minimum, max,
					 ov5670->exposure->step, max);
		break;
	}

	/* V4L2 controls values will be applied only when power is already up */
	if (!pm_runtime_get_if_in_use(&client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = ov5670_write_reg(ov5670, OV5670_REG_ANALOG_GAIN,
				       OV5670_REG_VALUE_16BIT, ctrl->val);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ret = ov5670_update_digital_gain(ov5670, ctrl->val);
		break;
	case V4L2_CID_EXPOSURE:
		/* 4 least significant bits of expsoure are fractional part */
		ret = ov5670_write_reg(ov5670, OV5670_REG_EXPOSURE,
				       OV5670_REG_VALUE_24BIT, ctrl->val << 4);
		break;
	case V4L2_CID_VBLANK:
		/* Update VTS that meets expected vertical blanking */
		ret = ov5670_write_reg(ov5670, OV5670_REG_VTS,
				       OV5670_REG_VALUE_16BIT,
				       ov5670->cur_mode->height + ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = ov5670_enable_test_pattern(ov5670, ctrl->val);
		break;
	default:
		dev_info(&client->dev, "%s Unhandled id:0x%x, val:0x%x\n",
			 __func__, ctrl->id, ctrl->val);
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops ov5670_ctrl_ops = {
	.s_ctrl = ov5670_set_ctrl,
};

/* Get GPIOs defined in dep_dev _CRS */
static int gpio_crs_get(struct ov5670 *ov5670)
{
	struct device *dep_dev = ov5670->dep_dev;

	ov5670->xshutdn = devm_gpiod_get_index(dep_dev, NULL, 0, GPIOD_ASIS);
	if (IS_ERR(ov5670->xshutdn)) {
		dev_err(dep_dev, "Couldn't get GPIO XSHUTDN\n");
		return -EINVAL;
	}

	ov5670->pwdnb = devm_gpiod_get_index(dep_dev, NULL, 1, GPIOD_ASIS);
	if (IS_ERR(ov5670->pwdnb)) {
		dev_err(dep_dev, "Couldn't get GPIO PWDNB\n");
		return -EINVAL;
	}

	ov5670->led_gpio = devm_gpiod_get_index(dep_dev, NULL, 2, GPIOD_ASIS);
	if (IS_ERR(ov5670->led_gpio)) {
		dev_err(dep_dev, "Couldn't get GPIO LED\n");
		return -EINVAL;
	}

	return 0;
}

/* Put GPIOs defined in dep_dev _CRS */
static void gpio_crs_put(struct ov5670 *ov5670)
{
	gpiod_put(ov5670->xshutdn);
	gpiod_put(ov5670->pwdnb);
	gpiod_put(ov5670->led_gpio);
}

/* Controls GPIOs defined in dep_dev _CRS */
static int gpio_crs_ctrl(struct v4l2_subdev *sd, bool flag)
{
	struct ov5670 *ov5670 = to_ov5670(sd);

	gpiod_set_value_cansleep(ov5670->xshutdn, flag);
	gpiod_set_value_cansleep(ov5670->pwdnb, flag);
	gpiod_set_value_cansleep(ov5670->led_gpio, flag);

	return 0;
}

/* Get GPIOs provided by tps68470-gpio */
static int gpio_pmic_get(struct ov5670 *ov5670)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov5670->sd);

	gpiod_add_lookup_table(&ov5670_pmic_gpios);

	/* TODO_1: nicer way to get all GPIOs */
	/* TODO_2: Not sure what exactly are needed. Just add all. */
	ov5670->gpio0 = devm_gpiod_get_index(&client->dev, "gpio.0", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ov5670->gpio0)) {
		dev_err(&client->dev, "Error fetching gpio.0.\n");
		goto fail;
	}
	ov5670->gpio1 = devm_gpiod_get_index(&client->dev, "gpio.1", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ov5670->gpio1)) {
		dev_err(&client->dev, "Error fetching gpio.1.\n");
		goto fail;
	}
	ov5670->gpio2 = devm_gpiod_get_index(&client->dev, "gpio.2", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ov5670->gpio2)) {
		dev_err(&client->dev, "Error fetching gpio.2.\n");
		goto fail;
	}
	ov5670->gpio3 = devm_gpiod_get_index(&client->dev, "gpio.3", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ov5670->gpio3)) {
		dev_err(&client->dev, "Error fetching gpio.3.\n");
		goto fail;
	}
	ov5670->gpio4 = devm_gpiod_get_index(&client->dev, "gpio.4", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ov5670->gpio4)) {
		dev_err(&client->dev, "Error fetching gpio.4.\n");
		goto fail;
	}
	ov5670->gpio5 = devm_gpiod_get_index(&client->dev, "gpio.5", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ov5670->gpio5)) {
		dev_err(&client->dev, "Error fetching gpio.5.\n");
		goto fail;
	}
	ov5670->gpio6 = devm_gpiod_get_index(&client->dev, "gpio.6", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ov5670->gpio6)) {
		dev_err(&client->dev, "Error fetching gpio.6.\n");
		goto fail;
	}
	ov5670->s_enable = devm_gpiod_get_index(&client->dev, "s_enable", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ov5670->s_enable)) {
		dev_err(&client->dev, "Error fetching s_enable.\n");
		goto fail;
	}
	ov5670->s_idle = devm_gpiod_get_index(&client->dev, "s_idle", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ov5670->s_idle)) {
		dev_err(&client->dev, "Error fetching s_idle.\n");
		goto fail;
	}
	ov5670->s_resetn = devm_gpiod_get_index(&client->dev, "s_resetn", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ov5670->s_resetn)) {
		dev_err(&client->dev, "Error fetching s_resetn.\n");
		goto fail;
	}

	return 0;

fail:
	gpiod_remove_lookup_table(&ov5670_pmic_gpios);
	return -EINVAL;
}

static void gpio_pmic_put(struct ov5670 *ov5670)
{
	gpiod_put(ov5670->gpio0);
	gpiod_put(ov5670->gpio1);
	gpiod_put(ov5670->gpio2);
	gpiod_put(ov5670->gpio3);
	gpiod_put(ov5670->gpio4);
	gpiod_put(ov5670->gpio5);
	gpiod_put(ov5670->gpio6);
	gpiod_put(ov5670->s_enable);
	gpiod_put(ov5670->s_idle);
	gpiod_put(ov5670->s_resetn);

	gpiod_remove_lookup_table(&ov5670_pmic_gpios);
}

/* Controls GPIOs provided by tps68470-gpio */
static int gpio_pmic_ctrl(struct v4l2_subdev *sd, bool flag)
{
	struct ov5670 *ov5670 = to_ov5670(sd);

	gpiod_set_value_cansleep(ov5670->gpio0, flag);
	gpiod_set_value_cansleep(ov5670->gpio1, flag);
	gpiod_set_value_cansleep(ov5670->gpio2, flag);
	gpiod_set_value_cansleep(ov5670->gpio3, flag);
	gpiod_set_value_cansleep(ov5670->gpio4, flag);
	gpiod_set_value_cansleep(ov5670->gpio5, flag);
	gpiod_set_value_cansleep(ov5670->gpio6, flag);
	gpiod_set_value_cansleep(ov5670->s_enable, flag);
	gpiod_set_value_cansleep(ov5670->s_idle, flag);
	gpiod_set_value_cansleep(ov5670->s_resetn, flag);

	return 0;
}

/* Get regulators provided by tps68470-regulator */
static int regulator_pmic_get(struct ov5670 *ov5670)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov5670->sd);
	int i;

	for (i = 0; i < OV5670_NUM_SUPPLIES; i++)
		ov5670->supplies[i].supply = ov5670_supply_names[i];

	return devm_regulator_bulk_get(&client->dev,
				       OV5670_NUM_SUPPLIES,
				       ov5670->supplies);
}

/* Configure clock provided by tps68470-clk */
static int ov5670_configure_clock(struct ov5670 *ov5670)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov5670->sd);
	u32 current_freq;
	int ret;

	ov5670->xvclk = devm_clk_get(&client->dev, "tps68470-clk");
	if (IS_ERR(ov5670->xvclk)) {
		dev_err(&client->dev, "xvclk clock missing or invalid.\n");
		return PTR_ERR(ov5670->xvclk);
	}

	/* TODO: get this value from SSDB */
	ov5670->xvclk_freq = 19200000;

	ret = clk_set_rate(ov5670->xvclk, ov5670->xvclk_freq);
	if (ret < 0) {
		dev_err(&client->dev, "Error setting xvclk rate.\n");
		return -EINVAL;
	}

	current_freq = clk_get_rate(ov5670->xvclk);
	if (current_freq != ov5670->xvclk_freq) {
		dev_err(&client->dev, "Couldn't set xvclk freq to %d Hz, "
				 "current freq: %d Hz\n",
				 ov5670->xvclk_freq, current_freq);
		return -EINVAL;
	}

	return 0;
}

static int power_ctrl(struct v4l2_subdev *sd, bool flag)
{
	struct ov5670 *ov5670 = to_ov5670(sd);
	int ret;

	/* turn on */
	if (flag) {
		ret = gpio_crs_ctrl(sd, flag);
		ret = gpio_pmic_ctrl(sd, flag);
		ret = regulator_bulk_enable(OV5670_NUM_SUPPLIES, ov5670->supplies);
		ov5670->regulator_enabled = true;
		ret = clk_prepare_enable(ov5670->xvclk);
		ov5670->clk_enabled = true;
	}

	/* turn off in reverse order */
	if (ov5670->clk_enabled) {
		clk_disable_unprepare(ov5670->xvclk);
		ov5670->clk_enabled = false;
	}
	if (ov5670->regulator_enabled) {
		ret = regulator_bulk_disable(OV5670_NUM_SUPPLIES, ov5670->supplies);
		ov5670->regulator_enabled = false;
	}
	ret = gpio_pmic_ctrl(sd, flag);
	ret = gpio_crs_ctrl(sd, flag);

	return ret;
}

static int gpio_ctrl(struct v4l2_subdev *sd, bool flag)
{

	/* TODO: enable/disable GPIO here */

	return 0;
}

static int __power_up(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	/* power control */
	ret = power_ctrl(sd, 1);
	if (ret)
		goto fail_power;

	/* add some delay (10~11ms) */
	usleep_range(10000, 11000);

	/* gpio ctrl */
	ret = gpio_ctrl(sd, 1);
	if (ret) {
		ret = gpio_ctrl(sd, 1);
		if (ret)
			goto fail_power;
	}

	/* add some delay (30~31ms) */
	usleep_range(30000, 31000);

	return 0;

fail_power:
	power_ctrl(sd, 0);
	dev_err(&client->dev, "sensor power-up failed\n");

	return ret;
}

static int power_down(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	/* gpio ctrl */
	ret = gpio_ctrl(sd, 0);
	if (ret) {
		ret = gpio_ctrl(sd, 0);
		if (ret)
			dev_err(&client->dev, "gpio failed 2\n");
	}

	/* power control */
	ret = power_ctrl(sd, 0);
	if (ret)
		dev_err(&client->dev, "vprog failed.\n");

	return ret;
}

static int power_up(struct v4l2_subdev *sd)
{
	static const int retry_count = 4;
	int i, ret;

	for (i = 0; i < retry_count; i++) {
		ret = __power_up(sd);
		if (!ret)
			return 0;

		power_down(sd);
	}
	return ret;
}

static int ov5670_s_power(struct v4l2_subdev *sd, int on)
{
	int ret;

	pr_info("%s: on %d\n", __func__, on);
	if (on == 0)
		return power_down(sd);

	/* on != 0 */
	ret = power_up(sd);
	return ret;
}

/* Initialize control handlers */
static int ov5670_init_controls(struct ov5670 *ov5670)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov5670->sd);
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl_handler *ctrl_hdlr;
	s64 vblank_max;
	s64 vblank_def;
	s64 vblank_min;
	s64 exposure_max;
	int ret;

	ctrl_hdlr = &ov5670->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 10);
	if (ret)
		return ret;

	ctrl_hdlr->lock = &ov5670->mutex;
	ov5670->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr,
						   &ov5670_ctrl_ops,
						   V4L2_CID_LINK_FREQ,
						   0, 0, link_freq_menu_items);
	if (ov5670->link_freq)
		ov5670->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/* By default, V4L2_CID_PIXEL_RATE is read only */
	ov5670->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &ov5670_ctrl_ops,
					       V4L2_CID_PIXEL_RATE, 0,
					       link_freq_configs[0].pixel_rate,
					       1,
					       link_freq_configs[0].pixel_rate);

	vblank_max = OV5670_VTS_MAX - ov5670->cur_mode->height;
	vblank_def = ov5670->cur_mode->vts_def - ov5670->cur_mode->height;
	vblank_min = ov5670->cur_mode->vts_min - ov5670->cur_mode->height;
	ov5670->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &ov5670_ctrl_ops,
					   V4L2_CID_VBLANK, vblank_min,
					   vblank_max, 1, vblank_def);

	ov5670->hblank = v4l2_ctrl_new_std(
				ctrl_hdlr, &ov5670_ctrl_ops, V4L2_CID_HBLANK,
				OV5670_FIXED_PPL - ov5670->cur_mode->width,
				OV5670_FIXED_PPL - ov5670->cur_mode->width, 1,
				OV5670_FIXED_PPL - ov5670->cur_mode->width);
	if (ov5670->hblank)
		ov5670->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/* Get min, max, step, default from sensor */
	v4l2_ctrl_new_std(ctrl_hdlr, &ov5670_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  ANALOG_GAIN_MIN, ANALOG_GAIN_MAX, ANALOG_GAIN_STEP,
			  ANALOG_GAIN_DEFAULT);

	/* Digital gain */
	v4l2_ctrl_new_std(ctrl_hdlr, &ov5670_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  OV5670_DGTL_GAIN_MIN, OV5670_DGTL_GAIN_MAX,
			  OV5670_DGTL_GAIN_STEP, OV5670_DGTL_GAIN_DEFAULT);

	/* Get min, max, step, default from sensor */
	exposure_max = ov5670->cur_mode->vts_def - 8;
	ov5670->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &ov5670_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     OV5670_EXPOSURE_MIN,
					     exposure_max, OV5670_EXPOSURE_STEP,
					     exposure_max);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &ov5670_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(ov5670_test_pattern_menu) - 1,
				     0, 0, ov5670_test_pattern_menu);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		goto error;
	}

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (ret)
		goto error;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &ov5670_ctrl_ops,
					      &props);
	if (ret)
		goto error;

	ov5670->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static int ov5670_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	/* Only one bayer order GRBG is supported */
	if (code->index > 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SGRBG10_1X10;

	return 0;
}

static int ov5670_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != MEDIA_BUS_FMT_SGRBG10_1X10)
		return -EINVAL;

	fse->min_width = supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static void ov5670_update_pad_format(const struct ov5670_mode *mode,
				     struct v4l2_subdev_format *fmt)
{
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.code = MEDIA_BUS_FMT_SGRBG10_1X10;
	fmt->format.field = V4L2_FIELD_NONE;
}

static int ov5670_do_get_pad_format(struct ov5670 *ov5670,
				    struct v4l2_subdev_pad_config *cfg,
				    struct v4l2_subdev_format *fmt)
{
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		fmt->format = *v4l2_subdev_get_try_format(&ov5670->sd, cfg,
							  fmt->pad);
	else
		ov5670_update_pad_format(ov5670->cur_mode, fmt);

	return 0;
}

static int ov5670_get_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_format *fmt)
{
	struct ov5670 *ov5670 = to_ov5670(sd);
	int ret;

	mutex_lock(&ov5670->mutex);
	ret = ov5670_do_get_pad_format(ov5670, cfg, fmt);
	mutex_unlock(&ov5670->mutex);

	return ret;
}

static int ov5670_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_format *fmt)
{
	struct ov5670 *ov5670 = to_ov5670(sd);
	const struct ov5670_mode *mode;
	s32 vblank_def;
	s32 h_blank;

	mutex_lock(&ov5670->mutex);

	fmt->format.code = MEDIA_BUS_FMT_SGRBG10_1X10;

	mode = v4l2_find_nearest_size(supported_modes,
				      ARRAY_SIZE(supported_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);
	ov5670_update_pad_format(mode, fmt);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		*v4l2_subdev_get_try_format(sd, cfg, fmt->pad) = fmt->format;
	} else {
		ov5670->cur_mode = mode;
		__v4l2_ctrl_s_ctrl(ov5670->link_freq, mode->link_freq_index);
		__v4l2_ctrl_s_ctrl_int64(
			ov5670->pixel_rate,
			link_freq_configs[mode->link_freq_index].pixel_rate);
		/* Update limits and set FPS to default */
		vblank_def = ov5670->cur_mode->vts_def -
			     ov5670->cur_mode->height;
		__v4l2_ctrl_modify_range(
			ov5670->vblank,
			ov5670->cur_mode->vts_min - ov5670->cur_mode->height,
			OV5670_VTS_MAX - ov5670->cur_mode->height, 1,
			vblank_def);
		__v4l2_ctrl_s_ctrl(ov5670->vblank, vblank_def);
		h_blank = OV5670_FIXED_PPL - ov5670->cur_mode->width;
		__v4l2_ctrl_modify_range(ov5670->hblank, h_blank, h_blank, 1,
					 h_blank);
	}

	mutex_unlock(&ov5670->mutex);

	return 0;
}

static int ov5670_get_skip_frames(struct v4l2_subdev *sd, u32 *frames)
{
	*frames = OV5670_NUM_OF_SKIP_FRAMES;

	return 0;
}

/* Prepare streaming by writing default values and customized values */
static int ov5670_start_streaming(struct ov5670 *ov5670)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov5670->sd);
	const struct ov5670_reg_list *reg_list;
	int link_freq_index;
	int ret;

	/* Get out of from software reset */
	ret = ov5670_write_reg(ov5670, OV5670_REG_SOFTWARE_RST,
			       OV5670_REG_VALUE_08BIT, OV5670_SOFTWARE_RST);
	if (ret) {
		dev_err(&client->dev, "%s failed to set powerup registers\n",
			__func__);
		return ret;
	}

	/* Setup PLL */
	link_freq_index = ov5670->cur_mode->link_freq_index;
	reg_list = &link_freq_configs[link_freq_index].reg_list;
	ret = ov5670_write_reg_list(ov5670, reg_list);
	if (ret) {
		dev_err(&client->dev, "%s failed to set plls\n", __func__);
		return ret;
	}

	/* Apply default values of current mode */
	reg_list = &ov5670->cur_mode->reg_list;
	ret = ov5670_write_reg_list(ov5670, reg_list);
	if (ret) {
		dev_err(&client->dev, "%s failed to set mode\n", __func__);
		return ret;
	}

	ret = __v4l2_ctrl_handler_setup(ov5670->sd.ctrl_handler);
	if (ret)
		return ret;

	/* Write stream on list */
	ret = ov5670_write_reg(ov5670, OV5670_REG_MODE_SELECT,
			       OV5670_REG_VALUE_08BIT, OV5670_MODE_STREAMING);
	if (ret) {
		dev_err(&client->dev, "%s failed to set stream\n", __func__);
		return ret;
	}

	return 0;
}

static int ov5670_stop_streaming(struct ov5670 *ov5670)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov5670->sd);
	int ret;

	ret = ov5670_write_reg(ov5670, OV5670_REG_MODE_SELECT,
			       OV5670_REG_VALUE_08BIT, OV5670_MODE_STANDBY);
	if (ret)
		dev_err(&client->dev, "%s failed to set stream\n", __func__);

	/* Return success even if it was an error, as there is nothing the
	 * caller can do about it.
	 */
	return 0;
}

static int ov5670_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct ov5670 *ov5670 = to_ov5670(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	mutex_lock(&ov5670->mutex);
	if (ov5670->streaming == enable)
		goto unlock_and_return;

	if (enable) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		ret = ov5670_start_streaming(ov5670);
		if (ret)
			goto error;
	} else {
		ret = ov5670_stop_streaming(ov5670);
		pm_runtime_put(&client->dev);
	}
	ov5670->streaming = enable;
	goto unlock_and_return;

error:
	pm_runtime_put(&client->dev);

unlock_and_return:
	mutex_unlock(&ov5670->mutex);

	return ret;
}

static int __maybe_unused ov5670_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov5670 *ov5670 = to_ov5670(sd);

	if (ov5670->streaming)
		ov5670_stop_streaming(ov5670);

	return 0;
}

static int __maybe_unused ov5670_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov5670 *ov5670 = to_ov5670(sd);
	int ret;

	if (ov5670->streaming) {
		ret = ov5670_start_streaming(ov5670);
		if (ret) {
			ov5670_stop_streaming(ov5670);
			return ret;
		}
	}

	return 0;
}

/* Verify chip ID */
static int ov5670_identify_module(struct ov5670 *ov5670)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov5670->sd);
	int ret;
	u32 val;

	ret = ov5670_read_reg(ov5670, OV5670_REG_CHIP_ID,
			      OV5670_REG_VALUE_24BIT, &val);
	if (ret)
		return ret;

	if (val != OV5670_CHIP_ID) {
		dev_err(&client->dev, "chip id mismatch: %x!=%x\n",
			OV5670_CHIP_ID, val);
		return -ENXIO;
	}

	return 0;
}

static const struct v4l2_subdev_video_ops ov5670_video_ops = {
	.s_stream = ov5670_set_stream,
};

static const struct v4l2_subdev_core_ops ov5670_core_ops = {
	.s_power = ov5670_s_power,
};

static const struct v4l2_subdev_pad_ops ov5670_pad_ops = {
	.enum_mbus_code = ov5670_enum_mbus_code,
	.get_fmt = ov5670_get_pad_format,
	.set_fmt = ov5670_set_pad_format,
	.enum_frame_size = ov5670_enum_frame_size,
};

static const struct v4l2_subdev_sensor_ops ov5670_sensor_ops = {
	.g_skip_frames = ov5670_get_skip_frames,
};

static const struct v4l2_subdev_ops ov5670_subdev_ops = {
	.video = &ov5670_video_ops,
	.pad = &ov5670_pad_ops,
	.sensor = &ov5670_sensor_ops,
};

static const struct media_entity_operations ov5670_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_subdev_internal_ops ov5670_internal_ops = {
	.open = ov5670_open,
};

static int match_depend(struct device *dev, const void *data)
{
	return (dev && dev->fwnode == data) ? 1 : 0;
}

void *get_dep_dev(struct device *dev)
{
	struct acpi_handle *dev_handle = ACPI_HANDLE(dev);
	struct acpi_handle_list dep_devices;
	struct device *dep_dev;
	int ret;
	int i;

	// Get dependent INT3472 device
	if (!acpi_has_method(dev_handle, "_DEP")) {
		printk("No dependent devices\n");
		return ERR_PTR(-ENODEV);
	}

	ret = acpi_evaluate_reference(dev_handle, "_DEP", NULL,
					 &dep_devices);
	if (ACPI_FAILURE(ret)) {
		printk("Failed to evaluate _DEP.\n");
		return ERR_PTR(-ENODEV);
	}

	for (i = 0; i < dep_devices.count; i++) {
		struct acpi_device *device;
		struct acpi_device_info *info;

		ret = acpi_get_object_info(dep_devices.handles[i], &info);
		if (ACPI_FAILURE(ret)) {
			printk("Error reading _DEP device info\n");
			return ERR_PTR(-ENODEV);
		}

		if (info->valid & ACPI_VALID_HID &&
				!strcmp(info->hardware_id.string, "INT3472")) {
			if (acpi_bus_get_device(dep_devices.handles[i], &device))
				return ERR_PTR(-ENODEV);

			/* FIXME: For Acer Switch Alpha 12, use pci_bus_type because
			 * platform_bus_type doesn't work. */
			dep_dev = bus_find_device(&pci_bus_type, NULL,
					&device->fwnode, match_depend);
			if (dep_dev) {
				dev_info(dev, "Dependent device found: %s\n",
					dev_name(dep_dev));
				break;
			}
		}
	}

	if (!dep_dev) {
		dev_err(dev, "Error getting dependent device\n");
		return ERR_PTR(-EINVAL);
	}

	return dep_dev;
}

static int ov5670_probe(struct i2c_client *client)
{
	struct ov5670 *ov5670;
	struct device *dep_dev;
	const char *err_msg;
	int ret;

	ov5670 = devm_kzalloc(&client->dev, sizeof(*ov5670), GFP_KERNEL);
	if (!ov5670) {
		ret = -ENOMEM;
		err_msg = "devm_kzalloc() error";
		goto error_print;
	}

	ov5670->dep_dev = get_dep_dev(&client->dev);
	if (IS_ERR(ov5670->dep_dev)) {
		ret = PTR_ERR(ov5670->dep_dev);
		dev_err(&client->dev, "cannot get dep_dev: ret %d\n", ret);
		return ret;
	}
	dep_dev = ov5670->dep_dev;

	ret = gpio_crs_get(ov5670);
	if (ret) {
		dev_err(dep_dev, "Failed to get _CRS GPIOs\n");
		return ret;
	}

	ret = gpio_pmic_get(ov5670);
	if (ret) {
		dev_err(dep_dev, "Failed to get PMIC GPIOs\n");
		goto put_crs_gpio;
	}

	ret = regulator_pmic_get(ov5670);
	if (ret) {
		dev_err(&client->dev, "Failed to get power regulators\n");
		goto put_pmic_gpio;
	}

	ret = ov5670_configure_clock(ov5670);
	if (ret) {
		dev_dbg(&client->dev, "Could not configure clock.\n");
		goto disable_regulator;
	}

	/* Initialize subdev */
	v4l2_i2c_subdev_init(&ov5670->sd, client, &ov5670_subdev_ops);

	ret = power_up(&ov5670->sd);
	if (ret) {
		err_msg = "ov5670 power-up err.";
		power_down(&ov5670->sd);
		goto error_print;
	}

	/* Check module identity */
	ret = ov5670_identify_module(ov5670);
	if (ret) {
		err_msg = "ov5670_identify_module() error";
		goto error_print;
	}

	mutex_init(&ov5670->mutex);

	/* Set default mode to max resolution */
	ov5670->cur_mode = &supported_modes[0];

	ret = ov5670_init_controls(ov5670);
	if (ret) {
		err_msg = "ov5670_init_controls() error";
		goto error_mutex_destroy;
	}

	ov5670->sd.internal_ops = &ov5670_internal_ops;
	ov5670->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	ov5670->sd.entity.ops = &ov5670_subdev_entity_ops;
	ov5670->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	/* Source pad initialization */
	ov5670->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&ov5670->sd.entity, 1, &ov5670->pad);
	if (ret) {
		err_msg = "media_entity_pads_init() error";
		goto error_handler_free;
	}

	/* Async register for subdev */
	ret = v4l2_async_register_subdev_sensor_common(&ov5670->sd);
	if (ret < 0) {
		err_msg = "v4l2_async_register_subdev() error";
		goto error_entity_cleanup;
	}

	ov5670->streaming = false;

	/*
	 * Device is already turned on by i2c-core with ACPI domain PM.
	 * Enable runtime PM and turn off the device.
	 */
	pm_runtime_set_active(&client->dev);
	pm_runtime_enable(&client->dev);
	pm_runtime_idle(&client->dev);

	/* turn off sensor, after probed */
	ret = power_down(&ov5670->sd);
	if (ret)
		dev_info(&client->dev, "ov5670 power-off err.\n");

	return 0;

error_entity_cleanup:
	media_entity_cleanup(&ov5670->sd.entity);

error_handler_free:
	v4l2_ctrl_handler_free(ov5670->sd.ctrl_handler);

error_mutex_destroy:
	mutex_destroy(&ov5670->mutex);

disable_regulator:
	regulator_bulk_disable(OV5670_NUM_SUPPLIES, ov5670->supplies);

put_pmic_gpio:
	gpio_pmic_put(ov5670);

put_crs_gpio:
	gpio_crs_put(ov5670);

error_print:
	dev_err(&client->dev, "%s: %s %d\n", __func__, err_msg, ret);

	return ret;
}

static int ov5670_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov5670 *ov5670 = to_ov5670(sd);

	gpio_crs_put(ov5670);
	gpio_pmic_put(ov5670);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	mutex_destroy(&ov5670->mutex);

	pm_runtime_disable(&client->dev);

	return 0;
}

static const struct dev_pm_ops ov5670_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(ov5670_suspend, ov5670_resume)
};

#ifdef CONFIG_ACPI
static const struct acpi_device_id ov5670_acpi_ids[] = {
	{"INT3479"},
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(acpi, ov5670_acpi_ids);
#endif

static struct i2c_driver ov5670_i2c_driver = {
	.driver = {
		.name = "ov5670",
		.pm = &ov5670_pm_ops,
		.acpi_match_table = ACPI_PTR(ov5670_acpi_ids),
	},
	.probe_new = ov5670_probe,
	.remove = ov5670_remove,
};

module_i2c_driver(ov5670_i2c_driver);

MODULE_AUTHOR("Rapolu, Chiranjeevi <chiranjeevi.rapolu@intel.com>");
MODULE_AUTHOR("Yang, Hyungwoo <hyungwoo.yang@intel.com>");
MODULE_DESCRIPTION("Omnivision ov5670 sensor driver");
MODULE_LICENSE("GPL v2");
