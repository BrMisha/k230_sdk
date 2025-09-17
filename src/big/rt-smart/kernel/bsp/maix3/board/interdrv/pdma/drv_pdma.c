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

#include <rtthread.h>
#include <rthw.h>
#include <cache.h>
#include "ioremap.h"
#include "board.h"
#include "drv_hardlock.h"
#include "drv_pdma.h"

#define DBG_TAG "pdma"
#ifdef RT_DEBUG
#define DBG_LVL DBG_LOG
#else
#define DBG_LVL DBG_WARNING
#endif
#define DBG_COLOR
#include <rtdbg.h>

#define PDMA_CH0_IRQ 139
#define PDMA_CH1_IRQ 196

#define PDMA_BASE_ADDR 0x80804000
#define PDMA_IO_SIZE 0x200
#define PDMA_MAX_LINE_SIZE 0x3FFFFFFF

/* interrupt mask */
#define PDONE_INT 0x00000001
#define PITEM_INT 0x00000100
#define PPAUSE_INT 0x00010000
#define PTOUT_INT 0x01000000
#define PALL_INT 0x01010101

/* register structure */
typedef struct pdma_ch_reg {
    rt_uint32_t ch_ctl;
    rt_uint32_t ch_status;
    pdma_ch_cfg_t ch_cfg;
    rt_uint32_t ch_llt_saddr;
    rt_uint32_t reserved[4];
} pdma_ch_reg_t;

typedef struct pdma_reg {
    rt_uint32_t pdma_ch_en;
    rt_uint32_t dma_int_mask;
    rt_uint32_t dma_int_stat;
    rt_uint32_t reserved[5];
    pdma_ch_reg_t pdma_ch_reg[8];
    rt_uint32_t ch_peri_dev_sel[8];
} pdma_reg_t;

/* llt structure */
typedef struct pdma_llt {
    rt_uint32_t line_size : 30;
    rt_uint32_t pause : 1;
    rt_uint32_t node_intr : 1;
    rt_uint32_t src_addr;
    rt_uint32_t dst_addr;
    rt_uint32_t next_llt_addr;
} pdma_llt_t;

typedef struct {
    pdma_reg_t* reg;
    int hardlock;
    struct rt_event event;
    struct {
        int irq;
        char* name;
        void* data;
        rt_uint32_t size;
        void (*callback)(void* param);
        void* param;
    } chan[PDMA_CH_MAX];
} pdma_dev_t;
static pdma_dev_t pdma_dev;

static int pdma_take(int timeout, rt_base_t* plevel)
{
    rt_base_t level;

    while (1) {
        level = rt_hw_interrupt_disable();
        if (kd_hardlock_lock(pdma_dev.hardlock) == 0)
            break;
        rt_hw_interrupt_enable(level);
        if (timeout > 0)
            timeout--;
        else if (timeout == 0)
            return -RT_ETIMEOUT;
        rt_thread_mdelay(1);
    }
    *plevel = level;

    return 0;
}

int rt_dma_chan_request(char* name)
{
    rt_base_t level;
    int chan;

    if (pdma_take(1000, &level))
        return -RT_ETIMEOUT;

    chan = __builtin_ffs(~(pdma_dev.reg->pdma_ch_en)) - 1;
    if (chan >= PDMA_CH_0 && chan <= PDMA_CH_7) {
        pdma_dev.reg->pdma_ch_en |= (1 << chan);
        pdma_dev.reg->dma_int_mask &= (~(PALL_INT << chan));
    } else {
        chan = -RT_EBUSY;
    }

    kd_hardlock_unlock(pdma_dev.hardlock);
    rt_hw_interrupt_enable(level);

    if (chan >= 0) {
        pdma_dev.chan[chan].name = rt_strdup(name ? name : "anonymous");
        pdma_dev.reg->dma_int_stat = (PALL_INT << chan);
        pdma_dev.chan[chan].callback = 0;
        pdma_dev.chan[chan].param = 0;
        rt_event_recv(&pdma_dev.event, PALL_INT << chan,
            RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, 0, 0);
        rt_hw_interrupt_umask(pdma_dev.chan[chan].irq);
    } else {
        LOG_D("No idle pdma channel\n");
    }

    return chan;
}

int rt_dma_chan_release(int chan)
{
    rt_base_t level;
    char* name;
    void* data;

    if (chan >= PDMA_CH_MAX)
        return -RT_EINVAL;

    if (pdma_take(RT_WAITING_FOREVER, &level))
        return -RT_ETIMEOUT;

    pdma_dev.reg->pdma_ch_en &= ~(1 << chan);
    pdma_dev.reg->dma_int_mask |= (PALL_INT << chan);
    pdma_dev.reg->dma_int_stat = (PALL_INT << chan);
    name = pdma_dev.chan[chan].name;
    pdma_dev.chan[chan].name = RT_NULL;
    data = pdma_dev.chan[chan].data;
    pdma_dev.chan[chan].data = RT_NULL;
    pdma_dev.chan[chan].size = 0;

    kd_hardlock_unlock(pdma_dev.hardlock);
    rt_hw_interrupt_enable(level);

    rt_hw_interrupt_mask(pdma_dev.chan[chan].irq);
    rt_free(name);
    rt_free(data);

    return 0;
}

int rt_dma_chan_start(int chan)
{
    if (chan >= PDMA_CH_MAX)
        return -RT_EINVAL;

    pdma_dev.reg->pdma_ch_reg[chan].ch_ctl = 0x1;

    return 0;
}

int rt_dma_chan_stop(int chan)
{
    if (chan >= PDMA_CH_MAX)
        return -RT_EINVAL;

    pdma_dev.reg->pdma_ch_reg[chan].ch_ctl = 0x2;

    return 0;
}

int rt_dma_chan_config(int chan, pdma_transfer_cfg_t* cfg)
{
    pdma_llt_t* list;
    int list_num;

    if (chan >= PDMA_CH_MAX)
        return -RT_EINVAL;

    list_num = (cfg->length - 1) / PDMA_MAX_LINE_SIZE + 1;
    if (list_num == pdma_dev.chan[chan].size) {
        list = pdma_dev.chan[chan].data;
    } else {
        list = rt_malloc(sizeof(pdma_llt_t) * list_num);
        if (list == RT_NULL) {
            LOG_E("malloc pdma list failed\n");
            return -RT_ENOMEM;
        }
        rt_free(pdma_dev.chan[chan].data);
        pdma_dev.chan[chan].data = list;
        pdma_dev.chan[chan].size = list_num;
    }

    for (int i = 0; i < list_num; i++) {
        if (cfg->ch_cfg.ch_src_type == TX) {
            list[i].src_addr = (rt_uint64_t)cfg->src_addr + PDMA_MAX_LINE_SIZE * i;
            list[i].dst_addr = (rt_uint64_t)cfg->dst_addr;
        } else {
            list[i].src_addr = (rt_uint64_t)cfg->src_addr;
            list[i].dst_addr = (rt_uint64_t)cfg->dst_addr + PDMA_MAX_LINE_SIZE * i;
        }

        list[i].line_size = PDMA_MAX_LINE_SIZE;
        list[i].next_llt_addr = (rt_uint64_t)(list + 1);
        list[i].pause = 0;
    }
    list[list_num - 1].next_llt_addr = 0;
    list[list_num - 1].line_size = cfg->length - (list_num - 1) * PDMA_MAX_LINE_SIZE;

    rt_hw_cpu_dcache_clean((void*)list, sizeof(pdma_llt_t) * list_num);

    pdma_dev.reg->ch_peri_dev_sel[chan] = cfg->device;
    pdma_dev.reg->pdma_ch_reg[chan].ch_cfg = cfg->ch_cfg;
    pdma_dev.reg->pdma_ch_reg[chan].ch_llt_saddr = (rt_uint64_t)list;

    return 0;
}

int rt_dma_chan_done(int chan, int timeout)
{
    int ret = 0;
    rt_err_t err;
    rt_uint32_t event;

    err = rt_event_recv(&pdma_dev.event, PALL_INT << chan,
        RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, timeout, &event);
    if (err == RT_EOK) {
        if (event & PDONE_INT)
            return 0;
        if (event & PITEM_INT) {
            LOG_D("pdma ch%d node int", chan);
            ret = 1;
        }
        if (event & PPAUSE_INT) {
            LOG_W("pdma ch%d pause", chan);
            ret |= 2;
        }
        if (event & PTOUT_INT) {
            LOG_E("pdma ch%d timeout", chan);
            ret = -RT_ETIMEOUT;
        }
    } else if (err == -RT_ETIMEOUT) {
        LOG_E("pdma ch%d transfer timeout", chan);
        ret = -RT_ETIMEOUT;
    } else {
        ret = -RT_ERROR;
    }

    return ret;
}

int rt_dma_chan_callback(int chan, void (*callback)(void* param), void* param)
{
    if (chan >= PDMA_CH_MAX)
        return -RT_EINVAL;

    pdma_dev.chan[chan].callback = callback;
    pdma_dev.chan[chan].param = param;

    return 0;
}

static void pdma_irq(int irq, void* param)
{
    rt_uint32_t stat;
    int ch = (rt_uint64_t)param;

    stat = pdma_dev.reg->dma_int_stat & (PALL_INT << ch);
    if (stat) {
        pdma_dev.reg->dma_int_stat = stat;
        if (pdma_dev.chan[ch].callback)
            pdma_dev.chan[ch].callback(pdma_dev.chan[ch].param);
        rt_event_send(&pdma_dev.event, stat);
    }
}

int rt_hw_pdma_device_init(void)
{
    pdma_dev.reg = rt_ioremap((void*)PDMA_BASE_ADDR, PDMA_IO_SIZE);

    if (RT_NULL == pdma_dev.reg) {
        LOG_E("pdma module ioremap error!\n");
        return -RT_ERROR;
    }

    if (kd_request_lock(HARDLOCK_PDMA)) {
        LOG_E("Fail to request hardlock-%d\n", HARDLOCK_PDMA);
        return -RT_ERROR;
    }
    pdma_dev.hardlock = HARDLOCK_PDMA;

    rt_event_init(&pdma_dev.event, "pdma_event", RT_IPC_FLAG_PRIO);

    pdma_dev.chan[0].irq = PDMA_CH0_IRQ;
    for (int i = 1; i < 8; i++)
        pdma_dev.chan[i].irq = PDMA_CH1_IRQ - 1 + i;

    for (int i = 0; i < 8; i++)
        rt_hw_interrupt_install(pdma_dev.chan[i].irq, pdma_irq, (void*)(rt_uint64_t)i, "pdma");

    return 0;
}
INIT_BOARD_EXPORT(rt_hw_pdma_device_init);
