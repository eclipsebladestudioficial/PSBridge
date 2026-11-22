#ifndef PSBRIDGE_PGD_H
#define PSBRIDGE_PGD_H

#include <tamtypes.h>

typedef struct {
    u8 counter_prefix[12];
    u32 header_offset;
    u32 data_offset;
    u32 size;
    u32 position;
    int active;
} PspPgd;

int psbridge_pgd_open(PspPgd *pgd, int descriptor, const u8 key[16]);
int psbridge_pgd_read(PspPgd *pgd, int descriptor, void *destination, u32 size);
s64 psbridge_pgd_seek(PspPgd *pgd, s64 offset, int whence);

#endif
