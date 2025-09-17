/* Copyright (c) 2023, Canaan Bright Sight Co., Ltd
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef DRV_PDMA_H__
#define DRV_PDMA_H__

#include <rtdef.h>

typedef enum pdma_ch {
    PDMA_CH_0 = 0,
    PDMA_CH_1 = 1,
    PDMA_CH_2 = 2,
    PDMA_CH_3 = 3,
    PDMA_CH_4 = 4,
    PDMA_CH_5 = 5,
    PDMA_CH_6 = 6,
    PDMA_CH_7 = 7,
    PDMA_CH_MAX,
} pdma_ch_e;

typedef enum pdma_burst_len {
    PBURST_LEN_1 = 0,
    PBURST_LEN_2 = 1,
    PBURST_LEN_3 = 2,
    PBURST_LEN_4 = 3,
    PBURST_LEN_5 = 4,
    PBURST_LEN_6 = 5,
    PBURST_LEN_7 = 6,
    PBURST_LEN_8 = 7,
    PBURST_LEN_9 = 8,
    PBURST_LEN_10 = 9,
    PBURST_LEN_11 = 10,
    PBURST_LEN_12 = 11,
    PBURST_LEN_13 = 12,
    PBURST_LEN_14 = 13,
    PBURST_LEN_15 = 14,
    PBURST_LEN_16 = 15,
} pdma_burst_len_e;

typedef enum device_sel {
    UART0_TX = 0,
    UART0_RX = 1,
    UART1_TX = 2,
    UART1_RX = 3,
    UART2_TX = 4,
    UART2_RX = 5,
    UART3_TX = 6,
    UART3_RX = 7,
    UART4_TX = 8,
    UART4_RX = 9,
    I2C0_TX = 10,
    I2C0_RX = 11,
    I2C1_TX = 12,
    I2C1_RX = 13,
    I2C2_TX = 14,
    I2C2_RX = 15,
    I2C3_TX = 16,
    I2C3_RX = 17,
    I2C4_TX = 18,
    I2C4_RX = 19,
    AUDIO_TX = 20,
    AUDIO_RX = 21,
    JAMLINK0_TX = 22,
    JAMLINK0_RX = 23,
    JAMLINK1_TX = 24,
    JAMLINK1_RX = 25,
    JAMLINK2_TX = 26,
    JAMLINK2_RX = 27,
    JAMLINK3_TX = 28,
    JAMLINK3_RX = 29,
    ADC0 = 30,
    ADC1 = 31,
    ADC2 = 32,
    PDM_IN = 33,
} device_sel_e;

typedef enum pendian {
    PDEFAULT,
    PBYTE2,
    PBYTE4,
    PBYTE8,
} pendian_e;

typedef enum hsize {
    PSBYTE1,
    PSBYTE2,
    PSBYTE4,
} hsize_e;

typedef enum src_type {
    TX = 0,
    RX = 1,
} src_type_e;

typedef struct pdma_ch_cfg {
    rt_uint32_t ch_src_type : 1;
    rt_uint32_t ch_dev_hsize : 2;
    rt_uint32_t reserved0 : 1;
    rt_uint32_t ch_dat_endian : 2;
    rt_uint32_t reserved1 : 2;
    rt_uint32_t ch_dev_blen : 4;
    rt_uint32_t ch_priority : 4;
    rt_uint32_t ch_dev_tout : 12;
    rt_uint32_t reserved2 : 4;
} pdma_ch_cfg_t;

typedef struct {
    device_sel_e device;
    void *src_addr;
    void *dst_addr;
    rt_uint32_t length;
    pdma_ch_cfg_t ch_cfg;
} pdma_transfer_cfg_t;

int rt_dma_chan_request(char* name);
int rt_dma_chan_release(int chan);
int rt_dma_chan_start(int chan);
int rt_dma_chan_stop(int chan);
int rt_dma_chan_config(int chan, pdma_transfer_cfg_t* cfg);
int rt_dma_chan_done(int chan, int timeout);
int rt_dma_chan_callback(int chan, void (*callback)(void* param), void* param);

#endif
