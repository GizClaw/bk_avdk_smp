// Copyright 2024-2025 Beken
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <stdint.h>
#include <stddef.h>

/* Provides MBEDTLS_GCM_DERIVED_MODEL_KEY and MBEDTLS_GCM_DERIVED_DEVICE_ROOT_KEY,
 * which select the OTP root key the ladder derives from. */
#include "mbedtls/gcm.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Recommended GCM IV length in bytes. */
#define KEY_LADDER_GCM_IV_LEN     12
/** Full GCM tag length in bytes. */
#define KEY_LADDER_GCM_TAG_LEN    16
/** Shortest accepted GCM tag length in bytes. */
#define KEY_LADDER_GCM_TAG_MIN    4

#define KEY_LADDER_OK              0
#define KEY_LADDER_ERR_PARAM      -1
#define KEY_LADDER_ERR_ENGINE     -2
#define KEY_LADDER_ERR_AUTH       -3

/**
 * AES-256-GCM encrypt with a key derived from the OTP root key.
 * The derived key is produced and used inside the crypto engine only.
 *
 * iv must be unique for every message encrypted under the same root.
 * aad is optional. plain_len may be 0 to compute an authentication tag
 * over aad only.
 *
 * \param key_type  MBEDTLS_GCM_DERIVED_MODEL_KEY or
 *                  MBEDTLS_GCM_DERIVED_DEVICE_ROOT_KEY.
 * \param iv        IV/nonce buffer, iv_len bytes.
 * \param iv_len    IV length in bytes, must be non-zero.
 * \param aad       Additional authenticated data, may be NULL when aad_len is 0.
 * \param aad_len   AAD length in bytes.
 * \param plain     Plaintext input, may be NULL when plain_len is 0.
 * \param plain_len Plaintext length in bytes.
 * \param cipher    Ciphertext output, size >= plain_len, may be NULL when plain_len is 0.
 * \param tag       Tag output buffer, tag_len bytes.
 * \param tag_len   Requested tag length, KEY_LADDER_GCM_TAG_MIN..KEY_LADDER_GCM_TAG_LEN.
 * \return KEY_LADDER_OK on success, KEY_LADDER_ERR_* on failure.
 */
int bk_key_ladder_gcm_encrypt(int key_type,
                              const uint8_t *iv, size_t iv_len,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *plain, size_t plain_len,
                              uint8_t *cipher,
                              uint8_t *tag, size_t tag_len);

/**
 * AES-256-GCM decrypt and verify tag with a key derived from the OTP root key.
 *
 * The same key_type, iv, aad and tag_len used for encryption must be supplied.
 * On tag mismatch the plaintext output is zeroized before returning.
 *
 * \param key_type   MBEDTLS_GCM_DERIVED_MODEL_KEY or
 *                   MBEDTLS_GCM_DERIVED_DEVICE_ROOT_KEY.
 * \param iv         IV/nonce buffer, iv_len bytes.
 * \param iv_len     IV length in bytes, must be non-zero.
 * \param aad        Additional authenticated data, may be NULL when aad_len is 0.
 * \param aad_len    AAD length in bytes.
 * \param cipher     Ciphertext input, may be NULL when cipher_len is 0.
 * \param cipher_len Ciphertext length in bytes.
 * \param plain      Plaintext output, size >= cipher_len, may be NULL when cipher_len is 0.
 * \param tag        Tag to verify, tag_len bytes.
 * \param tag_len    Tag length, KEY_LADDER_GCM_TAG_MIN..KEY_LADDER_GCM_TAG_LEN.
 * \return KEY_LADDER_OK on success, KEY_LADDER_ERR_AUTH on tag mismatch,
 *         other KEY_LADDER_ERR_* on failure.
 */
int bk_key_ladder_gcm_decrypt(int key_type,
                              const uint8_t *iv, size_t iv_len,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *cipher, size_t cipher_len,
                              uint8_t *plain,
                              const uint8_t *tag, size_t tag_len);

#ifdef __cplusplus
}
#endif
