// QR code, ISO/IEC 18004: byte mode, error correction M, one block (versions 1-3), mask chosen by the
// standard's penalty score.
#include "wt_qr.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// per version 1..3 at level M: data codewords, error codewords (one block each)
static const int DATA_CW[4] = {0, 16, 28, 44};
static const int EC_CW[4] = {0, 10, 16, 26};

static uint8_t gf_mul(uint8_t x, uint8_t y) {
	int z = 0;
	for (int i = 7; i >= 0; i--) {
		z = (z << 1) ^ ((z >> 7) * 0x11D);
		z ^= ((y >> i) & 1) * x;
	}
	return (uint8_t)z;
}

static void rs_ec(const uint8_t* data, int n, int degree, uint8_t* out) {
	uint8_t div[32] = {0};
	div[degree - 1] = 1;
	uint8_t root = 1;
	for (int i = 0; i < degree; i++) {
		for (int j = 0; j < degree; j++) {
			div[j] = gf_mul(div[j], root);
			if (j + 1 < degree)
				div[j] ^= div[j + 1];
		}
		root = gf_mul(root, 0x02);
	}
	memset(out, 0, (size_t)degree);
	for (int k = 0; k < n; k++) {
		uint8_t f = data[k] ^ out[0];
		memmove(out, out + 1, (size_t)degree - 1);
		out[degree - 1] = 0;
		for (int i = 0; i < degree; i++)
			out[i] ^= gf_mul(div[i], f);
	}
}

typedef struct {
	int size;
	unsigned char m[WT_QR_MAX][WT_QR_MAX];
	unsigned char fn[WT_QR_MAX][WT_QR_MAX]; // function module (not data)
} Grid;

static void setf(Grid* g, int x, int y, int dark) {
	g->m[y][x] = (unsigned char)(dark != 0);
	g->fn[y][x] = 1;
}

static void finder(Grid* g, int cx, int cy) {
	for (int dy = -4; dy <= 4; dy++)
		for (int dx = -4; dx <= 4; dx++) {
			int x = cx + dx, y = cy + dy;
			if (x < 0 || y < 0 || x >= g->size || y >= g->size)
				continue;
			int d = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
			setf(g, x, y, d != 2 && d != 4);
		}
}

static void format_bits(Grid* g, int mask) {
	int data = (0 << 3) | mask; // level M = 00
	int rem = data;
	for (int i = 0; i < 10; i++)
		rem = (rem << 1) ^ ((rem >> 9) * 0x537);
	int bits = ((data << 10) | rem) ^ 0x5412;
	int s = g->size;
	for (int i = 0; i <= 5; i++)
		setf(g, 8, i, (bits >> i) & 1);
	setf(g, 8, 7, (bits >> 6) & 1);
	setf(g, 8, 8, (bits >> 7) & 1);
	setf(g, 7, 8, (bits >> 8) & 1);
	for (int i = 9; i < 15; i++)
		setf(g, 14 - i, 8, (bits >> i) & 1);
	for (int i = 0; i < 8; i++)
		setf(g, s - 1 - i, 8, (bits >> i) & 1);
	for (int i = 8; i < 15; i++)
		setf(g, 8, s - 15 + i, (bits >> i) & 1);
	setf(g, 8, s - 8, 1); // the dark module
}

static bool mask_bit(int mask, int x, int y) {
	switch (mask) {
	case 0: return (x + y) % 2 == 0;
	case 1: return y % 2 == 0;
	case 2: return x % 3 == 0;
	case 3: return (x + y) % 3 == 0;
	case 4: return (x / 3 + y / 2) % 2 == 0;
	case 5: return x * y % 2 + x * y % 3 == 0;
	case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
	default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
	}
}

static void apply_mask(Grid* g, int mask) {
	for (int y = 0; y < g->size; y++)
		for (int x = 0; x < g->size; x++)
			if (!g->fn[y][x] && mask_bit(mask, x, y))
				g->m[y][x] ^= 1;
}

// the standard's four penalty rules
static long penalty(const Grid* g) {
	int s = g->size;
	long p = 0;
	for (int pass = 0; pass < 2; pass++)
		for (int a = 0; a < s; a++) {
			int run = 1;
			for (int b = 1; b <= s; b++) {
				int cur = b < s ? (pass ? g->m[b][a] : g->m[a][b]) : -1;
				int prev = pass ? g->m[b - 1][a] : g->m[a][b - 1];
				if (cur == prev)
					run++;
				else {
					if (run >= 5)
						p += 3 + (run - 5);
					run = 1;
				}
			}
			// 1:1:3:1:1 finder-like patterns with 4 light on one side
			for (int b = 0; b + 11 <= s; b++) {
				static const int pat1[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
				static const int pat2[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
				bool m1 = true, m2 = true;
				for (int k = 0; k < 11; k++) {
					int v = pass ? g->m[b + k][a] : g->m[a][b + k];
					m1 &= v == pat1[k];
					m2 &= v == pat2[k];
				}
				p += (m1 ? 40 : 0) + (m2 ? 40 : 0);
			}
		}
	for (int y = 0; y + 1 < s; y++)
		for (int x = 0; x + 1 < s; x++) {
			int c = g->m[y][x];
			if (c == g->m[y][x + 1] && c == g->m[y + 1][x] && c == g->m[y + 1][x + 1])
				p += 3;
		}
	int dark = 0;
	for (int y = 0; y < s; y++)
		for (int x = 0; x < s; x++)
			dark += g->m[y][x];
	int total = s * s;
	int k = (abs(dark * 20 - total * 10) + total - 1) / total - 1;
	if (k > 0)
		p += k * 10;
	return p;
}

bool wt_qr_make(const char* text, WtQr* qr) {
	int len = (int)strlen(text);
	int ver = 0;
	for (int v = 1; v <= 3 && !ver; v++)
		if (len + 2 <= DATA_CW[v]) // 4 bits mode + 8 bits count + 4 bits end fit in 2 bytes
			ver = v;
	if (!ver)
		return false;
	int ndata = DATA_CW[ver], nec = EC_CW[ver];
	uint8_t cw[80] = {0};
	// bit stream: 0100, count (8), bytes, 0000
	int bit = 0;
#define PUT(val, nbits)                                                       \
	for (int _i = (nbits) - 1; _i >= 0; _i--, bit++)                         \
		if (((val) >> _i) & 1)                                                \
			cw[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7));
	PUT(0x4, 4);
	PUT(len, 8);
	for (int i = 0; i < len; i++)
		PUT((uint8_t)text[i], 8);
	int room = ndata * 8 - bit;
	int term = room < 4 ? room : 4;
	bit += term;
	bit = (bit + 7) & ~7;
	for (int i = bit / 8, pad = 0; i < ndata; i++, pad ^= 1)
		cw[i] = pad ? 0x11 : 0xEC;
#undef PUT
	rs_ec(cw, ndata, nec, cw + ndata);
	int ncw = ndata + nec;

	Grid g;
	memset(&g, 0, sizeof(g));
	g.size = 17 + 4 * ver;
	int s = g.size;
	for (int i = 0; i < s; i++) {
		setf(&g, 6, i, i % 2 == 0);
		setf(&g, i, 6, i % 2 == 0);
	}
	finder(&g, 3, 3);
	finder(&g, s - 4, 3);
	finder(&g, 3, s - 4);
	if (ver > 1) { // one alignment pattern, at (s-7, s-7)
		int c = s - 7;
		for (int dy = -2; dy <= 2; dy++)
			for (int dx = -2; dx <= 2; dx++) {
				int d = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
				setf(&g, c + dx, c + dy, d != 1);
			}
	}
	format_bits(&g, 0); // reserves the format areas
	// the codewords, in the zigzag from the bottom right
	int i = 0;
	for (int right = s - 1; right >= 1; right -= 2) {
		if (right == 6)
			right = 5;
		for (int vert = 0; vert < s; vert++)
			for (int j = 0; j < 2; j++) {
				int x = right - j;
				bool up = ((right + 1) & 2) == 0;
				int y = up ? s - 1 - vert : vert;
				if (!g.fn[y][x] && i < ncw * 8) {
					g.m[y][x] = (cw[i >> 3] >> (7 - (i & 7))) & 1;
					i++;
				}
			}
	}
	int best = 0;
	long best_p = -1;
	for (int mask = 0; mask < 8; mask++) {
		Grid t = g;
		apply_mask(&t, mask);
		format_bits(&t, mask);
		long p = penalty(&t);
		if (best_p < 0 || p < best_p) {
			best_p = p;
			best = mask;
		}
	}
	apply_mask(&g, best);
	format_bits(&g, best);
	qr->size = s;
	memcpy(qr->m, g.m, sizeof(qr->m));
	return true;
}
