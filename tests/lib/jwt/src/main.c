/*
 * RFC 7519 Json Web Tokens
 *
 * Copyright (C) 2018, Linaro, Ltd
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include <zephyr/types.h>
#include <stdbool.h>
#include <ztest.h>
#include <json.h>

#if !defined(CONFIG_MBEDTLS_CFG_FILE)
#  error "Shouldn't happen"
#  include <mbedtls/config.h>
#else
#  include CONFIG_MBEDTLS_CFG_FILE
#endif

#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>

extern unsigned char jwt_test_private_der[];
extern unsigned int jwt_test_private_der_len;

#ifdef MBEDTLS_MEMORY_BUFFER_ALLOC_C
# include <mbedtls/memory_buffer_alloc.h>
static unsigned char heap[32768];
#else
# error "no memoby buffer"
#endif

/*
 * Base-64 encoding is typically done by lookup into a 64-byte static
 * array.  As an experiment, lets look at both code size and time for
 * one that does the character encoding computationally.  Like the
 * array version, this doesn't do bounds checking, and assumes the
 * passed value has been masked.
 *
 * On Cortex-M, this function is 34 bytes of code, which is only a
 * little more than half of the size of the lookup table.
 */
#if 1
int base64_char(int value)
{
	if (value < 26) {
		return value + 'A';
	} else if (value < 52) {
		return value + 'a' - 26;
	} else if (value < 62) {
		return value + '0' - 52;
	} else if (value == 62) {
		return '-';
	} else {
		return '_';
	}
}
#else
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static inline int base64_char(int value)
{
	return b64_table[value];
}
#endif

struct base64_state {
	char *buf;
	int len;
	unsigned char wip[3];
	int pending;
	bool overflowed;
};

void base64_init(struct base64_state *st, char *buf, int buflen)
{
	st->buf = buf;
	st->len = buflen;
	st->pending = 0;
	st->overflowed = false;
	memset(st->wip, 0, 3);
}

void base64_outch(struct base64_state *st, char ch)
{
	if (st->overflowed) {
		return;
	}

	if (st->len < 2) {
		st->overflowed = true;
		return;
	}

	*st->buf++ = ch;
	st->len--;
	*st->buf = 0;
}

/* Perform a non-padded flush. */
#if 1
void base64_flush(struct base64_state *st)
{
	if (st->pending < 1) {
		return;
	}

	base64_outch(st, base64_char(st->wip[0] >> 2));
	base64_outch(st, base64_char(((st->wip[0] & 0x03) << 4) | (st->wip[1] >> 4)));

	if (st->pending >= 2) {
		base64_outch(st, base64_char(((st->wip[1] & 0x0f) << 2) | (st->wip[2] >> 6)));
	}
	if (st->pending >= 3) {
		base64_outch(st, base64_char(st->wip[2] & 0x3f));
	}

	st->pending = 0;
	memset(st->wip, 0, 3);
	return;
}
#else
void base64_flush(struct base64_state *st)
{
	if (st->pending < 1) {
		return;
	}

	uint8_t w0 = st->wip[0];
	uint8_t w1 = st->wip[1];
	base64_outch(st, base64_char(w0 >> 2));
	base64_outch(st, base64_char(((w0 & 0x03) << 4) | (w1 >> 4)));

	if (st->pending < 2) {
		goto done;
	}

	uint8_t w2 = st->wip[2];
	base64_outch(st, base64_char(((w1 & 0x0f) << 2) | (w2 >> 6)));

	if (st->pending < 3) {
		goto done;
	}

	base64_outch(st, base64_char(w2 & 0x3f));

done:
	st->pending = 0;
	memset(st->wip, 0, 3);
	return;
}
#endif

void base64_addbyte(struct base64_state *st, uint8_t byte)
{
	st->wip[st->pending++] = byte;
	if (st->pending == 3) {
		base64_flush(st);
	}
}

int base64_append_bytes(const char *bytes, size_t len,
			 void *data)
{
	struct base64_state *st = data;

	while (len-- > 0) {
		base64_addbyte(st, *bytes++);
	}

	return 0;
}

struct jwt_header {
	char *typ;
	char *alg;
};

struct json_obj_descr jwt_header_desc[] = {
	JSON_OBJ_DESCR_PRIM(struct jwt_header, alg, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct jwt_header, typ, JSON_TOK_STRING),
};

struct jwt_payload {
	s32_t exp;
	s32_t iat;
	char *aud;
};

struct json_obj_descr jwt_payload_desc[] = {
	JSON_OBJ_DESCR_PRIM(struct jwt_payload, aud, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct jwt_payload, exp, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct jwt_payload, iat, JSON_TOK_NUMBER),
};

static void test_json_encode(void)
{
	struct jwt_header head = {
		.typ = "JWT",
		.alg = "RS256",
	};
	struct jwt_payload payload = {
		.exp = 1530312026,
		.iat = 1530308426,
		.aud = "iot-work-199419",
	};

	char buf[470];
	struct base64_state b64_state;

	printk("test_json_encode\n");
	ssize_t len = json_calc_encoded_len(jwt_header_desc,
					    ARRAY_SIZE(jwt_header_desc),
					    &head);
	printk("Encoded length: %d\n", len);

	base64_init(&b64_state, buf, sizeof(buf));

	int res = json_obj_encode(jwt_header_desc, ARRAY_SIZE(jwt_header_desc),
				  &head, base64_append_bytes, &b64_state);
	base64_flush(&b64_state);
	printk("Res = %d\n", res);

	base64_outch(&b64_state, '.');

	res = json_obj_encode(jwt_payload_desc, ARRAY_SIZE(jwt_payload_desc),
			      &payload, base64_append_bytes, &b64_state);
	base64_flush(&b64_state);
	printk("Res = %d\n", res);

	printk("Value: %s\n", buf);

	// Sign it.
	mbedtls_pk_context ctx;
	printk("Size of context: %d\n", sizeof(ctx));
	// printk("Size of rsa ctx: %d\n", sizeof(rsa));
	mbedtls_pk_init(&ctx);
	res = mbedtls_pk_parse_key(&ctx, jwt_test_private_der,
				   jwt_test_private_der_len,
				   NULL, 0);
	printk("Parse: %x\n", res);
	zassert_equal(res, 0, "Parsing internal key");

	u8_t hash[32], sig[256];
	size_t sig_len = sizeof(sig);

	mbedtls_sha256(buf, b64_state.buf - buf, hash, 0);

	res = mbedtls_pk_sign(&ctx, MBEDTLS_MD_SHA256,
			      hash, sizeof(hash),
			      sig, &sig_len,
			      NULL, NULL);
	printk("Sign status: %x (%d)\n", res, sig_len);

	base64_outch(&b64_state, '.');

	base64_append_bytes(sig, sig_len, &b64_state);
	base64_flush(&b64_state);

	printk("Value: %s\n", buf);
}

void test_main(void)
{
	mbedtls_memory_buffer_alloc_init(heap, sizeof(heap));

	ztest_test_suite(lib_jwt_test,
			 ztest_unit_test(test_json_encode));

	ztest_run_test_suite(lib_jwt_test);
}

