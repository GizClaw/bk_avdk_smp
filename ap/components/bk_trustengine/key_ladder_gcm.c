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

#include "sdkconfig.h"

#if CONFIG_TRUSTENGINE

#include "mbedtls/gcm.h"
#include "key_ladder_gcm.h"

/** EK1 bit length fed into the key ladder. EK2/EK3 are fixed to 128 bits. */
#define KEY_LADDER_EK1_BITS    256

/*
 * Key ladder derivation parameters. They are not secret, but changing any of
 * them changes the derived key, so data encrypted with the previous values can
 * no longer be decrypted.
 */
static const uint8_t g_ek1[32] = {
	0xcd, 0xbd, 0xd1, 0xc8, 0x28, 0x53, 0x55, 0x8a,
	0xc2, 0x0f, 0xcb, 0x5a, 0x7f, 0xe1, 0x1f, 0x78,
	0xd4, 0x6a, 0x6b, 0x91, 0xed, 0xc0, 0x85, 0x89,
	0xa8, 0xbe, 0x45, 0x11, 0x3f, 0x43, 0x1f, 0x55
};

/* EK2/EK3 match the key ladder tool defaults, so the tool can compute an EK1
 * that makes the ladder derive a given target key. */
static const uint8_t g_ek2[16] = {
	0x49, 0xC0, 0x68, 0x83, 0xF2, 0x98, 0x63, 0x7D,
	0x8D, 0x2F, 0x1B, 0x3C, 0xF5, 0x5D, 0x1A, 0xC4
};

static const uint8_t g_ek3[16] = {
	0x35, 0x09, 0x32, 0x0F, 0xF9, 0x69, 0x91, 0x4F,
	0x2F, 0x76, 0x9F, 0xBC, 0x1E, 0xF1, 0x41, 0x67
};

_Static_assert(sizeof(g_ek1) * 8 == KEY_LADDER_EK1_BITS,
               "EK1 constant size does not match KEY_LADDER_EK1_BITS");

/** Map an mbedtls GCM return value onto a KEY_LADDER_* code. */
static int key_ladder_map_err(int ret)
{
	switch (ret) {
	case 0:
		return KEY_LADDER_OK;
	case MBEDTLS_ERR_GCM_AUTH_FAILED:
		return KEY_LADDER_ERR_AUTH;
	case MBEDTLS_ERR_GCM_BAD_INPUT:
	case MBEDTLS_ERR_GCM_BUFFER_TOO_SMALL:
		return KEY_LADDER_ERR_PARAM;
	default:
		return KEY_LADDER_ERR_ENGINE;
	}
}

/** Validate the arguments shared by the encrypt and decrypt entries. */
static int key_ladder_check_args(int key_type,
                                 const uint8_t *iv, size_t iv_len,
                                 const uint8_t *aad, size_t aad_len,
                                 const uint8_t *in, size_t in_len,
                                 const uint8_t *out,
                                 const uint8_t *tag, size_t tag_len)
{
	if (key_type != MBEDTLS_GCM_DERIVED_MODEL_KEY &&
	    key_type != MBEDTLS_GCM_DERIVED_DEVICE_ROOT_KEY)
		return KEY_LADDER_ERR_PARAM;

	if (iv == NULL || iv_len == 0)
		return KEY_LADDER_ERR_PARAM;

	if (tag == NULL || tag_len < KEY_LADDER_GCM_TAG_MIN ||
	    tag_len > KEY_LADDER_GCM_TAG_LEN)
		return KEY_LADDER_ERR_PARAM;

	if (aad_len > 0 && aad == NULL)
		return KEY_LADDER_ERR_PARAM;

	if (in_len > 0 && (in == NULL || out == NULL))
		return KEY_LADDER_ERR_PARAM;

	return KEY_LADDER_OK;
}

/** Initialize a GCM context bound to the key derived from the given root. */
static int key_ladder_gcm_setup(mbedtls_gcm_context *ctx, int key_type)
{
	int ret;

	mbedtls_gcm_init(ctx);

	/* The derivation parameters are always valid, so any failure here comes
	 * from the engine or from context allocation. */
	ret = mbedtls_gcm_set_derived_key(ctx, MBEDTLS_CIPHER_ID_AES, key_type,
	                                  g_ek1, g_ek2, g_ek3, KEY_LADDER_EK1_BITS);
	if (ret != 0) {
		mbedtls_gcm_free(ctx);
		return KEY_LADDER_ERR_ENGINE;
	}

	return KEY_LADDER_OK;
}

int bk_key_ladder_gcm_encrypt(int key_type,
                              const uint8_t *iv, size_t iv_len,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *plain, size_t plain_len,
                              uint8_t *cipher,
                              uint8_t *tag, size_t tag_len)
{
	mbedtls_gcm_context ctx;
	int ret;

	ret = key_ladder_check_args(key_type, iv, iv_len, aad, aad_len,
	                            plain, plain_len, cipher, tag, tag_len);
	if (ret != KEY_LADDER_OK)
		return ret;

	ret = key_ladder_gcm_setup(&ctx, key_type);
	if (ret != KEY_LADDER_OK)
		return ret;

	ret = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, plain_len,
	                                iv, iv_len, aad, aad_len,
	                                plain, cipher, tag_len, tag);
	mbedtls_gcm_free(&ctx);

	return key_ladder_map_err(ret);
}

int bk_key_ladder_gcm_decrypt(int key_type,
                              const uint8_t *iv, size_t iv_len,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *cipher, size_t cipher_len,
                              uint8_t *plain,
                              const uint8_t *tag, size_t tag_len)
{
	mbedtls_gcm_context ctx;
	int ret;

	ret = key_ladder_check_args(key_type, iv, iv_len, aad, aad_len,
	                            cipher, cipher_len, plain, tag, tag_len);
	if (ret != KEY_LADDER_OK)
		return ret;

	ret = key_ladder_gcm_setup(&ctx, key_type);
	if (ret != KEY_LADDER_OK)
		return ret;

	ret = mbedtls_gcm_auth_decrypt(&ctx, cipher_len,
	                               iv, iv_len, aad, aad_len,
	                               tag, tag_len, cipher, plain);
	mbedtls_gcm_free(&ctx);

	return key_ladder_map_err(ret);
}

#endif /* CONFIG_TRUSTENGINE */
