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

#include "common.h"

#include <string.h>

#include "mbedtls_test.h"

#if CONFIG_TRUSTENGINE

#include "key_ladder_gcm.h"

#define KL_TEST_PLAIN_LEN    32
#define KL_TEST_AAD_LEN      20
/* Longest buffer dumped below is the plaintext. */
#define KL_TEST_HEX_MAX      (KL_TEST_PLAIN_LEN * 2 + 1)

static const uint8_t kl_test_iv[KEY_LADDER_GCM_IV_LEN] = {
	0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	0x88, 0x99, 0xaa, 0xbb
};

static const uint8_t kl_test_aad[KL_TEST_AAD_LEN] = {
	'k', 'e', 'y', '-', 'l', 'a', 'd', 'd', 'e', 'r',
	'-', 'g', 'c', 'm', '-', 'h', 'e', 'a', 'd', 'r'
};

/** Convert the CLI selector into the derived key type. */
static int kl_to_key_type(int root_sel)
{
	return root_sel ? MBEDTLS_GCM_DERIVED_DEVICE_ROOT_KEY
	                : MBEDTLS_GCM_DERIVED_MODEL_KEY;
}

static const char *kl_key_type_name(int key_type)
{
	return (key_type == MBEDTLS_GCM_DERIVED_DEVICE_ROOT_KEY) ? "device-key"
	                                                         : "model-key";
}

/** Dump a labeled buffer as one hex line. */
static void kl_print_hex(const char *label, const uint8_t *buf, size_t len)
{
	static const char hex_chars[] = "0123456789abcdef";
	char hex[KL_TEST_HEX_MAX];
	size_t shown = (len > KL_TEST_PLAIN_LEN) ? KL_TEST_PLAIN_LEN : len;
	size_t i;

	for (i = 0; i < shown; i++) {
		hex[2 * i + 0] = hex_chars[(buf[i] >> 4) & 0x0f];
		hex[2 * i + 1] = hex_chars[buf[i] & 0x0f];
	}
	hex[2 * shown] = '\0';

	BK_DUMP_OUT("key_ladder: %s (%u bytes): %s\r\n",
	            label, (unsigned)len, hex);
}

/** Plaintext and AAD survive an encrypt/decrypt round trip. */
static int kl_case_roundtrip(int key_type)
{
	uint8_t plain[KL_TEST_PLAIN_LEN];
	uint8_t cipher[KL_TEST_PLAIN_LEN];
	uint8_t dec[KL_TEST_PLAIN_LEN];
	uint8_t tag[KEY_LADDER_GCM_TAG_LEN];
	int ret;

	memset(plain, 0xa5, sizeof(plain));
	memset(cipher, 0, sizeof(cipher));
	memset(dec, 0, sizeof(dec));
	memset(tag, 0, sizeof(tag));

	ret = bk_key_ladder_gcm_encrypt(key_type, kl_test_iv, sizeof(kl_test_iv),
	                                kl_test_aad, sizeof(kl_test_aad),
	                                plain, sizeof(plain),
	                                cipher, tag, sizeof(tag));
	if (ret != KEY_LADDER_OK) {
		BK_DUMP_OUT("key_ladder: encrypt failed, ret=%d\r\n", ret);
		return -1;
	}

	kl_print_hex("plain ", plain, sizeof(plain));
	kl_print_hex("cipher", cipher, sizeof(cipher));
	kl_print_hex("tag   ", tag, sizeof(tag));

	ret = bk_key_ladder_gcm_decrypt(key_type, kl_test_iv, sizeof(kl_test_iv),
	                                kl_test_aad, sizeof(kl_test_aad),
	                                cipher, sizeof(cipher),
	                                dec, tag, sizeof(tag));
	if (ret != KEY_LADDER_OK) {
		BK_DUMP_OUT("key_ladder: decrypt failed, ret=%d\r\n", ret);
		return -1;
	}

	if (memcmp(plain, dec, sizeof(plain)) != 0) {
		BK_DUMP_OUT("key_ladder: plaintext mismatch\r\n");
		kl_print_hex("expect", plain, sizeof(plain));
		kl_print_hex("got   ", dec, sizeof(dec));
		return -1;
	}

	return 0;
}

/** A modified tag must be rejected. */
static int kl_case_bad_tag(int key_type)
{
	uint8_t plain[KL_TEST_PLAIN_LEN];
	uint8_t cipher[KL_TEST_PLAIN_LEN];
	uint8_t dec[KL_TEST_PLAIN_LEN];
	uint8_t tag[KEY_LADDER_GCM_TAG_LEN];
	int ret;

	memset(plain, 0xb6, sizeof(plain));
	memset(dec, 0xff, sizeof(dec));

	ret = bk_key_ladder_gcm_encrypt(key_type, kl_test_iv, sizeof(kl_test_iv),
	                                kl_test_aad, sizeof(kl_test_aad),
	                                plain, sizeof(plain),
	                                cipher, tag, sizeof(tag));
	if (ret != KEY_LADDER_OK) {
		BK_DUMP_OUT("key_ladder: encrypt failed, ret=%d\r\n", ret);
		return -1;
	}

	tag[0] ^= 0x01;

	ret = bk_key_ladder_gcm_decrypt(key_type, kl_test_iv, sizeof(kl_test_iv),
	                                kl_test_aad, sizeof(kl_test_aad),
	                                cipher, sizeof(cipher),
	                                dec, tag, sizeof(tag));
	if (ret != KEY_LADDER_ERR_AUTH) {
		BK_DUMP_OUT("key_ladder: expect AUTH error, got ret=%d\r\n", ret);
		return -1;
	}

	return 0;
}

/** A modified AAD must be rejected. */
static int kl_case_bad_aad(int key_type)
{
	uint8_t plain[KL_TEST_PLAIN_LEN];
	uint8_t cipher[KL_TEST_PLAIN_LEN];
	uint8_t dec[KL_TEST_PLAIN_LEN];
	uint8_t tag[KEY_LADDER_GCM_TAG_LEN];
	uint8_t bad_aad[KL_TEST_AAD_LEN];
	int ret;

	memset(plain, 0xc7, sizeof(plain));
	memset(dec, 0xff, sizeof(dec));

	ret = bk_key_ladder_gcm_encrypt(key_type, kl_test_iv, sizeof(kl_test_iv),
	                                kl_test_aad, sizeof(kl_test_aad),
	                                plain, sizeof(plain),
	                                cipher, tag, sizeof(tag));
	if (ret != KEY_LADDER_OK) {
		BK_DUMP_OUT("key_ladder: encrypt failed, ret=%d\r\n", ret);
		return -1;
	}

	memcpy(bad_aad, kl_test_aad, sizeof(bad_aad));
	bad_aad[0] ^= 0x01;

	ret = bk_key_ladder_gcm_decrypt(key_type, kl_test_iv, sizeof(kl_test_iv),
	                                bad_aad, sizeof(bad_aad),
	                                cipher, sizeof(cipher),
	                                dec, tag, sizeof(tag));
	if (ret != KEY_LADDER_ERR_AUTH) {
		BK_DUMP_OUT("key_ladder: expect AUTH error, got ret=%d\r\n", ret);
		return -1;
	}

	return 0;
}

/** AAD-only operation produces a tag that verifies. */
static int kl_case_auth_only(int key_type)
{
	uint8_t tag[KEY_LADDER_GCM_TAG_LEN];
	int ret;

	memset(tag, 0, sizeof(tag));

	ret = bk_key_ladder_gcm_encrypt(key_type, kl_test_iv, sizeof(kl_test_iv),
	                                kl_test_aad, sizeof(kl_test_aad),
	                                NULL, 0, NULL, tag, sizeof(tag));
	if (ret != KEY_LADDER_OK) {
		BK_DUMP_OUT("key_ladder: auth-only encrypt failed, ret=%d\r\n", ret);
		return -1;
	}

	kl_print_hex("gmac  ", tag, sizeof(tag));

	ret = bk_key_ladder_gcm_decrypt(key_type, kl_test_iv, sizeof(kl_test_iv),
	                                kl_test_aad, sizeof(kl_test_aad),
	                                NULL, 0, NULL, tag, sizeof(tag));
	if (ret != KEY_LADDER_OK) {
		BK_DUMP_OUT("key_ladder: auth-only verify failed, ret=%d\r\n", ret);
		return -1;
	}

	return 0;
}

int te200_key_ladder_gcm_test(int root_sel)
{
	int key_type = kl_to_key_type(root_sel);
	const char *name = kl_key_type_name(key_type);
	int err_cnt = 0;

	BK_DUMP_OUT("key_ladder: ===== %s test begin =====\r\n", name);

	if (kl_case_roundtrip(key_type) != 0) {
		BK_DUMP_OUT("key_ladder: [FAIL] %s roundtrip\r\n", name);
		err_cnt++;
	} else {
		BK_DUMP_OUT("key_ladder: [PASS] %s roundtrip\r\n", name);
	}

	if (kl_case_bad_tag(key_type) != 0) {
		BK_DUMP_OUT("key_ladder: [FAIL] %s tampered tag\r\n", name);
		err_cnt++;
	} else {
		BK_DUMP_OUT("key_ladder: [PASS] %s tampered tag\r\n", name);
	}

	if (kl_case_bad_aad(key_type) != 0) {
		BK_DUMP_OUT("key_ladder: [FAIL] %s tampered aad\r\n", name);
		err_cnt++;
	} else {
		BK_DUMP_OUT("key_ladder: [PASS] %s tampered aad\r\n", name);
	}

	if (kl_case_auth_only(key_type) != 0) {
		BK_DUMP_OUT("key_ladder: [FAIL] %s auth only\r\n", name);
		err_cnt++;
	} else {
		BK_DUMP_OUT("key_ladder: [PASS] %s auth only\r\n", name);
	}

	BK_DUMP_OUT("key_ladder: ===== %s test end: %s =====\r\n",
	            name, err_cnt ? "FAIL" : "PASS");

	return err_cnt ? -1 : 0;
}

int te200_key_ladder_root_diff_test(void)
{
	uint8_t plain[KL_TEST_PLAIN_LEN];
	uint8_t cipher_model[KL_TEST_PLAIN_LEN];
	uint8_t cipher_device[KL_TEST_PLAIN_LEN];
	uint8_t tag_model[KEY_LADDER_GCM_TAG_LEN];
	uint8_t tag_device[KEY_LADDER_GCM_TAG_LEN];
	int ret;

	memset(plain, 0xd8, sizeof(plain));

	ret = bk_key_ladder_gcm_encrypt(MBEDTLS_GCM_DERIVED_MODEL_KEY,
	                                kl_test_iv, sizeof(kl_test_iv),
	                                kl_test_aad, sizeof(kl_test_aad),
	                                plain, sizeof(plain),
	                                cipher_model, tag_model, sizeof(tag_model));
	if (ret != KEY_LADDER_OK) {
		BK_DUMP_OUT("key_ladder: model-key encrypt failed, ret=%d\r\n", ret);
		return -1;
	}

	ret = bk_key_ladder_gcm_encrypt(MBEDTLS_GCM_DERIVED_DEVICE_ROOT_KEY,
	                                kl_test_iv, sizeof(kl_test_iv),
	                                kl_test_aad, sizeof(kl_test_aad),
	                                plain, sizeof(plain),
	                                cipher_device, tag_device, sizeof(tag_device));
	if (ret != KEY_LADDER_OK) {
		BK_DUMP_OUT("key_ladder: device-key encrypt failed, ret=%d\r\n", ret);
		return -1;
	}

	kl_print_hex("model ", cipher_model, sizeof(cipher_model));
	kl_print_hex("device", cipher_device, sizeof(cipher_device));

	if (memcmp(cipher_model, cipher_device, sizeof(cipher_model)) == 0) {
		BK_DUMP_OUT("key_ladder: [FAIL] both roots produced the same cipher\r\n");
		return -1;
	}

	BK_DUMP_OUT("key_ladder: [PASS] root selection changes the derived key\r\n");
	return 0;
}

#else /* CONFIG_TRUSTENGINE */

int te200_key_ladder_gcm_test(int root_sel)
{
	(void)root_sel;
	BK_DUMP_OUT("key_ladder: hardware crypto engine is not enabled\r\n");
	return -1;
}

int te200_key_ladder_root_diff_test(void)
{
	BK_DUMP_OUT("key_ladder: hardware crypto engine is not enabled\r\n");
	return -1;
}

#endif /* CONFIG_TRUSTENGINE */
