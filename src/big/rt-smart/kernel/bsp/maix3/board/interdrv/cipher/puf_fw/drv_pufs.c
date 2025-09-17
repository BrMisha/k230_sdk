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
#include <dfs_posix.h>
#include <board.h>
#include <ioremap.h>
#include <lwp_user_mm.h>
#include "drv_hardlock.h"
#include "drv_pufs.h"
#include "pufs_dma.h"
#include "pufs_internal.h"
#include "pufs_rt.h"
#include "pufs_rt_internal.h"
#include "pufs_ka.h"
#include "pufs_crypto.h"
#include "pufs_crypto_internal.h"
#include "pufs_hmac.h"
#include "pufs_cmac.h"
#include "pufs_kdf.h"
#include "pufs_ecp.h"
#include "pufs_sp38a.h"
#include "pufs_sp38d.h"
#include "pufs_sp38e.h"
#include "pufs_sp38c.h"
#include "pufs_sp90a.h"
#include "pufs_sm2.h"
#include <rtdbg.h>

#define DBG_TAG "PUFS"
#ifdef RT_DEBUG
#define DBG_LVL DBG_LOG
#else
#define DBG_LVL DBG_WARNING
#endif
#define DBG_COLOR

#define DMA_ADDR_OFFSET 0x000
#define CRYPTO_ADDR_OFFSET 0x100
#define SP38A_ADDR_OFFSET 0x200
#define CMAC_ADDR_OFFSET 0x220
#define SP38C_ADDR_OFFSET 0x240
#define SP38D_ADDR_OFFSET 0x260
#define SP38E_ADDR_OFFSET 0x280
#define KWP_ADDR_OFFSET 0x300
#define CHACHA_ADDR_OFFSET 0x400
#define HMAC_HASH_ADDR_OFFSET 0x800
#define KDF_ADDR_OFFSET 0x900
#define SP90A_ADDR_OFFSET 0xB00
#define KA_ADDR_OFFSET 0xC00
#define PKC_ADDR_OFFSET 0x1000
#define RT_ADDR_OFFSET 0x3000
#define CDE_ADDR_OFFSET 0x4000

enum {
    PUFS_TYPE_NONE = 0,
    PUFS_TYPE_HASH,
    PUFS_TYPE_HMAC,
    PUFS_TYPE_CMAC,
    PUFS_TYPE_SP38A,
    PUFS_TYPE_SP38C,
    PUFS_TYPE_SP38D,
    PUFS_TYPE_SP38E,
};

struct pufs_device {
    struct rt_device dev;
    void* base;
    uint8_t hardlock;
    uint8_t busy;
    uint8_t type;
    void* ctx;
    union {
        struct {
            pufs_status_t (*update)(pufs_sp38a_ctx*, uint8_t*, uint32_t*, const uint8_t*, uint32_t);
            pufs_status_t (*final)(pufs_sp38a_ctx*, uint8_t*, uint32_t*);
        } sp38a;
        struct {
            uint8_t encrypt;
            uint8_t taglen;
            pufs_status_t (*update)(pufs_sp38c_ctx*, uint8_t*, uint32_t*, const uint8_t*, uint32_t);
            pufs_status_t (*final)(pufs_sp38c_ctx*, uint8_t*, uint32_t*, uint8_t*);
        } sp38c;
        struct {
            uint8_t encrypt;
            pufs_status_t (*update)(pufs_sp38d_ctx*, uint8_t*, uint32_t*, const uint8_t*, uint32_t);
            pufs_status_t (*final)(pufs_sp38d_ctx*, uint8_t*, uint32_t*, uint8_t*, uint32_t);
        } sp38d;
        struct {
            pufs_status_t (*update)(pufs_sp38e_ctx*, uint8_t*, uint32_t*, const uint8_t*, uint32_t);
            pufs_status_t (*final)(pufs_sp38e_ctx*, uint8_t*, uint32_t*);
        } sp38e;
    };
};

static struct pufs_device pufs_dev;
static struct rt_device hwrng_dev;

static int get_from(void* dst, void* src, size_t size)
{
    if (!dst || !src || !size)
        return 0;

    if (lwp_get_from_user(dst, src, size) == 0)
        memcpy(dst, src, size);

    return size;
}

static int put_to(void* dst, void* src, size_t size)
{
    if (!dst || !src || !size)
        return 0;

    if (lwp_put_to_user(dst, src, size) == 0)
        memcpy(dst, src, size);

    return size;
}

static int pufs_init(void)
{
    if (pufs_dev.busy)
        return -EBUSY;
    if (0 != kd_hardlock_lock(pufs_dev.hardlock))
        return -EBUSY;
    pufs_dev.type = PUFS_TYPE_NONE;
    pufs_dev.ctx = NULL;
    pufs_dev.busy = 1;

    return 0;
}

static int pufs_deinit(void)
{
    if (pufs_dev.busy == 0)
        return 0;
    if (pufs_dev.type == PUFS_TYPE_HASH) {
        pufs_hash_ctx_free(pufs_dev.ctx);
    } else if (pufs_dev.type == PUFS_TYPE_SP38A) {
        pufs_sp38a_ctx_free(pufs_dev.ctx);
    } else if (pufs_dev.type == PUFS_TYPE_SP38D) {
        pufs_sp38d_ctx_free(pufs_dev.ctx);
    } else if (pufs_dev.type == PUFS_TYPE_SP38C) {
        pufs_sp38c_ctx_free(pufs_dev.ctx);
    } else if (pufs_dev.type == PUFS_TYPE_SP38E) {
        pufs_sp38e_ctx_free(pufs_dev.ctx);
    } else if (pufs_dev.type == PUFS_TYPE_HMAC) {
        pufs_hmac_ctx_free(pufs_dev.ctx);
    } else if (pufs_dev.type == PUFS_TYPE_CMAC) {
        pufs_cmac_ctx_free(pufs_dev.ctx);
    }
    pufs_dev.type = PUFS_TYPE_NONE;
    pufs_dev.ctx = NULL;
    kd_hardlock_unlock(HARDLOCK_HASH);
    pufs_dev.busy = 0;

    return 0;
}

int uid_get(pufs_uid_get_t* arg)
{
    int ret;
    pufs_uid_st uid;

    ret = pufs_get_uid(&uid, arg->slot);
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }

    put_to(arg->uid, &uid, sizeof(uid));

    return 0;
}

int key_io(pufs_key_io_t* arg)
{
    int ret;
    uint8_t mode = arg->mode;
    pufs_key_type_t keytype = arg->keytype;
    pufs_ka_slot_t keyslot = arg->keyslot;
    uint8_t* keyaddr = arg->keyaddr;
    uint32_t keybits = arg->keybits;
    uint8_t keytmp[SW_KEY_MAXLEN];

    ret = pufs_init();
    if (ret != 0)
        return ret;

    keybits = keybits > SW_KEY_MAXLEN * 8 ? SW_KEY_MAXLEN * 8 : keybits;
    if (mode == KM_IMPORT_PT || mode == KM_IMPORT_WRAP) {
        get_from(keytmp, keyaddr, (keybits + 7) >> 3);
        keyaddr = keytmp;
        if (mode == KM_IMPORT_PT)
            ret = pufs_import_plaintext_key(keytype, keyslot, keyaddr, keybits);
        else
            ret = pufs_import_wrapped_key(keytype, keyslot, keyaddr, keybits, arg->kwslot, arg->kwbits, arg->keywrap);
    } else if (mode == KM_EXPORT_WRAP) {
        ret = pufs_export_wrapped_key(keytype, keyslot, keytmp, keybits, arg->kwslot, arg->kwbits, arg->keywrap);
        if (ret == 0)
            put_to(keyaddr, keytmp, (keybits + 7) >> 3);
    } else if (mode == KM_CLEAR) {
        ret = pufs_clear_key(keytype, keyslot, keybits);
    } else {
        pufs_deinit();
        return -EINVAL;
    }

    pufs_deinit();
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }

    return 0;
}

int key_derive(pufs_key_derive_t* arg)
{
    int ret;
    pufs_key_type_t keytype = arg->ztype;
    uint32_t keybits = arg->zbits;
    uint8_t* keyaddr = arg->zaddr;
    uint8_t* iv = arg->iv;
    uint8_t* salt = arg->salt;
    uint32_t saltlen = arg->saltlen;
    uint8_t keytmp[SW_KEY_MAXLEN];
    uint8_t ivtmp[BC_BLOCK_SIZE];
    uint8_t salttmp[DGST_INT_STATE_LEN];

    keybits = keybits > SW_KEY_MAXLEN * 8 ? SW_KEY_MAXLEN * 8 : keybits;
    saltlen = saltlen > DGST_INT_STATE_LEN ? DGST_INT_STATE_LEN : saltlen;
    if (keytype == SWKEY) {
        get_from(keytmp, keyaddr, (keybits + 7) >> 3);
        keyaddr = keytmp;
    }
    if (iv) {
        get_from(ivtmp, iv, BC_BLOCK_SIZE);
        iv = ivtmp;
    }
    if (salt) {
        get_from(salttmp, salt, saltlen);
        salt = salttmp;
    }

    ret = pufs_init();
    if (ret != 0)
        return ret;

    ret = pufs_kdf_base(arg->keytype, arg->keyslot, arg->outbits, arg->method,
        arg->prf, arg->hash, arg->iter, arg->feedback, iv, arg->ctrpos,
        arg->ctrlen, keytype, (uint64_t)keyaddr, keybits, salt, saltlen,
        arg->info, arg->infolen, arg->out);

    pufs_deinit();
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }

    return 0;
}

int hash_init(pufs_hash_init_t* arg)
{
    int ret;

    ret = pufs_init();
    if (ret != 0)
        return ret;

    pufs_hash_ctx* hash_ctx = pufs_hash_ctx_new();
    if (hash_ctx == NULL) {
        pufs_deinit();
        return -ENOMEM;
    }
    pufs_dev.ctx = hash_ctx;
    pufs_dev.type = PUFS_TYPE_HASH;
    ret = pufs_hash_init(hash_ctx, arg->mode);
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        pufs_deinit();
        return -ret;
    }

    return 0;
}

int hash_update(pufs_hash_update_t* arg)
{
    int ret;

    if (pufs_dev.busy == 0 || pufs_dev.type != PUFS_TYPE_HASH || pufs_dev.ctx == NULL) {
        LOG_E("hash context no init\n");
        return -EPERM;
    }

    ret = pufs_hash_update(pufs_dev.ctx, arg->msg, arg->msglen);
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        pufs_deinit();
        return -ret;
    }

    return 0;
}

int hash_final(pufs_hash_final_t* arg)
{
    int ret;
    pufs_dgst_st md;

    if (pufs_dev.busy == 0 || pufs_dev.type != PUFS_TYPE_HASH || pufs_dev.ctx == NULL) {
        LOG_E("hash context no init\n");
        return -EPERM;
    }

    ret = pufs_hash_final(pufs_dev.ctx, &md);
    pufs_deinit();

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        ret = -ret;
    } else {
        put_to(arg->dgst, md.dgst, md.dlen);
        put_to(arg->dlen, &md.dlen, sizeof(md.dlen));
    }

    return ret;
}

int hash_deinit(void)
{
    if (pufs_dev.busy == 0 || pufs_dev.type != PUFS_TYPE_HASH || pufs_dev.ctx == NULL)
        return 0;

    pufs_deinit();

    return 0;
}

int mac_init(pufs_mac_init_t* arg)
{
    int ret;
    uint8_t cipher, mode;
    uint32_t keybits;
    pufs_key_type_t keytype;
    uint8_t* keyaddr;
    uint8_t keytmp[SW_KEY_MAXLEN];

    ret = pufs_init();
    if (ret != 0)
        return ret;

    cipher = arg->cipher;
    mode = arg->mode;
    keybits = arg->keybits;
    keytype = arg->keytype;
    keyaddr = arg->keyaddr;
    keybits = keybits > SW_KEY_MAXLEN * 8 ? SW_KEY_MAXLEN * 8 : keybits;

    if (keytype == SWKEY) {
        get_from(keytmp, keyaddr, (keybits + 7) >> 3);
        keyaddr = keytmp;
    }

    if (cipher == MAC_HMAC) {
        pufs_hmac_ctx* hmac_ctx = pufs_hmac_ctx_new();
        if (hmac_ctx == NULL) {
            pufs_deinit();
            return -ENOMEM;
        }
        pufs_dev.ctx = hmac_ctx;
        pufs_dev.type = PUFS_TYPE_HMAC;
        ret = pufs_hmac_init(hmac_ctx, mode, keytype, keyaddr, keybits);
        if (ret != SUCCESS) {
            LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
            pufs_deinit();
            return -ret;
        }
    } else if (cipher == MAC_CMAC) {
        pufs_cmac_ctx* cmac_ctx = pufs_cmac_ctx_new();
        if (cmac_ctx == NULL) {
            pufs_deinit();
            return -ENOMEM;
        }
        pufs_dev.ctx = cmac_ctx;
        pufs_dev.type = PUFS_TYPE_CMAC;
        ret = pufs_cmac_init(cmac_ctx, mode, keytype, keyaddr, keybits);
        if (ret != SUCCESS) {
            LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
            pufs_deinit();
            return -ret;
        }
    } else {
        LOG_D("%s: cipher invalid\n", __func__);
        pufs_deinit();
        return -EINVAL;
    }

    return ret;
}

int mac_update(pufs_mac_update_t* arg)
{
    int ret = 0;
    uint8_t type = pufs_dev.type;

    if (pufs_dev.busy == 0 || pufs_dev.ctx == NULL || (pufs_dev.type != PUFS_TYPE_HMAC && pufs_dev.type != PUFS_TYPE_CMAC)) {
        LOG_E("mac context no init\n");
        return -EPERM;
    }

    if (type == PUFS_TYPE_HMAC)
        ret = pufs_hmac_update(pufs_dev.ctx, arg->msg, arg->msglen);
    else if (type == PUFS_TYPE_CMAC)
        ret = pufs_cmac_update(pufs_dev.ctx, arg->msg, arg->msglen);
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        pufs_deinit();
        return -ret;
    }

    return 0;
}

int mac_final(pufs_mac_final_t* arg)
{
    int ret = 0;
    pufs_dgst_st md;
    uint8_t type = pufs_dev.type;

    if (pufs_dev.busy == 0 || pufs_dev.ctx == NULL || (pufs_dev.type != PUFS_TYPE_HMAC && pufs_dev.type != PUFS_TYPE_CMAC)) {
        LOG_E("mac context no init\n");
        return -EPERM;
    }

    if (type == PUFS_TYPE_HMAC)
        ret = pufs_hmac_final(pufs_dev.ctx, &md);
    else if (type == PUFS_TYPE_CMAC)
        ret = pufs_cmac_final(pufs_dev.ctx, &md);
    pufs_deinit();

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        ret = -ret;
    } else {
        put_to(arg->dgst, md.dgst, md.dlen);
        put_to(arg->dlen, &md.dlen, sizeof(md.dlen));
    }

    return ret;
}

int mac_deinit(void)
{
    if (pufs_dev.busy == 0 || pufs_dev.ctx == NULL || (pufs_dev.type != PUFS_TYPE_HMAC && pufs_dev.type != PUFS_TYPE_CMAC))
        return 0;

    pufs_deinit();

    return 0;
}

static int sp38a_init(pufs_sp38a_ctx* sp38a_ctx, pufs_skcipher_init_t* cfg)
{
    int ret = 0;
    pufs_cipher_t cipher = cfg->cipher;
    uint8_t mode = cfg->mode;
    uint8_t encrypt = cfg->encrypt;
    uint32_t keybits = cfg->aes.keybits;
    pufs_key_type_t keytype = cfg->aes.keytype;
    uint8_t* keyaddr = cfg->aes.keyaddr;
    uint8_t* iv = cfg->aes.iv;
    uint32_t ivlen = cfg->aes.ivlen;
    uint8_t keytmp[SW_KEY_MAXLEN];
    uint8_t ivtmp[BC_BLOCK_SIZE];

    keybits = keybits > SW_KEY_MAXLEN * 8 ? SW_KEY_MAXLEN * 8 : keybits;
    ivlen = ivlen > BC_BLOCK_SIZE ? BC_BLOCK_SIZE : ivlen;

    if (keytype == SWKEY) {
        get_from(keytmp, keyaddr, (keybits + 7) >> 3);
        keyaddr = keytmp;
    }

    if (mode != MODE_ECB) {
        get_from(ivtmp, iv, ivlen);
        iv = ivtmp;
    }

    if (mode == MODE_ECB) {
        if (encrypt) {
            ret = pufs_enc_ecb_init(sp38a_ctx, cipher, keytype, keyaddr, keybits);
            pufs_dev.sp38a.update = pufs_enc_ecb_update;
            pufs_dev.sp38a.final = pufs_enc_ecb_final;
        } else {
            ret = pufs_dec_ecb_init(sp38a_ctx, cipher, keytype, keyaddr, keybits);
            pufs_dev.sp38a.update = pufs_dec_ecb_update;
            pufs_dev.sp38a.final = pufs_dec_ecb_final;
        }
    } else if (mode == MODE_CFB) {
        if (encrypt) {
            ret = pufs_enc_cfb_init(sp38a_ctx, cipher, keytype, keyaddr, keybits, iv);
            pufs_dev.sp38a.update = pufs_enc_cfb_update;
            pufs_dev.sp38a.final = pufs_enc_cfb_final;
        } else {
            ret = pufs_dec_cfb_init(sp38a_ctx, cipher, keytype, keyaddr, keybits, iv);
            pufs_dev.sp38a.update = pufs_dec_cfb_update;
            pufs_dev.sp38a.final = pufs_dec_cfb_final;
        }
    } else if (mode == MODE_OFB) {
        if (encrypt) {
            ret = pufs_enc_ofb_init(sp38a_ctx, cipher, keytype, keyaddr, keybits, iv);
            pufs_dev.sp38a.update = pufs_enc_ofb_update;
            pufs_dev.sp38a.final = pufs_enc_ofb_final;
        } else {
            ret = pufs_dec_ofb_init(sp38a_ctx, cipher, keytype, keyaddr, keybits, iv);
            pufs_dev.sp38a.update = pufs_dec_ofb_update;
            pufs_dev.sp38a.final = pufs_dec_ofb_final;
        }
    } else if (mode >= MODE_CBC && mode <= MODE_CBC_CS3) {
        int cs = mode - MODE_CBC;
        if (encrypt) {
            ret = pufs_enc_cbc_init(sp38a_ctx, cipher, keytype, keyaddr, keybits, iv, cs);
            pufs_dev.sp38a.update = pufs_enc_cbc_update;
            pufs_dev.sp38a.final = pufs_enc_cbc_final;
        } else {
            ret = pufs_dec_cbc_init(sp38a_ctx, cipher, keytype, keyaddr, keybits, iv, cs);
            pufs_dev.sp38a.update = pufs_dec_cbc_update;
            pufs_dev.sp38a.final = pufs_dec_cbc_final;
        }
    } else if (mode >= MODE_CTR_32 && mode <= MODE_CTR) {
        int ctrlen = mode == MODE_CTR_32 ? 32 : mode == MODE_CTR_64 ? 64
                                                                    : 128;
        if (encrypt) {
            ret = pufs_enc_ctr_init(sp38a_ctx, cipher, keytype, keyaddr, keybits, iv, ctrlen);
            pufs_dev.sp38a.update = pufs_enc_ctr_update;
            pufs_dev.sp38a.final = pufs_enc_ctr_final;
        } else {
            ret = pufs_dec_ctr_init(sp38a_ctx, cipher, keytype, keyaddr, keybits, iv, ctrlen);
            pufs_dev.sp38a.update = pufs_dec_ctr_update;
            pufs_dev.sp38a.final = pufs_dec_ctr_final;
        }
    } else {
        LOG_D("%s: mode invalid\n", __func__);
        pufs_deinit();
        return EINVAL;
    }

    return ret;
}

static int sp38c_init(pufs_sp38c_ctx* sp38c_ctx, pufs_skcipher_init_t* cfg)
{
    int ret = 0;
    pufs_cipher_t cipher = cfg->cipher;
    uint8_t mode = cfg->mode;
    uint8_t encrypt = cfg->encrypt;
    uint32_t keybits = cfg->aes_ccm.keybits;
    pufs_key_type_t keytype = cfg->aes_ccm.keytype;
    uint8_t* keyaddr = cfg->aes_ccm.keyaddr;
    uint8_t* nonce = cfg->aes_ccm.nonce;
    uint32_t noncelen = cfg->aes_ccm.noncelen;
    uint32_t aadlen = cfg->aes_ccm.aadlen;
    uint32_t ptlen = cfg->aes_ccm.inlen;
    uint32_t taglen = cfg->aes_ccm.taglen;
    uint8_t keytmp[SW_KEY_MAXLEN];
    uint8_t noncetmp[BC_BLOCK_SIZE];

    keybits = keybits > SW_KEY_MAXLEN * 8 ? SW_KEY_MAXLEN * 8 : keybits;
    noncelen = noncelen > BC_BLOCK_SIZE ? BC_BLOCK_SIZE : noncelen;

    if (keytype == SWKEY) {
        get_from(keytmp, keyaddr, (keybits + 7) >> 3);
        keyaddr = keytmp;
    }

    get_from(noncetmp, nonce, noncelen);
    nonce = noncetmp;

    if (encrypt) {
        ret = pufs_enc_ccm_init(sp38c_ctx, cipher, keytype, keyaddr, keybits, nonce, noncelen, aadlen, ptlen, taglen);
        pufs_dev.sp38c.update = pufs_enc_ccm_update;
        pufs_dev.sp38c.final = pufs_enc_ccm_final;
    } else {
        ret = pufs_dec_ccm_init(sp38c_ctx, cipher, keytype, keyaddr, keybits, nonce, noncelen, aadlen, ptlen, taglen);
        pufs_dev.sp38c.update = pufs_dec_ccm_update;
        pufs_dev.sp38c.final = pufs_dec_ccm_final_tag;
    }
    pufs_dev.sp38c.encrypt = encrypt;
    pufs_dev.sp38c.taglen = taglen;

    return ret;
}

static int sp38d_init(pufs_sp38d_ctx* sp38d_ctx, pufs_skcipher_init_t* cfg)
{
    int ret = 0;
    pufs_cipher_t cipher = cfg->cipher;
    uint8_t mode = cfg->mode;
    uint8_t encrypt = cfg->encrypt;
    uint32_t keybits = cfg->aes_gcm.keybits;
    pufs_key_type_t keytype = cfg->aes_gcm.keytype;
    uint8_t* keyaddr = cfg->aes_gcm.keyaddr;
    uint8_t* iv = cfg->aes_gcm.iv;
    uint32_t ivlen = cfg->aes_gcm.ivlen;
    uint8_t keytmp[SW_KEY_MAXLEN];
    uint8_t ivtmp[12];

    keybits = keybits > SW_KEY_MAXLEN * 8 ? SW_KEY_MAXLEN * 8 : keybits;

    if (keytype == SWKEY) {
        get_from(keytmp, keyaddr, (keybits + 7) >> 3);
        keyaddr = keytmp;
    }

    if (ivlen == 12) {
        get_from(ivtmp, iv, ivlen);
        iv = ivtmp;
    }

    if (encrypt) {
        ret = pufs_enc_gcm_init(sp38d_ctx, cipher, keytype, keyaddr, keybits, iv, ivlen);
        pufs_dev.sp38d.update = pufs_enc_gcm_update;
        pufs_dev.sp38d.final = pufs_enc_gcm_final;
    } else {
        ret = pufs_dec_gcm_init(sp38d_ctx, cipher, keytype, keyaddr, keybits, iv, ivlen);
        pufs_dev.sp38d.update = pufs_dec_gcm_update;
        pufs_dev.sp38d.final = pufs_dec_gcm_final_tag;
    }
    pufs_dev.sp38d.encrypt = encrypt;

    return ret;
}

static int sp38e_init(pufs_sp38e_ctx* sp38e_ctx, pufs_skcipher_init_t* cfg)
{
    int ret = 0;
    pufs_cipher_t cipher = cfg->cipher;
    uint8_t mode = cfg->mode;
    uint8_t encrypt = cfg->encrypt;
    uint32_t keybits = cfg->aes_xts.keybits;
    pufs_key_type_t keytype1 = cfg->aes_xts.keytype1;
    uint8_t* keyaddr1 = cfg->aes_xts.keyaddr1;
    pufs_key_type_t keytype2 = cfg->aes_xts.keytype2;
    uint8_t* keyaddr2 = cfg->aes_xts.keyaddr2;
    uint8_t* iv = cfg->aes_xts.iv;
    uint32_t ivlen = cfg->aes_xts.ivlen;
    uint8_t keytmp1[SW_KEY_MAXLEN];
    uint8_t keytmp2[SW_KEY_MAXLEN];
    uint8_t ivtmp[BC_BLOCK_SIZE];

    keybits = keybits > SW_KEY_MAXLEN * 8 ? SW_KEY_MAXLEN * 8 : keybits;
    ivlen = ivlen > BC_BLOCK_SIZE ? BC_BLOCK_SIZE : ivlen;

    if (keytype1 == SWKEY) {
        get_from(keytmp1, keyaddr1, (keybits + 7) >> 3);
        keyaddr1 = keytmp1;
    }
    if (keytype2 == SWKEY) {
        get_from(keytmp2, keyaddr2, (keybits + 7) >> 3);
        keyaddr2 = keytmp2;
    }

    get_from(ivtmp, iv, ivlen);
    iv = ivtmp;

    if (encrypt) {
        ret = pufs_enc_xts_init(sp38e_ctx, cipher, keytype1, keyaddr1, keybits, keytype2, keyaddr2, iv, 0);
        pufs_dev.sp38e.update = pufs_enc_xts_update;
        pufs_dev.sp38e.final = pufs_enc_xts_final;
    } else {
        ret = pufs_dec_xts_init(sp38e_ctx, cipher, keytype1, keyaddr1, keybits, keytype2, keyaddr2, iv, 0);
        pufs_dev.sp38e.update = pufs_dec_xts_update;
        pufs_dev.sp38e.final = pufs_dec_xts_final;
    }

    return ret;
}

int skcipher_init(pufs_skcipher_init_t* arg)
{
    int ret;
    uint8_t mode;

    ret = pufs_init();
    if (ret != 0)
        return ret;

    mode = arg->mode;
    if (mode >= MODE_ECB && mode <= MODE_CTR) {
        pufs_sp38a_ctx* sp38a_ctx = pufs_sp38a_ctx_new();
        if (sp38a_ctx == NULL) {
            pufs_deinit();
            return -ENOMEM;
        }
        pufs_dev.ctx = sp38a_ctx;
        pufs_dev.type = PUFS_TYPE_SP38A;
        ret = sp38a_init(sp38a_ctx, arg);
        if (ret != SUCCESS) {
            LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
            pufs_deinit();
            return -ret;
        }
    } else if (mode == MODE_GCM) {
        pufs_sp38d_ctx* sp38d_ctx = pufs_sp38d_ctx_new();
        if (sp38d_ctx == NULL) {
            pufs_deinit();
            return -ENOMEM;
        }
        pufs_dev.ctx = sp38d_ctx;
        pufs_dev.type = PUFS_TYPE_SP38D;
        ret = sp38d_init(sp38d_ctx, arg);
        if (ret != SUCCESS) {
            LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
            pufs_deinit();
            return -ret;
        }
    } else if (mode == MODE_CCM) {
        pufs_sp38c_ctx* sp38c_ctx = pufs_sp38c_ctx_new();
        if (sp38c_ctx == NULL) {
            pufs_deinit();
            return -ENOMEM;
        }
        pufs_dev.ctx = sp38c_ctx;
        pufs_dev.type = PUFS_TYPE_SP38C;
        ret = sp38c_init(sp38c_ctx, arg);
        if (ret != SUCCESS) {
            LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
            pufs_deinit();
            return -ret;
        }
    } else if (mode == MODE_XTS) {
        pufs_sp38e_ctx* sp38e_ctx = pufs_sp38e_ctx_new();
        if (sp38e_ctx == NULL) {
            pufs_deinit();
            return -ENOMEM;
        }
        pufs_dev.ctx = sp38e_ctx;
        pufs_dev.type = PUFS_TYPE_SP38E;
        ret = sp38e_init(sp38e_ctx, arg);
        if (ret != SUCCESS) {
            LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
            pufs_deinit();
            return -ret;
        }
    } else {
        LOG_D("%s: mode invalid\n", __func__);
        pufs_deinit();
        return -EINVAL;
    }

    return 0;
}

int skcipher_update(pufs_skcipher_update_t* arg)
{
    int ret;
    uint32_t outlen;
    uint8_t type = pufs_dev.type;

    if (pufs_dev.busy == 0 || pufs_dev.ctx == NULL || (type != PUFS_TYPE_SP38A && type != PUFS_TYPE_SP38C && type != PUFS_TYPE_SP38D && type != PUFS_TYPE_SP38E)) {
        LOG_E("skcipher context no init\n");
        return -EPERM;
    }

    if (type == PUFS_TYPE_SP38A)
        ret = pufs_dev.sp38a.update(pufs_dev.ctx, arg->out, &outlen, arg->in, arg->inlen);
    else if (type == PUFS_TYPE_SP38D)
        ret = pufs_dev.sp38d.update(pufs_dev.ctx, arg->out, &outlen, arg->in, arg->inlen);
    else if (type == PUFS_TYPE_SP38C)
        ret = pufs_dev.sp38c.update(pufs_dev.ctx, arg->out, &outlen, arg->in, arg->inlen);
    else if (type == PUFS_TYPE_SP38E)
        ret = pufs_dev.sp38e.update(pufs_dev.ctx, arg->out, &outlen, arg->in, arg->inlen);

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        pufs_deinit();
        return -ret;
    } else {
        put_to(arg->outlen, &outlen, sizeof(outlen));
    }

    return 0;
}

int skcipher_final(pufs_skcipher_final_t* arg)
{
    int ret;
    uint32_t outlen;
    uint8_t type = pufs_dev.type;

    if (pufs_dev.busy == 0 || pufs_dev.ctx == NULL || (type != PUFS_TYPE_SP38A && type != PUFS_TYPE_SP38C && type != PUFS_TYPE_SP38D && type != PUFS_TYPE_SP38E)) {
        LOG_E("skcipher context no init\n");
        return -EPERM;
    }

    if (type == PUFS_TYPE_SP38A) {
        ret = pufs_dev.sp38a.final(pufs_dev.ctx, arg->out, &outlen);
        pufs_deinit();
    } else if (type == PUFS_TYPE_SP38D) {
        uint32_t taglen = arg->taglen;
        uint8_t tag[DGST_INT_STATE_LEN];
        taglen = taglen > DGST_INT_STATE_LEN ? DGST_INT_STATE_LEN : taglen;
        ret = pufs_dev.sp38d.final(pufs_dev.ctx, arg->out, &outlen, tag, taglen);
        pufs_deinit();
        if (ret == 0) {
            if (pufs_dev.sp38d.encrypt) {
                put_to(arg->tag, tag, taglen);
            } else {
                uint8_t tagtmp[DGST_INT_STATE_LEN];
                get_from(tagtmp, arg->tag, taglen);
                if (memcmp(tag, tagtmp, taglen)) {
                    put_to(arg->outlen, &outlen, sizeof(outlen));
                    ret = E_VERFAIL;
                }
            }
        }
    } else if (type == PUFS_TYPE_SP38C) {
        uint32_t taglen = pufs_dev.sp38c.taglen;
        uint8_t tag[DGST_INT_STATE_LEN];
        ret = pufs_dev.sp38c.final(pufs_dev.ctx, arg->out, &outlen, tag);
        pufs_deinit();
        if (ret == 0) {
            if (pufs_dev.sp38c.encrypt) {
                put_to(arg->tag, tag, taglen);
            } else {
                uint8_t tagtmp[DGST_INT_STATE_LEN];
                get_from(tagtmp, arg->tag, taglen);
                if (memcmp(tag, tagtmp, taglen)) {
                    put_to(arg->outlen, &outlen, sizeof(outlen));
                    ret = E_VERFAIL;
                }
            }
        }
    } else if (type == PUFS_TYPE_SP38E) {
        ret = pufs_dev.sp38e.final(pufs_dev.ctx, arg->out, &outlen);
        pufs_deinit();
    }

    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        ret = -ret;
    } else {
        put_to(arg->outlen, &outlen, sizeof(outlen));
    }

    return ret;
}

int skcipher_deinit(void)
{
    uint8_t type = pufs_dev.type;

    if (pufs_dev.busy == 0 || pufs_dev.ctx == NULL || (type != PUFS_TYPE_SP38A && type != PUFS_TYPE_SP38C && type != PUFS_TYPE_SP38D && type != PUFS_TYPE_SP38E))
        return 0;

    pufs_deinit();

    return 0;
}

int ecc_prk_gen(pufs_ecc_prk_gen_t* arg)
{
    int ret;

    ret = pufs_init();
    if (ret != 0)
        return ret;

    ret = pufs_ecp_set_curve_byname(arg->ecctype);
    if (ret != SUCCESS)
        goto exit;

    if (arg->is_ephemeral) {
        ret = pufs_ecp_gen_eprk(arg->prkslot);
    } else {
        pufs_key_type_t keytype = arg->keytype;
        uint32_t keybits = arg->keybits;
        uint8_t* keyaddr = arg->keyaddr;
        uint8_t keytmp[SW_KEY_MAXLEN];
        keybits = keybits > SW_KEY_MAXLEN * 8 ? SW_KEY_MAXLEN * 8 : keybits;
        if (keytype == SWKEY) {
            get_from(keytmp, keyaddr, (keybits + 7) >> 3);
            keyaddr = keytmp;
        }
        ret = pufs_ecp_gen_sprk(arg->prkslot, keytype, (size_t)keyaddr, keybits,
            arg->salt, arg->saltlen, arg->info, arg->infolen, arg->hashtype);
    }
exit:
    pufs_deinit();
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }

    return 0;
}

int ecc_puk_gen(pufs_ecc_puk_gen_t* arg)
{
    int ret;
    pufs_ec_point_st puk;

    ret = pufs_init();
    if (ret != 0)
        return ret;

    ret = pufs_ecp_set_curve_byname(arg->ecctype);
    if (ret != SUCCESS)
        goto exit;

    ret = pufs_ecp_gen_puk(&puk, arg->prktype, arg->prkslot);
exit:
    pufs_deinit();
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    } else {
        put_to(arg->puk, &puk, sizeof(puk));
    }

    return 0;
}

int ecc_puk_verify(pufs_ecc_puk_verify_t* arg)
{
    int ret;
    pufs_ec_point_st puk;

    ret = pufs_init();
    if (ret != 0)
        return ret;

    ret = pufs_ecp_set_curve_byname(arg->ecctype);
    if (ret != SUCCESS)
        goto exit;

    get_from(&puk, arg->puk, sizeof(puk));
    ret = pufs_ecp_validate_puk(puk, true);
exit:
    pufs_deinit();
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }

    return 0;
}

int ecc_cdh(pufs_ecc_cdh_t* arg)
{
    int ret;
    pufs_ec_point_st puk_e;
    pufs_ec_point_st puk_s;
    uint8_t outtmp[QLEN_MAX];
    uint8_t* out;

    ret = pufs_init();
    if (ret != 0)
        return ret;

    ret = pufs_ecp_set_curve_byname(arg->ecctype);
    if (ret != SUCCESS)
        goto exit;

    out = arg->out ? outtmp : NULL;
    get_from(&puk_e, arg->puk_e, sizeof(pufs_ec_point_st));
    if (arg->is_ephemeral) {
        ret = pufs_ecp_ecccdh_2e(puk_e, arg->prkslot_e, out);
    } else {
        get_from(&puk_s, arg->puk_s, sizeof(pufs_ec_point_st));
        ret = pufs_ecp_ecccdh_2e2s(puk_e, puk_s, arg->prkslot_e,
            arg->prktype_s, arg->prkslot_s, out);
    }
exit:
    pufs_deinit();
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    } else {
        if (out)
            put_to(arg->out, out, ecc_param[arg->ecctype].len);
    }

    return 0;
}

int ecdsa_sign(pufs_ecdsa_sign_t* arg)
{
    int ret;
    pufs_dgst_st md;
    pufs_ecdsa_sig_st sig;

    ret = pufs_init();
    if (ret != 0)
        return ret;

    ret = pufs_ecp_set_curve_byname(arg->ecctype);
    if (ret != SUCCESS)
        goto exit;

    md.dlen = arg->mdlen > DLEN_MAX ? DLEN_MAX : arg->mdlen;
    get_from(md.dgst, arg->md, md.dlen);
    ret = pufs_ecp_ecdsa_sign_dgst(&sig, md, arg->prktype, arg->prkslot);
exit:
    pufs_deinit();
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    } else {
        put_to(arg->sig, &sig, sizeof(sig));
    }

    return 0;
}

int ecdsa_verify(pufs_ecdsa_verify_t* arg)
{
    int ret;
    pufs_dgst_st md;
    pufs_ecdsa_sig_st sig;
    pufs_ec_point_st puk;

    ret = pufs_init();
    if (ret != 0)
        return ret;

    ret = pufs_ecp_set_curve_byname(arg->ecctype);
    if (ret != SUCCESS)
        goto exit;

    md.dlen = arg->mdlen > DLEN_MAX ? DLEN_MAX : arg->mdlen;
    get_from(md.dgst, arg->md, md.dlen);
    get_from(&sig, arg->sig, sizeof(sig));
    if (arg->otpslot <= OTPKEY_31) {
        ret = pufs_ecp_ecdsa_verify_dgst_otpkey(sig, md, arg->otpslot);
    } else {
        get_from(&puk, arg->puk, sizeof(puk));
        ret = pufs_ecp_ecdsa_verify_dgst(sig, md, puk);
    }
exit:
    pufs_deinit();
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }

    return 0;
}

int sm2_sign(pufs_sm2_sign_t* arg)
{
    int ret;
    pufs_ecdsa_sig_st sig;

    ret = pufs_init();
    if (ret != 0)
        return ret;

    ret = pufs_sm2_sign(&sig, arg->msg, arg->msglen, arg->id, arg->idlen,
        arg->prktype, arg->prkslot);
    pufs_deinit();
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    } else {
        put_to(arg->sig, &sig, sizeof(sig));
    }

    return 0;
}

int sm2_verify(pufs_sm2_verify_t* arg)
{
    int ret;
    pufs_ecdsa_sig_st sig;
    pufs_ec_point_st puk;

    ret = pufs_init();
    if (ret != 0)
        return ret;

    get_from(&sig, arg->sig, sizeof(sig));
    get_from(&puk, arg->puk, sizeof(puk));
    ret = pufs_sm2_verify(sig, arg->msg, arg->msglen, arg->id, arg->idlen,
        puk);
    pufs_deinit();
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    }

    return 0;
}

int sm2_enc(pufs_sm2_enc_t* arg)
{
    int ret;
    uint32_t outlen;
    pufs_ec_point_st puk;

    ret = pufs_init();
    if (ret != 0)
        return ret;

    get_from(&puk, arg->puk, sizeof(puk));
    ret = pufs_sm2_enc(arg->out, &outlen, arg->in, arg->inlen, puk,
        arg->format);
    pufs_deinit();
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    } else {
        put_to(arg->outlen, &outlen, sizeof(outlen));
    }

    return 0;
}

int sm2_dec(pufs_sm2_dec_t* arg)
{
    int ret;
    uint32_t outlen;

    ret = pufs_init();
    if (ret != 0)
        return ret;

    ret = pufs_sm2_dec(arg->out, &outlen, arg->in, arg->inlen, arg->prkslot,
        arg->format);
    pufs_deinit();
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    } else {
        put_to(arg->outlen, &outlen, sizeof(outlen));
    }

    return 0;
}

int sm2_kex(pufs_sm2_kex_t* arg)
{
    int ret;
    pufs_dgst_st s2, s3;
    pufs_ec_point_st pukr, tpukr;

    ret = pufs_init();
    if (ret != 0)
        return ret;

    get_from(&pukr, arg->pukr, sizeof(pukr));
    get_from(&tpukr, arg->tpukr, sizeof(tpukr));
    ret = pufs_sm2_kex(&s2, &s3, arg->key, arg->keybits, arg->idl, arg->idllen,
        arg->idr, arg->idrlen, arg->prkslotl, arg->tprkslotl, pukr, tpukr,
        arg->init);
    pufs_deinit();
    if (ret != SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        return -ret;
    } else {
        put_to(arg->dlen2, &s2.dlen, sizeof(s2.dlen));
        put_to(arg->dgst2, s2.dgst, s2.dlen);
        put_to(arg->dlen3, &s3.dlen, sizeof(s3.dlen));
        put_to(arg->dgst3, s3.dgst, s3.dlen);
    }

    return 0;
}

int rsa_sign(pufs_rsa_sign_t* arg)
{
    int ret;
    uint8_t mode = arg->rsamode;
    uint8_t type = arg->rsatype;
    uint32_t elen = (type + 1) * 128;
    uint8_t *n, *prk, *sig;

    if (type < RSA_1024 || type > RSA_4096 || mode < RSA_BASE || mode > RSA_PSS)
        return -EINVAL;

    ret = pufs_init();
    if (ret != 0)
        return ret;

    n = rt_malloc(elen);
    prk = rt_malloc(elen);
    sig = rt_malloc(elen);
    if (n == NULL || prk == NULL || sig == NULL) {
        ret = -ENOMEM;
        goto exit;
    }

    get_from(n, arg->n, elen);
    get_from(prk, arg->prk, elen);

    if (mode == RSA_BASE) {
        uint8_t* msg = rt_malloc(elen);
        if (msg == NULL) {
            ret = -ENOMEM;
            goto exit;
        }
        get_from(msg, arg->msg, elen);
        ret = pufs_rsa_sign(sig, type, n, arg->puk, prk, msg);
        rt_free(msg);
    } else if (mode == RSA_X931) {
        ret = pufs_rsa_x931_sign(sig, type, n, arg->puk, prk, arg->hashtype,
            arg->msg, arg->msglen);
    } else if (mode == RSA_P1V15) {
        ret = pufs_rsa_p1v15_sign(sig, type, n, arg->puk, prk, arg->hashtype,
            arg->msg, arg->msglen);
    } else if (mode == RSA_PSS) {
        ret = pufs_rsa_pss_sign(sig, type, n, arg->puk, prk, arg->hashtype,
            arg->msg, arg->msglen, arg->salt, arg->saltlen);
    }
exit:
    pufs_deinit();
    if (ret > SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        ret = -ret;
    } else if (ret == SUCCESS) {
        put_to(arg->sig, sig, elen);
    }
    rt_free(n);
    rt_free(prk);
    rt_free(sig);

    return ret;
}

int rsa_verify(pufs_rsa_verify_t* arg)
{
    int ret;
    uint8_t mode = arg->rsamode;
    uint8_t type = arg->rsatype;
    uint32_t elen = (type + 1) * 128;
    uint8_t *n, *sig;

    if (type < RSA_1024 || type > RSA_4096 || mode < RSA_BASE || mode > RSA_PSS)
        return -EINVAL;

    ret = pufs_init();
    if (ret != 0)
        return ret;

    n = rt_malloc(elen);
    sig = rt_malloc(elen);
    if (n == NULL || sig == NULL) {
        ret = -ENOMEM;
        goto exit;
    }

    get_from(n, arg->n, elen);
    get_from(sig, arg->sig, elen);

    if (mode == RSA_BASE) {
        uint8_t* msg = rt_malloc(elen);
        if (msg == NULL) {
            ret = -ENOMEM;
            goto exit;
        }
        get_from(msg, arg->msg, elen);
        ret = pufs_rsa_verify(sig, type, n, arg->puk, msg);
        rt_free(msg);
    } else if (mode == RSA_X931) {
        ret = pufs_rsa_x931_verify(sig, type, n, arg->puk, arg->msg,
            arg->msglen);
    } else if (mode == RSA_P1V15) {
        ret = pufs_rsa_p1v15_verify(sig, type, n, arg->puk, arg->msg,
            arg->msglen);
    } else if (mode == RSA_PSS) {
        ret = pufs_rsa_pss_verify(sig, type, n, arg->puk, arg->hashtype,
            arg->msg, arg->msglen);
    }
exit:
    pufs_deinit();
    if (ret > SUCCESS) {
        LOG_D("%s: %s\n", __func__, pufs_strstatus(ret));
        ret = -ret;
    }
    rt_free(n);
    rt_free(sig);

    return ret;
}

static rt_err_t pufs_control(rt_device_t dev, int cmd, void* args)
{
    int ret;

    switch (cmd) {
    case PUFS_UID_GET: {
        pufs_uid_get_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = uid_get(&arg);
        break;
    }
    case PUFS_HASH_INIT: {
        pufs_hash_init_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = hash_init(&arg);
        break;
    }
    case PUFS_HASH_UPDATE: {
        pufs_hash_update_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = hash_update(&arg);
        break;
    }
    case PUFS_HASH_FINAL: {
        pufs_hash_final_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = hash_final(&arg);
        break;
    }
    case PUFS_MAC_INIT: {
        pufs_mac_init_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = mac_init(&arg);
        break;
    }
    case PUFS_MAC_UPDATE: {
        pufs_mac_update_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = mac_update(&arg);
        break;
    }
    case PUFS_MAC_FINAL: {
        pufs_mac_final_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = mac_final(&arg);
        break;
    }
    case PUFS_SKCIPHER_INIT: {
        pufs_skcipher_init_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = skcipher_init(&arg);
        break;
    }
    case PUFS_SKCIPHER_UPDATE: {
        pufs_skcipher_update_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = skcipher_update(&arg);
        break;
    }
    case PUFS_SKCIPHER_FINAL: {
        pufs_skcipher_final_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = skcipher_final(&arg);
        break;
    }
    case PUFS_KEY_INOUT: {
        pufs_key_io_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = key_io(&arg);
        break;
    }
    case PUFS_KEY_DERIVE: {
        pufs_key_derive_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = key_derive(&arg);
        break;
    }
    case PUFS_ECC_PRK_GEN: {
        pufs_ecc_prk_gen_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = ecc_prk_gen(&arg);
        break;
    }
    case PUFS_ECC_PUK_GEN: {
        pufs_ecc_puk_gen_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = ecc_puk_gen(&arg);
        break;
    }
    case PUFS_ECC_PUK_VERIFY: {
        pufs_ecc_puk_verify_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = ecc_puk_verify(&arg);
        break;
    }
    case PUFS_ECC_CDH: {
        pufs_ecc_cdh_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = ecc_cdh(&arg);
        break;
    }
    case PUFS_ECDSA_SIGN: {
        pufs_ecdsa_sign_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = ecdsa_sign(&arg);
        break;
    }
    case PUFS_ECDSA_VERIFY: {
        pufs_ecdsa_verify_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = ecdsa_verify(&arg);
        break;
    }
    case PUFS_SM2_SIGN: {
        pufs_sm2_sign_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = sm2_sign(&arg);
        break;
    }
    case PUFS_SM2_VERIFY: {
        pufs_sm2_verify_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = sm2_verify(&arg);
        break;
    }
    case PUFS_SM2_ENC: {
        pufs_sm2_enc_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = sm2_enc(&arg);
        break;
    }
    case PUFS_SM2_DEC: {
        pufs_sm2_dec_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = sm2_dec(&arg);
        break;
    }
    case PUFS_SM2_KEX: {
        pufs_sm2_kex_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = sm2_kex(&arg);
        break;
    }
    case PUFS_RSA_SIGN: {
        pufs_rsa_sign_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = rsa_sign(&arg);
        break;
    }
    case PUFS_RSA_VERIFY: {
        pufs_rsa_verify_t arg;
        get_from(&arg, args, sizeof(arg));
        ret = rsa_verify(&arg);
        break;
    }
    default:
        ret = -EINVAL;
    }
    return ret;
}

static rt_err_t pufs_open(rt_device_t dev, rt_uint16_t oflag)
{
    return RT_EOK;
}

static rt_err_t pufs_close(rt_device_t dev)
{
    pufs_deinit();

    return RT_EOK;
}

const static struct rt_device_ops pufs_ops = {
    RT_NULL,
    pufs_open,
    pufs_close,
    RT_NULL,
    RT_NULL,
    pufs_control,
};

static rt_size_t hwrng_read(rt_device_t dev, rt_off_t pos, void* buffer, rt_size_t size)
{
    rt_size_t len = size;

    while (((uint64_t)buffer & 0x3) && len) {
        *(uint8_t*)buffer = rt_regs->rn;
        buffer = (uint8_t*)buffer + 1;
        len--;
    }
    while (len >= 4) {
        *(uint32_t*)buffer = rt_regs->rn;
        buffer += 4;
        len -= 4;
    }
    while (len) {
        *(uint8_t*)buffer = rt_regs->rn;
        buffer = (uint8_t*)buffer + 1;
        len--;
    }

    return size;
}

const static struct rt_device_ops hwrng_ops = {
    .read = hwrng_read,
};

int pufs_device_init(void)
{
    int ret;

    pufs_dev.base = rt_ioremap((void*)SECURITY_BASE_ADDR, SECURITY_IO_SIZE);

    if (kd_request_lock(HARDLOCK_PUFS)) {
        LOG_E("fail to request hardlock-%d\n", HARDLOCK_PUFS);
        return -RT_ERROR;
    }
    pufs_dev.hardlock = HARDLOCK_PUFS;

    ret = rt_device_register(&pufs_dev.dev, "pufs", RT_DEVICE_FLAG_RDWR);

    pufs_dev.dev.ops = &pufs_ops;

    pufs_module_init((uintptr_t)pufs_dev.base, SECURITY_IO_SIZE);
    pufs_dma_module_init(DMA_ADDR_OFFSET, NULL);
    pufs_rt_module_init(RT_ADDR_OFFSET);
    pufs_ka_module_init(KA_ADDR_OFFSET);
    pufs_kwp_module_init(KWP_ADDR_OFFSET);
    pufs_crypto_module_init(CRYPTO_ADDR_OFFSET);
    pufs_hmac_module_init(HMAC_HASH_ADDR_OFFSET);
    pufs_cmac_module_init(CMAC_ADDR_OFFSET);
    pufs_kdf_module_init(KDF_ADDR_OFFSET);
    pufs_pkc_module_init(PKC_ADDR_OFFSET);
    pufs_sp38a_module_init(SP38A_ADDR_OFFSET);
    pufs_sp38c_module_init(SP38C_ADDR_OFFSET);
    pufs_sp38d_module_init(SP38D_ADDR_OFFSET);
    pufs_sp38e_module_init(SP38E_ADDR_OFFSET);
    pufs_drbg_module_init(SP90A_ADDR_OFFSET);
    pufs_rt_cde_init(CDE_ADDR_OFFSET);

    rt_device_register(&hwrng_dev, "hwrng", RT_DEVICE_FLAG_RDWR);
    hwrng_dev.ops = &hwrng_ops;

    return ret;
}
INIT_DEVICE_EXPORT(pufs_device_init);

static int file_hash(int mode, char* file)
{
    int f;
    ssize_t size;
    void* buf;
    int ret;
    uint8_t md[64];
    uint32_t mdlen;
    pufs_hash_init_t init;
    pufs_hash_update_t update;
    pufs_hash_final_t final;

    f = open(file, O_RDONLY);
    if (f < 0) {
        printf("open %s err!\n", file);
        return -ENOENT;
    }

    size = lseek(f, 0, SEEK_END);
    lseek(f, 0, SEEK_SET);
    buf = malloc(CHUNK_MAXLEN);
    if (buf == NULL) {
        printf("malloc err!\n");
        close(f);
        return -ENOMEM;
    }

    memset(md, 0, sizeof(md));
    init.mode = mode;
    ret = hash_init(&init);
    if (ret)
        goto exit;

    while (size) {
        update.msg = (void*)buf;
        update.msglen = size > CHUNK_MAXLEN ? CHUNK_MAXLEN : size;
        read(f, buf, update.msglen);
        size -= update.msglen;
        ret = hash_update(&update);
        if (ret)
            goto exit;
    }

    final.dgst = md;
    final.dlen = &mdlen;
    ret = hash_final(&final);
    if (ret)
        goto exit;

    for (int i = 0; i < mdlen; i++)
        printf("%02x", md[i]);
    printf("\n");

exit:
    close(f);
    free(buf);
    if (ret)
        printf("cmd err code: %d\n", ret);

    return ret;
}

static int sha224sum(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: %s file\n", __func__);
        return -EINVAL;
    }

    return file_hash(HASH_SHA_224, argv[1]);
}

static int sha256sum(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: %s file\n", __func__);
        return -EINVAL;
    }

    return file_hash(HASH_SHA_256, argv[1]);
}

static int sha384sum(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: %s file\n", __func__);
        return -EINVAL;
    }

    return file_hash(HASH_SHA_384, argv[1]);
}

static int sha512sum(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: %s file\n", __func__);
        return -EINVAL;
    }

    return file_hash(HASH_SHA_512, argv[1]);
}

MSH_CMD_EXPORT(sha224sum, file sha224);
MSH_CMD_EXPORT(sha256sum, file sha256);
MSH_CMD_EXPORT(sha384sum, file sha384);
MSH_CMD_EXPORT(sha512sum, file sha512);
