﻿/* Copyright (c) 2017-2018, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _CAM_SBI_HW_REG_H_
#define _CAM_SBI_HW_REG_H_

#include "cam_irq_controller.h"
#include "cam_sbi_hw_core.h"

static struct cam_irq_register_set sbi_top_irq_reg_set[4] = {
	{
		.mask_reg_offset   = 0x00000020,
		.clear_reg_offset  = 0x00000030,
		.status_reg_offset = 0x00000010,
	},
	{
		.mask_reg_offset = 0x00000024,
		.clear_reg_offset = 0x00000034,
		.status_reg_offset = 0x00000014,
	},
	{
		.mask_reg_offset = 0x00000028,
		.clear_reg_offset = 0x00000038,
		.status_reg_offset = 0x00000018,
	},
	{
		.mask_reg_offset = 0x0000002C,
		.clear_reg_offset = 0x0000003C,
		.status_reg_offset = 0x0000001C,
	},
};

static struct cam_sbi_irq_controller_reg_info sbi_top_irq_reg_info = {
	.num_registers = 4,// not in use
	.irq_reg_set = sbi_top_irq_reg_set,
	.global_clear_offset  = 0x0000000C,
	 .global_clear_bitmask = 0x00000003,
	.reset_cmd_offset = 0x00000008,
};


const struct cam_sbi_hw_info cam_sbi10_hw_info = {
	.irq_reg_info = &sbi_top_irq_reg_info,

	.dma_reg = {
		.dma_wdma0_frame_size       = 0x00000604,
		.dma_wdma0_baseaddr         = 0x00000c08,// 0x00000608,// in use
		.dma_wdma0_t4_baseaddr      = 0x00000c24, // SBIv2
		.dma_wdma0_stride           = 0x0000060c,
		.dma_wdma0_t4_stride        = 0x00000c54, // SBIv2
		.dma_wdma0_meta_format      = 0x00000c40, // SBIv2
		.dma_wdma0_format           = 0x00000610,
		.dma_wdma0_control          = 0x00000614,
		.dma_wdma0_itr_mode         = 0x00000618,
		.dma_wdma0_error_enable     = 0x0000061c,
		.dma_wdma0_offset_addr      = 0x00000c3c, // in use
		.dma_wdma0_baseaddr_hr      = 0x00000c34,// in use
		.dma_wdma1_frame_size       = 0x00000644,
		.dma_wdma1_baseaddr         = 0x00000648,
		.dma_wdma1_stride           = 0x0000064c,
		.dma_wdma1_format           = 0x00000650,
		.dma_wdma1_control          = 0x00000654,
		.dma_wdma1_itr_mode         = 0x00000658,
		.dma_wdma1_error_enable     = 0x0000065c,
		.dma_wdma1_offset_addr      = 0x0000067c,
		.dma_wdma2_frame_size       = 0x00000684,
		.dma_wdma2_baseaddr         = 0x00000d08,// 0x00000688, // in use
		.dma_wdma2_stride           = 0x0000068c,
		.dma_wdma2_format           = 0x00000690,
		.dma_wdma2_control          = 0x00000694,
		.dma_wdma2_itr_mode         = 0x00000698,
		.dma_wdma2_error_enable     = 0x0000069c,
		.dma_wdma2_baseaddr_hr      = 0x00000d34, // in use
		.dma_wdma2_offset_addr      = 0x000006bc,
		.dma_wdma_max_moa           = 0x000006c0,
		.dma_rdma0_frame_size       = 0x00000704,
		.dma_rdma0_baseaddr         = 0x00000708,
		.dma_rdma0_t4_baseaddr      = 0x00000ea4, // SBIv2
		.dma_rdma0_meta_format      = 0x00000ec0, // SBIv2
		.dma_rdma0_stride           = 0x0000070c,
		.dma_rdma0_t4_stride        = 0x00000ed4, // SBIv2
		.dma_rdma0_format           = 0x00000710,
		.dma_rdma0_control          = 0x00000714,
		.dma_rdma0_itr_mode         = 0x00000718,
		.dma_rdma0_error_enable     = 0x0000071c,
		.dma_rdma0_offset_addr      = 0x0000073c,
		.dma_rdma0_baseaddr_hr      = 0x00000734,
		.dma_rdma1_frame_size       = 0x00000744,
		.dma_rdma1_baseaddr         = 0x00000f08, // in use
		.dma_rdma1_stride           = 0x0000074c,
		.dma_rdma1_format           = 0x00000750,
		.dma_rdma1_control          = 0x00000754,
		.dma_rdma1_itr_mode         = 0x00000758,
		.dma_rdma1_error_enable     = 0x0000075c,
		.dma_rdma1_offset_addr      = 0x00000f3c,
		.dma_rdma1_baseaddr_hr      = 0x00000f34, // in use
		.dma_rdma2_frame_size       = 0x00000784,
		.dma_rdma2_baseaddr         = 0x00000f88, // in use
		.dma_rdma2_stride           = 0x0000078c,
		.dma_rdma2_format           = 0x00000790,
		.dma_rdma2_control          = 0x00000794,
		.dma_rdma2_itr_mode         = 0x00000798,
		.dma_rdma2_error_enable     = 0x0000079c,
		.dma_rdma2_baseaddr_hr      = 0x00000fb4, // in use
		.dma_rdma2_offset_addr      = 0x000007bc,
		.dma_rdma_max_moa           = 0x000007c0,
	},
};


#endif /* _CAM_SBI_HW_REG_H_ */

