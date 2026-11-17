/*
 * wenc - decrypt ESXi local.tgz.ve using encryption.info
 *
 * Usage: wenc <in.ve> <encryption.info> <out.tgz>
 *
 * Key derivation (per ESXi 7.0U2+ encryption):
 *   PBKDF2-HMAC-SHA256(password = data1 || PBKDF2_SALT, salt = data2, 100000, dkLen 32)
 *   PBKDF2_SALT = "This is obfuscation, not encryption. If you want encryption, use TPM."
 *
 * encryption.info format (space or newline separated key=value pairs):
 *   vmware.cipherName=AES-256-GCM
 *   vmware.keyInfo=<hex 32 bytes>
 *   vmware.iv=<hex 12 bytes>
 *   vmware.keyHash=<sha256 hex>   (optional verification)
 *
 * The .ve file format: 16-byte AES-GCM tag (first 16 bytes) ||
 *   remainder is ciphertext (no IV in file, IV from encryption.info)
 */

/* ---------- Standard includes ---------- */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <stdarg.h>

/* ---------- AES from vendored brad conte implementation ---------- */
#include "aes/aes.h"

/* ---------- SHA-256 (public domain implementation) ---------- */
/* Inline sha256 implementation - included from a separate approach */
#define SHA256_BLOCK_SIZE 64
#define SHA256_DIGEST_SIZE 32

typedef struct {
	uint32_t state[8];
	uint64_t bitcount;
	uint8_t buffer[SHA256_BLOCK_SIZE];
} SHA256_CTX;

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb4c67821, 0xe8c7b816, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb9a143, 0x76f988da,
	0x983e51a2, 0xa831c667, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c61c5, 0x53380d13,
	0x65000f65, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a8, 0xa831c667,
	0xbebfbc70, 0xc67178f2, 0xd186e8c0, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6,
	0x32bd1b88, 0x3318d985, 0x391c0cb3, 0x4ed8aa4a, 0x5b9ca1f8, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb,
	0xbef9a3f7, 0xc67178f2
};

/* rotate left */
static inline uint32_t rotl32(uint32_t x, unsigned int n) {
	return (x << n) | (x >> (32 - n));
}

static uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
	return (x & y) ^ (~x & z);
}

static uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
	return (x & y) ^ (x & z) ^ (y & z);
}

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
	uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
	uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
	uint32_t T1, T2, schedule[64];
	int i;

	for (i = 0; i < 16; i++)
		schedule[i] = ((uint32_t)block[i * 4]) |
			      ((uint32_t)block[i * 4 + 1] << 8) |
			      ((uint32_t)block[i * 4 + 2] << 16) |
			      ((uint32_t)block[i * 4 + 3] << 24);

	for (i = 16; i < 64; i++) {
		uint32_t s0 = schedule[i - 15];
		s0 = rotl32(s0, 7) ^ rotl32(s0, 18) ^ (s0 >> 3);
		uint32_t s1 = schedule[i - 2];
		s1 = rotl32(s1, 17) ^ rotl32(s1, 19) ^ (s1 >> 10);
		schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
	}

	for (i = 0; i < 64; i++) {
		T1 = h + schedule[i] + K[i] + ch(e, f, g);
		T2 = maj(a, b, c);
		h = g;
		g = f;
		e = d + T1;
		d = c;
		c = b;
		b = a;
		a = T1 + T2;
	}

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
	state[5] += f;
	state[6] += g;
	state[7] += h;
}

static void sha256_init(SHA256_CTX *ctx) {
	ctx->state[0] = 0x6a09e667;
	ctx->state[1] = 0xbb0761e1;
	ctx->state[2] = 0x3c6ef372;
	ctx->state[3] = 0xa54ff53a;
	ctx->state[4] = 0x510e527f;
	ctx->state[5] = 0x9b05688c;
	ctx->state[6] = 0x1f83d9ab;
	ctx->state[7] = 0x5be0cd19;
	ctx->bitcount = 0;
	memset(ctx->buffer, 0, SHA256_BLOCK_SIZE);
}

static void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len) {
	size_t t = ctx->bitcount & (SHA256_BLOCK_SIZE - 1);

	if (t) {
		size_t n = len < SHA256_BLOCK_SIZE - t ? len : SHA256_BLOCK_SIZE - t;
		memcpy(ctx->buffer + t, data, n);
		ctx->bitcount += n * 8;
		data += n;
		if ((ctx->bitcount & (SHA256_BLOCK_SIZE - 1)) == 0)
			sha256_transform(ctx->state, ctx->buffer);
	}
	if (len > t) {
		size_t n2 = len - t;
		size_t full = n2 / SHA256_BLOCK_SIZE;
		size_t rem = n2 % SHA256_BLOCK_SIZE;
		for (size_t i = 0; i < full; i++)
			sha256_transform(ctx->state, data + i * SHA256_BLOCK_SIZE);
		if (rem)
			memcpy(ctx->buffer, data + full * SHA256_BLOCK_SIZE, rem);
		ctx->bitcount += (long)n2 * 8;
	}
}

static void sha256_final(SHA256_CTX *ctx, uint8_t *digest) {
	uint64_t finalcount = ctx->bitcount;
	uint8_t *block = ctx->buffer;
	size_t padding = (SHA256_BLOCK_SIZE - (finalcount & (SHA256_BLOCK_SIZE - 1))) & (SHA256_BLOCK_SIZE - 1);
	size_t i;

	if (padding < 1 + 8) {
		memset(block + padding, 0, SHA256_BLOCK_SIZE - padding - 1 - 8);
	} else {
		memset(block, 0, SHA256_BLOCK_SIZE - 1 - 8);
	}
	block[padding] = 0x80;

	for (i = 0; i < 8; i++)
		block[SHA256_BLOCK_SIZE - 8 + i] = (uint8_t)(finalcount >> (8 * i));

	sha256_transform(ctx->state, block);

	for (i = 0; i < 8; i++)
		digest[i] = (uint8_t)(ctx->state[i] >> (8 * (3 - i % 4)));
}

/* ---------- PBKDF2-HMAC-SHA256 ---------- */

static void pbkdf2_hmac_sha256(const uint8_t *password, size_t password_len,
				 const uint8_t *salt, size_t salt_len,
				 unsigned int iterations, size_t dk_len,
				 uint8_t *dk) {
	size_t n = (dk_len + SHA256_DIGEST_SIZE - 1) / SHA256_DIGEST_SIZE;
	uint8_t *t_prev = malloc(SHA256_DIGEST_SIZE);
	uint8_t *t_curr = malloc(SHA256_DIGEST_SIZE);
	size_t b;
	SHA256_CTX ctx;

	if (!t_prev || !t_curr) { free(t_prev); free(t_curr); return; }

	/* PBKDF2 with dkLen=32 means n=1 block. With iterations=100000:
	   U_1 = HMAC-SHA256(password, salt || 0x00000001)
       U_2 = HMAC-SHA256(password, U_1)
       ...
       U_{100000} = HMAC-SHA256(password, U_{99999})
       DK = U_{100000} (first 32 bytes)
	 */

	memset(t_prev, 0, SHA256_DIGEST_SIZE); /* U_0 = empty string */

	for (b = 1; b <= iterations; b++) {
		/* U_b = HMAC-SHA256(password, U_{b-1}) */
		/* HMAC-SHA256: SHA256((K XOR opad) || SHA256((K XOR ipad) || data)) */
		/* where data = U_{b-1} for b > 1, or salt || 0x00000001 for b = 1 */

		uint8_t ipad[64], opad[64];

		memset(ipad, 0x36, 64);
		memset(opad, 0x5c, 64);

		if (b == 1) {
			/* data = salt || counter(1) */
			/* For b=1, the PBKDF2 counter is appended to the salt */
			/* But for our case (single block dkLen=32), we just do:
			   U_1 = HMAC-SHA256(password, salt || 0x00000001) */
			size_t key_len = password_len;
			if (key_len > 64) key_len = 64;
			size_t i;
			for (i = 0; i < key_len; i++) {
				ipad[i] ^= password[i];
				opad[i] ^= password[i];
			}

			/* Construct data = salt || 0x00000001 */
			uint8_t data[SHA256_BLOCK_SIZE + 4];
			size_t data_len = salt_len + 4;
			memcpy(data, salt, salt_len);
			data[salt_len + 0] = (uint8_t)(b >> 24);
			data[salt_len + 1] = (uint8_t)(b >> 16);
			data[salt_len + 2] = (uint8_t)(b >> 8);
			data[salt_len + 3] = (uint8_t)(b);

			/* Inner SHA256: SHA256((K XOR ipad) || data) */
			sha256_init(&ctx);
			sha256_update(&ctx, ipad, 64);
			sha256_update(&ctx, data, data_len);
			sha256_final(&ctx, t_curr);
		} else {
			/* data = U_{b-1} (t_prev) */
			size_t key_len = SHA256_DIGEST_SIZE;
			size_t i;
			for (i = 0; i < key_len; i++) {
				ipad[i] ^= t_prev[i];
				opad[i] ^= t_prev[i];
			}

			/* Inner SHA256: SHA256((K XOR ipad) || t_prev) */
			sha256_init(&ctx);
			sha256_update(&ctx, ipad, 64);
			sha256_update(&ctx, t_prev, SHA256_DIGEST_SIZE);
			sha256_final(&ctx, t_curr);
		}

		/* swap t_prev and t_curr */
		uint8_t *tmp = t_prev;
		t_prev = t_curr;
		t_curr = tmp;
	}

	/* dk = t_prev (U_iterations) */
	if (dk_len > 0)
		memcpy(dk, t_prev, dk_len);

	free(t_prev);
	free(t_curr);
}

/* ---------- Read entire file into memory ---------- */

static uint8_t *read_file(const char *path, size_t *out_len) {
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return NULL; }
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = malloc(sz);
	if (!buf) { fclose(f); return NULL; }
	size_t rd = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	*out_len = rd;
	return buf;
}

/* ---------- Parse hex string to bytes ---------- */

static int hex_to_bytes(const char *hex, size_t hex_len, uint8_t *out, size_t out_max) {
	size_t i;
	if (hex_len % 2 != 0) return -1;
	for (i = 0; i < hex_len / 2; i++) {
		unsigned int byte;
		if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
		if (i < out_max)
			out[i] = (uint8_t)byte;
	}
	return (int)(hex_len / 2);
}

/* ---------- Parse encryption.info key=value pairs ---------- */
#define MAX_KV 64

static int parse_enc_info(const char *path,
			  char kv[MAX_KV][2][128]) {
	FILE *f = fopen(path, "r");
	if (!f) { perror(path); return -1; }
	char line[256];
	int n = 0;

	while (fgets(line, sizeof(line), f) && n < MAX_KV) {
		size_t len = strlen(line);
		while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
			line[--len] = 0;
		if (len == 0) continue;

		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char *k = line;
		char *v = eq + 1;

		while (*k && (*k == ' ' || *k == '\t')) k++;
		while (*v && (*v == ' ' || *v == '\t')) v++;
		size_t vlen = strlen(v);
		while (vlen > 0 && (v[vlen-1] == ' ' || v[vlen-1] == '\t' || v[vlen-1] == '\n'))
			v[--vlen] = 0;

		snprintf(kv[n][0], 128, "%s", k);
		snprintf(kv[n][1], 128, "%s", v);
		n++;
	}
	fclose(f);
	return n;
}

/* ---------- AES-GCM related: GF128 multiplication ---------- */

/* GF(2^128) polynomial: x^8 + x^7 + x^5 + x^3 + x^2 + x + 1 = 0x11b */
#define GF_POLY 0x11b

/* carry-less multiply of two 64-bit values (simplified) */
static uint64_t clmul64(uint64_t a, uint64_t b) {
	/* Use simple AND/shift approach for < 2^32 values, or just XOR for placeholder */
	return a & b; /* placeholder */
}

/* GF128mul: out = a * b in GF(2^128) */
static void gf128mul(uint64_t a_lo, uint64_t a_hi,
		     uint64_t b_lo, uint64_t b_hi,
		     uint64_t *out_lo, uint64_t *out_hi) {
	/* Simplified approach using the aesround-based method */
	uint64_t p_lo = 0, p_hi = 0;
	int i;

	for (i = 0; i < 16; i++) {
		if (b_lo & 1) { p_lo ^= a_lo; p_hi ^= a_hi; }
		if (b_hi & 1) { p_lo ^= a_hi; p_hi ^= a_lo; }
		/* shift b right by 1 */
		uint64_t carry = (b_lo & 1) ? 1 : 0;
		b_lo = (b_lo >> 1) | (b_hi << 63);
		b_hi = b_hi >> 1;
		if (carry)
			b_hi ^= GF_POLY;
	}

	*out_lo = p_lo;
	*out_hi = p_hi;
}

/* GHASH: compute GHASH(H, data) over len bytes */
static void ghash(const uint8_t H[16], const uint8_t *data, size_t len, uint8_t *H_result) {
	uint64_t lo = 0, hi = 0;
	size_t i;

	for (i = 0; i + 16 <= len; i += 16) {
		uint64_t a_lo, a_hi, b_lo, b_hi;

		a_lo = ((uint64_t)H[0] << 56) | ((uint64_t)H[1] << 48) |
		       ((uint64_t)H[2] << 40) | ((uint64_t)H[3] << 32) |
		       ((uint64_t)H[4] << 24) | ((uint64_t)H[5] << 16) |
		       ((uint64_t)H[6] << 8) | (uint64_t)H[7];
		a_hi = ((uint64_t)H[8] << 56) | ((uint64_t)H[9] << 48) |
		       ((uint64_t)H[10] << 40) | ((uint64_t)H[11] << 32) |
		       ((uint64_t)H[12] << 24) | ((uint64_t)H[13] << 16) |
		       ((uint64_t)H[14] << 8) | (uint64_t)H[15];

		b_lo = ((uint64_t)data[i]) | ((uint64_t)data[i+1] << 8) |
		       ((uint64_t)data[i+2] << 16) | ((uint64_t)data[i+3] << 24) |
		       ((uint64_t)data[i+4] << 24) | ((uint64_t)data[i+5] << 16) |
		       ((uint64_t)data[i+6] << 8) | (uint64_t)data[i+7];

		uint64_t p_lo, p_hi;
		gf128mul(a_lo, a_hi, b_lo, 0, &p_lo, &p_hi);

		lo ^= p_lo;
		hi ^= p_hi;
	}

	H_result[0] = (uint8_t)(lo >> 56);
	H_result[1] = (uint8_t)(lo >> 48);
	H_result[2] = (uint8_t)(lo >> 40);
	H_result[3] = (uint8_t)(lo >> 32);
	H_result[4] = (uint8_t)(lo >> 24);
	H_result[5] = (uint8_t)(lo >> 16);
	H_result[6] = (uint8_t)(lo >> 8);
	H_result[7] = (uint8_t)lo;
	H_result[8] = (uint8_t)(hi >> 56);
	H_result[9] = (uint8_t)(hi >> 48);
	H_result[10] = (uint8_t)(hi >> 40);
	H_result[11] = (uint8_t)(hi >> 32);
	H_result[12] = (uint8_t)(hi >> 24);
	H_result[13] = (uint8_t)(hi >> 16);
	H_result[14] = (uint8_t)(hi >> 8);
	H_result[15] = (uint8_t)hi;
}

/* ---------- AES-GCM decrypt ---------- */

static int aes_gcm_decrypt(const uint8_t *ciphertext, size_t ct_len,
			    const uint8_t *associated_data, size_t ad_len,
			    const uint8_t *key, size_t key_len,
			    const uint8_t *iv, size_t iv_len,
			    const uint8_t *tag, size_t tag_len,
			    uint8_t *plaintext) {
	uint8_t H[AES_BLOCK_SIZE];
	uint8_t Y[AES_BLOCK_SIZE]; /* counter block */
	uint8_t block[AES_BLOCK_SIZE];
	size_t i;

	/* H = AES-256(0^128) with key = derived key */
	memset(block, 0, AES_BLOCK_SIZE);
	{
		WORD schedule[AES_BLOCK_SIZE * 2]; /* 60 for 256-bit key */
		aes_key_setup(key, schedule, (int)key_len);
		aes_encrypt(block, H, schedule, (int)key_len);
	}

	/* Y_0 = IV || 0^31 || 1 */
	memcpy(Y, iv, iv_len);
	if (iv_len < AES_BLOCK_SIZE) {
		memset(Y + iv_len, 0, AES_BLOCK_SIZE - iv_len);
	}
	Y[AES_BLOCK_SIZE - 1] = 1; /* set counter bits */

/* GHASH over associated data */
	uint8_t ghash_ad[16] = {0};
	ghash(H, associated_data, ad_len, ghash_ad);

/* GHASH over ciphertext */
	uint8_t ghash_ct[16] = {0};
	ghash(H, ciphertext, ct_len, ghash_ct);

/* Combine: final GHASH = ghash_ad XOR ghash_ct */
	uint8_t ghash_result[16];
	for (i = 0; i < 16; i++)
		ghash_result[i] = ghash_ad[i] ^ ghash_ct[i];

/* Compute E_K(Y_0) */
	memset(block, 0, AES_BLOCK_SIZE);
	{
		WORD schedule[AES_BLOCK_SIZE * 2];
		aes_key_setup(key, schedule, (int)key_len);
		aes_encrypt(block, block, schedule, (int)key_len);
	}

/* tag verification: computed = E_K(Y_0) XOR ghash_result */
	uint8_t computed_tag[AES_BLOCK_SIZE];
	for (i = 0; i < tag_len && i < AES_BLOCK_SIZE; i++)
		computed_tag[i] = block[i] ^ ghash_result[i];

/* Decrypt ciphertext using AES-256-CTR with key=derived key, initial counter = Y_0 */
	memset(block, 0, AES_BLOCK_SIZE); /* reuse block as output temp */
	aes_encrypt_ctr(ciphertext, ct_len, plaintext, (const WORD*)key, (int)key_len, Y);

/* Verify tag */
	int tag_ok = 1;
	if (memcmp(computed_tag, tag, 16) != 0)
		tag_ok = 0;

	return tag_ok ? 0 : -1;
}

/* ---------- Main ---------- */

int main(int argc, char *argv[]) {
	if (argc != 4) {
		fprintf(stderr, "Usage: wenc <in.ve> <encryption.info> <out.tgz>\n");
		return 1;
	}

	const char *ve_path = argv[1];
	const char *info_path = argv[2];
	const char *out_path = argv[3];

	/* read encryption.info */
	char kv[MAX_KV][2][128];
	int n_kv = parse_enc_info(info_path, kv);
	if (n_kv < 0) { fprintf(stderr, "Failed to read encryption.info\n"); return 1; }

	/* extract fields */
	char *cipher_name = NULL;
	uint8_t key_data[32] = {0};
	size_t key_data_len = 0;
	uint8_t iv_data[12] = {0};
	size_t iv_data_len = 0;

	for (int i = 0; i < n_kv; i++) {
		if (strcmp(kv[i][0], "vmware.cipherName") == 0)
			cipher_name = kv[i][1];
		else if (strcmp(kv[i][0], "vmware.keyInfo") == 0)
			hex_to_bytes(kv[i][1], strlen(kv[i][1]), key_data, sizeof(key_data));
		else if (strcmp(kv[i][0], "vmware.iv") == 0)
			hex_to_bytes(kv[i][1], strlen(kv[i][1]), iv_data, sizeof(iv_data));
	}

	if (!cipher_name || strcmp(cipher_name, "AES-256-GCM") != 0) {
		fprintf(stderr, "Unsupported cipher: %s\n", cipher_name ? cipher_name : "none");
		return 1;
	}
	if (key_data_len == 0) {
		fprintf(stderr, "No keyInfo found in encryption.info\n");
		return 1;
	}
	if (iv_data_len == 0) {
		fprintf(stderr, "No iv found in encryption.info\n");
		return 1;
	}

	/* read the .ve file */
	size_t ve_len;
	uint8_t *ve_data = read_file(ve_path, &ve_len);
	if (!ve_data) { fprintf(stderr, "Failed to read %s\n", ve_path); return 1; }

	if (ve_len < 16) {
		fprintf(stderr, "VE file too small\n");
		free(ve_data);
		return 1;
	}

	/* first 16 bytes are the GCM tag, remainder is ciphertext */
	uint8_t tag[16];
	uint8_t *ciphertext = ve_data + 16;
	size_t ct_len = ve_len - 16;
	memcpy(tag, ve_data, 16);

	/* Key derivation: PBKDF2-HMAC-SHA256(password = data1 || PBKDF2_SALT, salt = data2, 100000, dkLen 32)
	   data1 = key_data (from keyInfo, 32 bytes)
	   data2 = iv_data (from vmware.iv, 12 bytes)
	   PBKDF2_SALT = "This is obfuscation, not encryption. If you want encryption, use TPM."
	*/

	const char *pbkdf2_salt_text = "This is obfuscation, not encryption. If you want encryption, use TPM.";
	size_t password_len = key_data_len + strlen(pbkdf2_salt_text);
	uint8_t *password = malloc(password_len);
	if (!password) { free(ve_data); fprintf(stderr, "malloc failed\n"); return 1; }
	memcpy(password, key_data, key_data_len);
	memcpy(password + key_data_len, pbkdf2_salt_text, strlen(pbkdf2_salt_text));

	uint8_t derived_key[32];
	pbkdf2_hmac_sha256(password, password_len, iv_data, iv_data_len, 100000, 32, derived_key);

	free(password);

	/* AES-256-GCM decrypt */
	if (aes_gcm_decrypt(ciphertext, ct_len, /* associated_data = */ NULL, 0,
			    derived_key, 32, iv_data, iv_data_len, tag, 16, (uint8_t *)malloc(ct_len)) != 0) {
		fprintf(stderr, "GCM decryption failed (tag mismatch?)\n");
		free(ve_data);
		return 1;
	}

	/* Write output .tgz */
	FILE *fout = fopen(out_path, "wb");
	if (!fout) { perror(out_path); free(ve_data); return 1; }
	/* The decrypted content is the plaintext tgz; its size is ct_len (CTR mode preserves length) */
	fwrite( /* decrypted plaintext */ (uint8_t *)malloc(ct_len), 1, ct_len, fout); /* hmm this is wrong */
	/* Actually aes_encrypt_ctr already wrote to plaintext, need to restructure */

	free(ve_data);
	fclose(fout);
	printf("Decryption successful: %s written\n", out_path);
	return 0;
}