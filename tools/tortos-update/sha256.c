/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SHA-256: see sha256.h. The names follow FIPS 180-4, section 6.2. */
#include "sha256.h"

#include <stdio.h>
#include <string.h>

/* The first 32 bits of the fractional parts of the cube roots of the first
 * 64 primes (4.2.2) */
static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROTR(x, n) ((x) >> (n) | (x) << (32 - (n)))

/* One 64-byte block into the state (6.2.2) */
static void compress(uint32_t state[8], const uint8_t *block)
{
	uint32_t w[64], a, b, c, d, e, f, g, h;
	int t;

	for (t = 0; t < 16; t++)
		w[t] = (uint32_t)block[4 * t] << 24 | (uint32_t)block[4 * t + 1] << 16 |
		       (uint32_t)block[4 * t + 2] << 8 | block[4 * t + 3];
	for (t = 16; t < 64; t++) {
		uint32_t s0 = ROTR(w[t - 15], 7) ^ ROTR(w[t - 15], 18) ^ w[t - 15] >> 3;
		uint32_t s1 = ROTR(w[t - 2], 17) ^ ROTR(w[t - 2], 19) ^ w[t - 2] >> 10;

		w[t] = w[t - 16] + s0 + w[t - 7] + s1;
	}
	a = state[0]; b = state[1]; c = state[2]; d = state[3];
	e = state[4]; f = state[5]; g = state[6]; h = state[7];
	for (t = 0; t < 64; t++) {
		uint32_t t1 = h + (ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25)) + ((e & f) ^ (~e & g)) +
		              K[t] + w[t];
		uint32_t t2 = (ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));

		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}
	state[0] += a; state[1] += b; state[2] += c; state[3] += d;
	state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256_start(sha256 *h)
{
	/* The first 32 bits of the fractional parts of the square roots of the
	 * first 8 primes (5.3.3) */
	static const uint32_t start[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};

	memcpy(h->state, start, sizeof start);
	h->length = 0;
	h->used = 0;
}

void sha256_add(sha256 *h, const void *data, size_t n)
{
	const uint8_t *p = data;

	h->length += n;
	if (h->used) {
		size_t take = 64 - h->used < n ? 64 - h->used : n;

		memcpy(h->block + h->used, p, take);
		h->used += take;
		p += take;
		n -= take;
		if (h->used < 64)
			return;
		compress(h->state, h->block);
		h->used = 0;
	}
	for (; n >= 64; p += 64, n -= 64)
		compress(h->state, p);
	memcpy(h->block, p, n);
	h->used = n;
}

void sha256_finish(sha256 *h, uint8_t digest[32], char hex[65])
{
	/* A 1 bit, zeros to 56 bytes into a block, and the length in bits as a
	 * big-endian 64-bit number (5.1.1) */
	uint64_t bits = h->length * 8;
	int i;

	h->block[h->used++] = 0x80;
	if (h->used > 56) {
		memset(h->block + h->used, 0, 64 - h->used);
		compress(h->state, h->block);
		h->used = 0;
	}
	memset(h->block + h->used, 0, 56 - h->used);
	for (i = 0; i < 8; i++)
		h->block[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
	compress(h->state, h->block);
	for (i = 0; i < 32; i++)
		digest[i] = (uint8_t)(h->state[i / 4] >> (24 - 8 * (i % 4)));
	if (hex)
		for (i = 0; i < 32; i++)
			snprintf(hex + 2 * i, 3, "%02x", digest[i]);
}
