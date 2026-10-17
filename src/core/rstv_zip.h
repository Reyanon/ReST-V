/*  ReST-V zip access (junzip), shared by the ST-V loader and the System C2 core (ADR-107).
    This file is part of ReST-V. GPL-2.0-or-later.
*/
#ifndef RSTV_ZIP_H
#define RSTV_ZIP_H
#include <stdio.h>
#include "core.h"
#include "junzip.h"
#ifdef __cplusplus
extern "C" {
#endif
#define MAX_ZIP_ENTRIES 64

typedef struct {
	char name[128];
	JZFileHeader header;
} ZipEntry;

typedef struct {
	FILE *fp;
	JZFile *zip;
	int count;
	ZipEntry e[MAX_ZIP_ENTRIES];
} Zip;

int zip_open(Zip *z, const char *path);
void zip_close(Zip *z);
const ZipEntry *zip_find(const Zip *z, const char *name, u32 crc); // by CRC, then by name
u8 *zip_read(Zip *z, const ZipEntry *e, u32 *size);                 // malloc'd, caller frees
u8 *rstv_zip_load(const char *path, const char *name, u32 crc, u32 *size);
#ifdef __cplusplus
}
#endif
#endif
