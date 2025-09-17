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
#include <lwp_user_mm.h>
#include "drv_aes.h"
#include "drv_pufs.h"
#include <rtdbg.h>

#define DBG_TAG "AES"
#ifdef RT_DEBUG
#define DBG_LVL DBG_LOG
#else
#define DBG_LVL DBG_WARNING
#endif
#define DBG_COLOR

static int aes_init(union rt_aes_control_args* ctl)
{
    int ret;
    pufs_skcipher_init_t cfg;

    cfg.cipher = SK_AES;
    cfg.mode = ctl->init.mode;
    cfg.encrypt = ctl->init.encrypt ? 1 : 0;
    cfg.aes.keytype = ctl->init.keytype;
    cfg.aes.keyaddr = ctl->init.key;
    cfg.aes.keybits = ctl->init.keylen << 3;
    cfg.aes.iv = ctl->init.iv;
    cfg.aes.ivlen = ctl->init.ivlen;

    ret = skcipher_init(&cfg);

    return ret;
}

static int aes_update(union rt_aes_control_args* ctl)
{
    int ret;
    pufs_skcipher_update_t arg;

    arg.out = ctl->update.out;
    arg.outlen = ctl->update.outlen;
    arg.in = ctl->update.in;
    arg.inlen = ctl->update.inlen;

    ret = skcipher_update(&arg);

    return ret;
}

static int aes_final(union rt_aes_control_args* ctl)
{
    int ret;
    pufs_skcipher_final_t arg;

    arg.out = ctl->final.out;
    arg.outlen = ctl->final.outlen;
    arg.tag = ctl->final.tag;
    arg.taglen = ctl->final.taglen;

    ret = skcipher_final(&arg);

    return ret;
}

static rt_err_t aes_control(rt_device_t dev, int cmd, void* args)
{
    int ret;
    union rt_aes_control_args ctl;

    if (lwp_get_from_user(&ctl, args, sizeof(ctl)) == 0)
        memcpy(&ctl, args, sizeof(ctl));

    switch (cmd) {
    case RT_AES_INIT:
        ret = aes_init(&ctl);
        break;
    case RT_AES_UPDATE:
        ret = aes_update(&ctl);
        break;
    case RT_AES_FINAL:
        ret = aes_final(&ctl);
        break;
    default:
        ret = -EINVAL;
    }

    return ret;
}

static rt_err_t aes_open(rt_device_t dev, rt_uint16_t oflag)
{
    return RT_EOK;
}

static rt_err_t aes_close(rt_device_t dev)
{
    skcipher_deinit();

    return RT_EOK;
}

const static struct rt_device_ops aes_ops = {
    RT_NULL,
    aes_open,
    aes_close,
    RT_NULL,
    RT_NULL,
    aes_control,
};

int rt_hw_aes_device_init(void)
{
    static struct rt_device aes_dev;

    if (RT_EOK != rt_device_register(&aes_dev, "aes", RT_DEVICE_FLAG_RDWR)) {
        LOG_E("hwaes device register fail!\n");
        return -RT_ERROR;
    }

    aes_dev.ops = &aes_ops;

    return 0;
}
INIT_DEVICE_EXPORT(rt_hw_aes_device_init);
