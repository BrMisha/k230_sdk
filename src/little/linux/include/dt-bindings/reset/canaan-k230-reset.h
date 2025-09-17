/* Copyright (c) 2023, Canaan Bright Sight Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/
#ifndef _DT_BINDINGS_CANAAN_K230_RESET_H_
#define _DT_BINDINGS_CANAAN_K230_RESET_H_

#define SYSCTL_RESET_CPU0_CORE  0
#define SYSCTL_RESET_CPU0_FLUSH 1
#define SYSCTL_RESET_CPU1_CORE  2
#define SYSCTL_RESET_CPU1_FLUSH 3
#define SYSCTL_RESET_AI         4
#define SYSCTL_RESET_VPU        5
#define SYSCTL_RESET_HS         6
#define SYSCTL_RESET_HS_AHB     7
#define SYSCTL_RESET_SDIO0      8
#define SYSCTL_RESET_SDIO1      9
#define SYSCTL_RESET_SDIO_AXI   10
#define SYSCTL_RESET_USB0       11
#define SYSCTL_RESET_USB1       12
#define SYSCTL_RESET_USB0_AHB   13
#define SYSCTL_RESET_USB1_AHB   14
#define SYSCTL_RESET_SPI0       15
#define SYSCTL_RESET_SPI1       16
#define SYSCTL_RESET_SPI2       17
#define SYSCTL_RESET_SEC        18
#define SYSCTL_RESET_PDMA       19
#define SYSCTL_RESET_SDMA       20
#define SYSCTL_RESET_DECOMPRESS 21
#define SYSCTL_RESET_SRAM       22
#define SYSCTL_RESET_SHRM_AXIM  23
#define SYSCTL_RESET_SHRM_AXIS  24
#define SYSCTL_RESET_SHRM_APB   25
#define SYSCTL_RESET_NONAI2D    26
#define SYSCTL_RESET_MCTL       27
#define SYSCTL_RESET_ISP        28
#define SYSCTL_RESET_ISP_DW     29
#define SYSCTL_RESET_CSI0_APB   30
#define SYSCTL_RESET_CSI1_APB   31
#define SYSCTL_RESET_CSI2_APB   32
#define SYSCTL_RESET_CSI_DPHY_APB   33
#define SYSCTL_RESET_ISP_AHB    34
#define SYSCTL_RESET_M0         35
#define SYSCTL_RESET_M1         36
#define SYSCTL_RESET_M2         37
#define SYSCTL_RESET_DPU        38
#define SYSCTL_RESET_DISP       39
#define SYSCTL_RESET_GPU        40
#define SYSCTL_RESET_AUDIO      41
#define SYSCTL_RESET_TIMER0     42
#define SYSCTL_RESET_TIMER1     43
#define SYSCTL_RESET_TIMER2     44
#define SYSCTL_RESET_TIMER3     45
#define SYSCTL_RESET_TIMER4     46
#define SYSCTL_RESET_TIMER5     47
#define SYSCTL_RESET_TIMER_APB  48
#define SYSCTL_RESET_HDI        49
#define SYSCTL_RESET_WDT0       50
#define SYSCTL_RESET_WDT1       51
#define SYSCTL_RESET_WDT0_APB   52
#define SYSCTL_RESET_WDT1_APB   53
#define SYSCTL_RESET_TS_APB     54
#define SYSCTL_RESET_MAILBOX    55
#define SYSCTL_RESET_STC        56
#define SYSCTL_RESET_PMU        57
#define SYSCTL_RESET_LS_APB     58
#define SYSCTL_RESET_UART0      59
#define SYSCTL_RESET_UART1      60
#define SYSCTL_RESET_UART2      61
#define SYSCTL_RESET_UART3      62
#define SYSCTL_RESET_UART4      63
#define SYSCTL_RESET_I2C0       64
#define SYSCTL_RESET_I2C1       65
#define SYSCTL_RESET_I2C2       66
#define SYSCTL_RESET_I2C3       67
#define SYSCTL_RESET_I2C4       68
#define SYSCTL_RESET_JAMLINK0_APB   69
#define SYSCTL_RESET_JAMLINK1_APB   70
#define SYSCTL_RESET_JAMLINK2_APB   71
#define SYSCTL_RESET_JAMLINK3_APB   72
#define SYSCTL_RESET_CODEC_APB      73
#define SYSCTL_RESET_GPIO_DB        74
#define SYSCTL_RESET_GPIO_APB       75
#define SYSCTL_RESET_ADC            76
#define SYSCTL_RESET_ADC_APB        77
#define SYSCTL_RESET_PWM_APB        78
#define SYSCTL_RESET_SPI2AXI        79

#endif