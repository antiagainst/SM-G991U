/*
 *  Copyright (C) 2010, Imagis Technology Co. Ltd. All Rights Reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */

#ifndef __ISG5320A_SUB_REG_H__
#define __ISG5320A_SUB_REG_H__

enum registers {
	ISG5320A_IRQSRC_REG = 0x00,
	ISG5320A_IRQSTS_REG,
	ISG5320A_IRQMSK_REG = 0x04,
	ISG5320A_IRQCON_REG,
	ISG5320A_OSCCON_REG,
	ISG5320A_IRQFUNC_REG,
	ISG5320A_WUTDATA_REG = 0x08,
	ISG5320A_WUTDATD_REG = 0x0C,
	ISG5320A_TEMPERATURE_ENABLE_REG = 0x10,
	ISG5320A_CML_BIAS_REG = 0x13,
	ISG5320A_BS_ON_WD_REG = 0x19,
	ISG5320A_NUM_OF_CLK = 0x1B,
	ISG5320A_SCANCTRL1_REG = 0x38,
	ISG5320A_SCANCTRL2_REG,
	ISG5320A_SCANCTRL11_REG = 0x42,
	ISG5320A_SCANCTRL13_REG = 0x44,
	ISG5320A_COARSE_OUT_A_REG = 0x47,
	ISG5320A_FINE_OUT_A_REG,
	ISG5320A_COARSE_OUT_B_REG,
	ISG5320A_FINE_OUT_B_REG,
	ISG5320A_CFCAL_RTN_REG,
	ISG5320A_CDC16_A_H_REG = 0x56,
	ISG5320A_CDC16_A_L_REG,
	ISG5320A_BSL16_A_H_REG,
	ISG5320A_BSL16_A_L_REG,
	ISG5320A_CDC16_B_H_REG,
	ISG5320A_CDC16_B_L_REG,
	ISG5320A_CDC16_T_H_REG,
	ISG5320A_CDC16_T_L_REG,
	ISG5320A_BSL16_B_H_REG,
	ISG5320A_BSL16_B_L_REG,
	ISG5320A_COARSE_A_REG = 0x63,
	ISG5320A_FINE_A_REG,
	ISG5320A_A_PROXCTL1_REG,
	ISG5320A_A_PROXCTL2_REG,
	ISG5320A_A_PROXCTL3_REG,
	ISG5320A_A_PROXCTL4_REG,
	ISG5320A_A_PROXCTL5_REG,
	ISG5320A_A_PROXCTL6_REG,
	ISG5320A_A_PROXCTL7_REG,
	ISG5320A_A_PROXCTL8_REG,
	ISG5320A_ANALOG_GAIN = 0x95,
	ISG5320A_COARSE_B_REG = 0x97,
	ISG5320A_FINE_B_REG,
	ISG5320A_B_PROXCTL1_REG,
	ISG5320A_B_PROXCTL2_REG,
	ISG5320A_B_PROXCTL3_REG,
	ISG5320A_B_PROXCTL4_REG,
	ISG5320A_B_PROXCTL5_REG,
	ISG5320A_B_PROXCTL6_REG,
	ISG5320A_B_PROXCTL7_REG,
	ISG5320A_B_PROXCTL8_REG,
	ISG5320A_CHB_LSUM_TYPE_REG = 0XA1,
	ISG5320A_DIGITAL_ACC_REG = 0xA4,
	ISG5320A_CHB_CDC_UP_COEF_REG = 0xB3,
	ISG5320A_CHB_CDC_DN_COEF_REG = 0xB4,
	ISG5320A_SOFTRESET_REG = 0xFD,
	ISG5320A_CHIPID_REG,
	ISG5320A_PROTECT_REG = 0xCD,
};

#define ISG5320A_PROX_STATE         4

#define ISG5320A_IRQ_ENABLE         0x0D
#define ISG5320A_IRQ_DISABLE        0x13

#define ISG5320A_BFCAL_START        0x07

#define ISG5320A_CFCAL_START        0xDD
#define ISG5320A_FCAL_CHECK         0x03
#define ISG5320A_CHECK_TIME         50

#define ISG5320A_SCAN_START         0xC9
#define ISG5320A_SCAN_STOP          0x00

#define ISG5320A_SCAN2_CLEAR        0x60
#define ISG5320A_SCAN2_RESET        0x00
#define ISG5320A_OSC_SLEEP          0xB0
#define ISG5320A_OSC_NOMAL          0xF0
#define ISG5320A_RESET_CONDITION    5000
#define ISG5320A_CH_EN              0x80

#define ISG5320A_RST_VALUE          0xDE
#define ISG5320A_PRT_VALUE          0xDE

#define ISG5320A_BS_WD_OFF          0x0A
#define ISG5320A_BS_WD_ON           0x8A

#define ISG5320A_BFCAL_CHK_RDY_TIME       (3 * 60 * 2) // 3min (unit: 500ms)
#define ISG5320A_BFCAL_CHK_CYCLE_TIME     4   //  2sec (unit: 500ms)
#define ISG5320A_BFCAL_CHK_DIFF_RATIO     3
enum {
	OFF = 0,
	ON,
};

enum {
	NONE_ENABLE = -1,
	FAR = 0,
	CLOSE,
};

struct isg5320a_reg_data {
	unsigned char addr;
	unsigned char val;
};

static const struct isg5320a_reg_data setup_reg[] = {
	{    .addr = 0x05,    .val = 0xFC,    },
	{    .addr = 0x06,    .val = 0xF1,    },
	{    .addr = 0x07,    .val = 0x0D,    },
	{    .addr = 0x08,    .val = 0x01,    },
	{    .addr = 0x09,    .val = 0x80,    },
	{    .addr = 0x0A,    .val = 0x20,    },
	{    .addr = 0x0B,    .val = 0x00,    },
	{    .addr = 0x0C,    .val = 0x00,    },
	{    .addr = 0x12,    .val = 0x89,    },
	{    .addr = 0x19,    .val = 0x8A,    },
	{    .addr = 0x1B,    .val = 0x28,    },
	{    .addr = 0x1D,    .val = 0x20,    },
	{    .addr = 0x1E,    .val = 0x00,    },
	{    .addr = 0x1F,    .val = 0x18,    },
	{    .addr = 0x20,    .val = 0x05,    },
	{    .addr = 0x21,    .val = 0x10,    },
	{    .addr = 0x22,    .val = 0x10,    },
	{    .addr = 0x23,    .val = 0x00,    },
	{    .addr = 0x24,    .val = 0x00,    },
	{    .addr = 0x25,    .val = 0x05,    },
	{    .addr = 0x26,    .val = 0x05,    },
	{    .addr = 0x27,    .val = 0x10,    },
	{    .addr = 0x28,    .val = 0x20,    },
	{    .addr = 0x29,    .val = 0x00,    },
	{    .addr = 0x2A,    .val = 0x20,    },
	{    .addr = 0x2B,    .val = 0x00,    },
	{    .addr = 0x2C,    .val = 0x00,    },
	{    .addr = 0x2D,    .val = 0x20,    },
	{    .addr = 0x2F,    .val = 0x18,    },
	{    .addr = 0x30,    .val = 0xF0,    },
	{    .addr = 0x41,    .val = 0x8F,    },
	{    .addr = 0x60,    .val = 0x00,    },
	{    .addr = 0x61,    .val = 0xBC,    },
	{    .addr = 0x62,    .val = 0x1F,    },
	{    .addr = 0x63,    .val = 0x00,    },
	{    .addr = 0x64,    .val = 0x00,    },
	{    .addr = 0x65,    .val = 0x44,    },
	{    .addr = 0x66,    .val = 0x3C,    },
	{    .addr = 0x67,    .val = 0x01,    },
	{    .addr = 0x68,    .val = 0x00,    },
	{    .addr = 0x69,    .val = 0x00,    },
	{    .addr = 0x6A,    .val = 0x10,    },
	{    .addr = 0x6B,    .val = 0x00,    },
	{    .addr = 0x6C,    .val = 0x10,    },
	{    .addr = 0x6D,    .val = 0x40,    },
	{    .addr = 0x6E,    .val = 0x22,    },
	{    .addr = 0x6F,    .val = 0x00,    },
	{    .addr = 0x70,    .val = 0x01,    },
	{    .addr = 0x71,    .val = 0x60,    },
	{    .addr = 0x72,    .val = 0xFF,    },
	{    .addr = 0x73,    .val = 0xFF,    },
	{    .addr = 0x74,    .val = 0xFF,    },
	{    .addr = 0x7B,    .val = 0x08,    },
	{    .addr = 0x7C,    .val = 0x00,    },
	{    .addr = 0x7D,    .val = 0x0C,    },
	{    .addr = 0x7E,    .val = 0x00,    },
	{    .addr = 0x7F,    .val = 0xF8,    },
	{    .addr = 0x80,    .val = 0xF8,    },
	{    .addr = 0x81,    .val = 0x00,    },
	{    .addr = 0x82,    .val = 0xF0,    },
	{    .addr = 0x83,    .val = 0xF0,    },
	{    .addr = 0x84,    .val = 0xF0,    },
	{    .addr = 0x85,    .val = 0xF0,    },
	{    .addr = 0x86,    .val = 0xF0,    },
	{    .addr = 0x87,    .val = 0xF0,    },
	{    .addr = 0x88,    .val = 0x00,    },
	{    .addr = 0x89,    .val = 0x00,    },
	{    .addr = 0x8A,    .val = 0x08,    },
	{    .addr = 0x8B,    .val = 0x00,    },
	{    .addr = 0x8C,    .val = 0x02,    },
	{    .addr = 0x8D,    .val = 0x00,    },
	{    .addr = 0x99,    .val = 0x21,    },
	{    .addr = 0x9A,    .val = 0x3C,    },
	{    .addr = 0x9B,    .val = 0x01,    },
	{    .addr = 0x9C,    .val = 0x2C,    },
	{    .addr = 0x9D,    .val = 0x00,    },
	{    .addr = 0x9E,    .val = 0x00,    },
	{    .addr = 0x9F,    .val = 0x00,    },
	{    .addr = 0xA0,    .val = 0x2D,    },
	{    .addr = 0xA1,    .val = 0xC0,    },
	{    .addr = 0xA2,    .val = 0x33,    },
	{    .addr = 0xA3,    .val = 0x00,    },
	{    .addr = 0xA4,    .val = 0x08,    },
	{    .addr = 0xA5,    .val = 0x60,    },
	{    .addr = 0xA6,    .val = 0xFF,    },
	{    .addr = 0xA7,    .val = 0xFF,    },
	{    .addr = 0xA8,    .val = 0xFF,    },
	{    .addr = 0xA9,    .val = 0x00,    },
	{    .addr = 0xAF,    .val = 0x40,    },
	{    .addr = 0xB0,    .val = 0x00,    },
	{    .addr = 0xB1,    .val = 0x60,    },
	{    .addr = 0xB2,    .val = 0x00,    },
	{    .addr = 0xB3,    .val = 0xB0,    },
	{    .addr = 0xB4,    .val = 0xB0,    },
	{    .addr = 0xB5,    .val = 0x00,    },
	{    .addr = 0xB6,    .val = 0xFD,    },
	{    .addr = 0xB7,    .val = 0x40,    },
	{    .addr = 0xB8,    .val = 0xFD,    },
	{    .addr = 0xB9,    .val = 0x80,    },
	{    .addr = 0xBA,    .val = 0xE0,    },
	{    .addr = 0xBB,    .val = 0xF0,    },
	{    .addr = 0xBC,    .val = 0x00,    },
	{    .addr = 0xBD,    .val = 0x00,    },
	{    .addr = 0xBE,    .val = 0x01,    },
	{    .addr = 0xBF,    .val = 0x00,    },
	{    .addr = 0xC0,    .val = 0x00,    },
	{    .addr = 0xC1,    .val = 0x4B,    },
	{    .addr = 0xC2,    .val = 0xF0,    },
	{    .addr = 0xC3,    .val = 0x00,    },
	{    .addr = 0xC4,    .val = 0x00,    },
	{    .addr = 0xC5,    .val = 0x00,    },
	{    .addr = 0x13,    .val = 0x38,    },
	{    .addr = 0x18,    .val = 0xF1,    }, /* A ch : CS0, B ch : CS1 */
	{    .addr = 0x1C,    .val = 0x18,    },
	{    .addr = 0x35,    .val = 0x00,    },
	{    .addr = 0x36,    .val = 0x00,    },
	{    .addr = 0x3A,    .val = 0x0A,    },
	{    .addr = 0x3B,    .val = 0x50,    },
	{    .addr = 0x3C,    .val = 0x0A,    },
	{    .addr = 0x3D,    .val = 0x50,    },
	{    .addr = 0x3E,    .val = 0x60,    },
	{    .addr = 0x94,    .val = 0x00,    },
	{    .addr = 0x95,    .val = 0xBD,    },
	{    .addr = 0x96,    .val = 0x1F,    },
	{    .addr = 0x2E,    .val = 0x13,    },
	{    .addr = 0x0E,    .val = 0xE0,    },
	{    .addr = 0x0F,    .val = 0x1E,    },
	{    .addr = 0x10,    .val = 0x40,    },
	{    .addr = 0x11,    .val = 0xC1,    },
	{    .addr = 0xD0,    .val = 0xE0,    },
	{    .addr = 0xD1,    .val = 0x02,    },
	{    .addr = 0xD2,    .val = 0x00,    },
	{    .addr = 0xD3,    .val = 0xFF,    },
	{    .addr = 0xD4,    .val = 0xFF,    },
	{    .addr = 0xD5,    .val = 0x14,    },
	{    .addr = 0xD6,    .val = 0x32,    },
	{    .addr = 0xD7,    .val = 0xFF,    },
	{    .addr = 0xD8,    .val = 0x20,    },
	{    .addr = 0xD9,    .val = 0x80,    },
	{    .addr = 0xDA,    .val = 0x80,    },
	{    .addr = 0xDB,    .val = 0xF0,    },
	{    .addr = 0xDC,    .val = 0x00,    },
	{    .addr = 0x76,    .val = 0x08,    },
	{    .addr = 0x77,    .val = 0x00,    },
	{    .addr = 0x79,    .val = 0x02,    },
	{    .addr = 0x7A,    .val = 0x00,    },
	{    .addr = 0xAA,    .val = 0x40,    },
	{    .addr = 0xAB,    .val = 0x00,    },
	{    .addr = 0xAD,    .val = 0x04,    },
	{    .addr = 0xAE,    .val = 0x00,    },
};

#endif /* __ISG5320A_SUB_REG_H__ */
