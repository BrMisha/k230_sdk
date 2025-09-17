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
#include "drv_hash.h"
#include "drv_pufs.h"
#include <rtdbg.h>

#define DBG_TAG "HASH"
#ifdef RT_DEBUG
#define DBG_LVL DBG_LOG
#else
#define DBG_LVL DBG_WARNING
#endif
#define DBG_COLOR

static int _hash_init(union rt_hash_control_args* ctl)
{
    int ret;
    pufs_hash_init_t arg;

    arg.mode = ctl->init.mode;

    ret = hash_init(&arg);

    return ret;
}

static int _hash_update(union rt_hash_control_args* ctl)
{
    int ret;
    pufs_hash_update_t arg;

    arg.msg = ctl->update.msg;
    arg.msglen = ctl->update.msglen;

    ret = hash_update(&arg);

    return ret;
}

static int _hash_final(union rt_hash_control_args* ctl)
{
    int ret;
    pufs_hash_final_t arg;

    arg.dgst = ctl->final.dgst;
    arg.dlen = ctl->final.dlen;

    ret = hash_final(&arg);

    return ret;
}

static rt_err_t hash_control(rt_device_t dev, int cmd, void* args)
{
    int ret;
    union rt_hash_control_args ctl;

    lwp_get_from_user(&ctl, args, sizeof(ctl));

    switch (cmd) {
    case RT_HASH_INIT:
        ret = _hash_init(&ctl);
        break;
    case RT_HASH_UPDATE:
        ret = _hash_update(&ctl);
        break;
    case RT_HASH_FINAL:
        ret = _hash_final(&ctl);
        break;
    default:
        ret = -EINVAL;
    }
    return ret;
}

static rt_err_t hash_open(rt_device_t dev, rt_uint16_t oflag)
{
    return RT_EOK;
}

static rt_err_t hash_close(rt_device_t dev)
{
    hash_deinit();

    return RT_EOK;
}

const static struct rt_device_ops hash_ops = {
    RT_NULL,
    hash_open,
    hash_close,
    RT_NULL,
    RT_NULL,
    hash_control
};

int rt_hw_hash_device_init(void)
{
    static struct rt_device hash_dev;

    if (RT_EOK != rt_device_register(&hash_dev, "hash", RT_DEVICE_FLAG_RDWR)) {
        LOG_E("hwhash device register fail\n");
        return -RT_ERROR;
    }

    hash_dev.ops = &hash_ops;

    return 0;
}
INIT_DEVICE_EXPORT(rt_hw_hash_device_init);
