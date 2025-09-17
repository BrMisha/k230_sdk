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

#ifndef __DRV_AES__
#define __DRV_AES__
#include <stdint.h>

#define RT_AES_INIT                 _IOWR('G', 0, int)
#define RT_AES_UPDATE               _IOWR('G', 1, int)
#define RT_AES_FINAL                _IOWR('G', 2, int)

union rt_aes_control_args {
    struct {
        uint8_t mode;
        uint8_t encrypt;
        uint8_t keytype;
        uint8_t keyslot;
        uint8_t *key;
        uint8_t *iv;
        uint32_t keylen;
        uint32_t ivlen;
    } init;
    struct {
        uint8_t *out;
        uint32_t *outlen;
        uint8_t *in;
        uint32_t inlen;
    } update;
    struct {
        uint8_t *out;
        uint32_t *outlen;
        uint8_t *tag;
        uint32_t taglen;
    } final;
};

#endif /*__DRV_AES__*/
