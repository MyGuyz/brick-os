// A tiny QR code maker for one short link (byte mode, error level M, versions 1-3: up to 42 bytes).
#ifndef WT_QR_H
#define WT_QR_H

#include <stdbool.h>

#define WT_QR_MAX 29 // version 3

typedef struct {
	int size;							   // 21, 25 or 29 modules
	unsigned char m[WT_QR_MAX][WT_QR_MAX]; // [row][col], 1 = dark
} WtQr;

// False when the text is longer than 42 bytes.
bool wt_qr_make(const char* text, WtQr* qr);

#endif
