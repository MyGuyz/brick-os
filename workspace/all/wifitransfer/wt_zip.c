#include "wt_zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t crc_table[256];
static void crc_init(void) {
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_table[i] = c;
	}
}
static uint32_t crc_add(uint32_t crc, const unsigned char* p, size_t n) {
	crc = ~crc;
	while (n--)
		crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

#define FLAGS (0x0008 | 0x0800) // sizes and CRC after the data; names are UTF-8

static void le16(uint8_t* p, uint32_t v) {
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}
static void le32(uint8_t* p, uint32_t v) {
	le16(p, v & 0xFFFF);
	le16(p + 2, v >> 16);
}

static void dos_time(uint32_t t, uint16_t* tm, uint16_t* dt) {
	time_t tt = (time_t)t;
	struct tm lt;
	if (!t || !localtime_r(&tt, &lt) || lt.tm_year < 80) {
		*tm = 0;
		*dt = (1 << 5) | 1; // 1980-01-01
		return;
	}
	*tm = (uint16_t)((lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec / 2));
	*dt = (uint16_t)(((lt.tm_year - 80) << 9) | ((lt.tm_mon + 1) << 5) | lt.tm_mday);
}

uint64_t wt_zip_length(const WtZipItem* items, int n) {
	if (n <= 0 || n > 65535)
		return 0;
	uint64_t total = 0;
	for (int i = 0; i < n; i++) {
		size_t nl = strlen(items[i].name);
		if (items[i].size >= 0xFFFFFFFFull || nl > 0xFFFF)
			return 0;
		total += 30 + nl + items[i].size + 16; // local header, data, descriptor
		total += 46 + nl;					   // central entry
	}
	total += 22;
	return total >= 0xFFFFFFFFull ? 0 : total;
}

bool wt_zip_write(const WtZipItem* items, int n, WtZipOut out, void* ctx) {
	if (!wt_zip_length(items, n))
		return false;
	uint32_t* crcs = calloc((size_t)n, sizeof(uint32_t));
	uint32_t* offs = calloc((size_t)n, sizeof(uint32_t));
	char* buf = malloc(256 * 1024);
	bool ok = crcs && offs && buf;
	uint32_t pos = 0;
	if (!crc_table[1])
		crc_init();
	for (int i = 0; ok && i < n; i++) {
		const WtZipItem* it = &items[i];
		size_t nl = strlen(it->name);
		uint16_t tm, dt;
		dos_time(it->mtime, &tm, &dt);
		uint8_t h[30] = {0};
		le32(h, 0x04034b50);
		le16(h + 4, 20);
		le16(h + 6, FLAGS);
		le16(h + 10, tm);
		le16(h + 12, dt);
		le16(h + 26, (uint32_t)nl);
		offs[i] = pos;
		ok = out(ctx, h, 30) && out(ctx, it->name, nl);
		pos += 30 + (uint32_t)nl;
		FILE* f = ok ? fopen(it->path, "rb") : NULL;
		uint64_t sent = 0;
		uint32_t crc = 0;
		while (ok && f) {
			size_t r = fread(buf, 1, 256 * 1024, f);
			if (!r)
				break;
			if (sent + r > it->size)
				r = (size_t)(it->size - sent);
			crc = crc_add(crc, (const unsigned char*)buf, r);
			ok = out(ctx, buf, r);
			sent += r;
			if (sent >= it->size)
				break;
		}
		if (f)
			fclose(f);
		if (sent != it->size) // the file went or got shorter: the promised length can't be kept
			ok = false;
		crcs[i] = (uint32_t)crc;
		pos += (uint32_t)it->size;
		uint8_t d[16];
		le32(d, 0x08074b50);
		le32(d + 4, crcs[i]);
		le32(d + 8, (uint32_t)it->size);
		le32(d + 12, (uint32_t)it->size);
		ok = ok && out(ctx, d, 16);
		pos += 16;
	}
	uint32_t cd_start = pos, cd_size = 0;
	for (int i = 0; ok && i < n; i++) {
		const WtZipItem* it = &items[i];
		size_t nl = strlen(it->name);
		uint16_t tm, dt;
		dos_time(it->mtime, &tm, &dt);
		uint8_t c[46] = {0};
		le32(c, 0x02014b50);
		le16(c + 4, 20);
		le16(c + 6, 20);
		le16(c + 8, FLAGS);
		le16(c + 12, tm);
		le16(c + 14, dt);
		le32(c + 16, crcs[i]);
		le32(c + 20, (uint32_t)it->size);
		le32(c + 24, (uint32_t)it->size);
		le16(c + 28, (uint32_t)nl);
		le32(c + 42, offs[i]);
		ok = out(ctx, c, 46) && out(ctx, it->name, nl);
		cd_size += 46 + (uint32_t)nl;
	}
	if (ok) {
		uint8_t e[22] = {0};
		le32(e, 0x06054b50);
		le16(e + 8, (uint32_t)n);
		le16(e + 10, (uint32_t)n);
		le32(e + 12, cd_size);
		le32(e + 16, cd_start);
		ok = out(ctx, e, 22);
	}
	free(crcs);
	free(offs);
	free(buf);
	return ok;
}
