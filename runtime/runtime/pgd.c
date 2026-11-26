#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/aes.h>

#include "pgd.h"

#define AES_BLOCK 16
#define PGD_HEADER_SIZE 0x90
#define PGD_READ_CHUNK 2048

static const u8 mac_key[AES_BLOCK] = {
    0x12, 0x46, 0x8d, 0x7e, 0x1c, 0x42, 0x20, 0x9b,
    0xba, 0x54, 0x26, 0x83, 0x5e, 0xb0, 0x33, 0x03
};
static const u8 header_key[AES_BLOCK] = {
    0xc4, 0x3b, 0xb6, 0xd6, 0x53, 0xee, 0x67, 0x49,
    0x3e, 0xa9, 0x5f, 0xbc, 0x0c, 0xed, 0x6f, 0x8a
};
static const u8 counter_key[AES_BLOCK] = {
    0x9c, 0x9b, 0x13, 0x72, 0xf8, 0xc6, 0x40, 0xcf,
    0x1c, 0x62, 0xf5, 0xd5, 0x92, 0xdd, 0xb5, 0x82
};
static const u8 fixed_version_key[AES_BLOCK] = {
    0xed, 0xe2, 0x5d, 0x2d, 0xbb, 0xf8, 0x12, 0xe5,
    0x3c, 0x5c, 0x59, 0x32, 0xfa, 0xe3, 0xe2, 0x43
};
static const u8 mac_mask[AES_BLOCK] = {
    0xe3, 0x50, 0xed, 0x1d, 0x91, 0x0a, 0x1f, 0xd0,
    0x29, 0xbb, 0x1c, 0x3e, 0xf3, 0x40, 0x77, 0xfb
};
static const u8 prefix_input_mask[AES_BLOCK] = {
    0x67, 0x8d, 0x7f, 0xa3, 0x2a, 0x9c, 0xa0, 0xd1,
    0x50, 0x8a, 0xd8, 0x38, 0x5e, 0x4b, 0x01, 0x7e
};
static const u8 prefix_output_mask[12] = {
    0x13, 0x5f, 0xa4, 0x7c, 0xab, 0x39, 0x5b, 0xa4,
    0x76, 0xb8, 0xcc, 0xa9
};

static int aes_cbc(u8 *output, const u8 *input, u32 size, const u8 key[16], int direction)
{
    Aes aes;
    int result;

    if (wc_AesInit(&aes, NULL, INVALID_DEVID))
        return -1;

    result = wc_AesSetKey(&aes, key, AES_BLOCK, NULL, direction);
    if (!result) {
        if (direction == AES_ENCRYPTION)
            result = wc_AesCbcEncrypt(&aes, output, input, size);
        else
            result = wc_AesCbcDecrypt(&aes, output, input, size);
    }

    wc_AesFree(&aes);
    return result;
}

static int verify_header_mac(const u8 *header, u32 length, const u8 *version,
    int wrapped)
{
    u8 subkey[AES_BLOCK] = {0};
    u8 message[0x80];
    u8 encrypted[0x80];
    u8 expected[AES_BLOCK];
    unsigned int carry;
    unsigned int difference = 0;
    unsigned int i;

    if (aes_cbc(subkey, subkey, AES_BLOCK, mac_key, AES_ENCRYPTION))
        return -1;

    carry = subkey[0] >> 7;
    for (i = 0; i < AES_BLOCK - 1; i++)
        subkey[i] = (subkey[i] << 1) | (subkey[i + 1] >> 7);

    subkey[AES_BLOCK - 1] = (subkey[AES_BLOCK - 1] << 1) ^ (carry * 0x87);

    memcpy(message, header, length);
    for (i = 0; i < AES_BLOCK; i++)
        message[length - AES_BLOCK + i] ^= subkey[i];

    if (aes_cbc(encrypted, message, length, mac_key, AES_ENCRYPTION))
        return -1;

    for (i = 0; i < AES_BLOCK; i++)
        expected[i] = encrypted[length - AES_BLOCK + i] ^ mac_mask[i] ^ version[i];

    if (aes_cbc(expected, expected, AES_BLOCK, mac_key, AES_ENCRYPTION))
        return -1;
    if (wrapped && aes_cbc(expected, expected, AES_BLOCK, counter_key, AES_ENCRYPTION))
        return -1;

    for (i = 0; i < AES_BLOCK; i++)
        difference |= expected[i] ^ header[length + i];
    return difference ? -1 : 0;
}

static int derive_prefix(u8 prefix[12], const u8 *file_key, const u8 *version)
{
    u8 block[AES_BLOCK];
    unsigned int i;

    for (i = 0; i < sizeof(block); i++)
        block[i] = file_key[i] ^ version[i] ^ prefix_input_mask[i];

    if (aes_cbc(block, block, sizeof(block), header_key, AES_DECRYPTION))
        return -1;

    for (i = 0; i < sizeof(prefix_output_mask); i++)
        prefix[i] = block[i] ^ prefix_output_mask[i];
    return 0;
}

static void write_counter(u8 block[AES_BLOCK], const u8 prefix[12], u32 counter)
{
    memcpy(block, prefix, 12);
    memcpy(block + 12, &counter, sizeof(counter));
}

static int decrypt_bytes(Aes *aes, const u8 prefix[12], u8 *data, u32 aligned_offset, u32 length)
{
    u8 counters[PGD_READ_CHUNK];
    u8 iv[AES_BLOCK] = {0};
    u32 i;

    if (aligned_offset)
        write_counter(iv, prefix, aligned_offset / AES_BLOCK);

    for (i = 0; i < length; i += AES_BLOCK)
        write_counter(counters + i, prefix, (aligned_offset + i) / AES_BLOCK + 1);

    if (wc_AesSetIV(aes, iv) || wc_AesCbcDecrypt(aes, counters, counters, length))
        return -1;

    for (i = 0; i < length; i++)
        data[i] ^= counters[i];

    return 0;
}

static int decrypt_descriptor(u8 *header, const u8 *version)
{
    Aes aes;
    u8 prefix[12];
    int result;

    if (derive_prefix(prefix, header + 0x10, version) < 0 || wc_AesInit(&aes, NULL, INVALID_DEVID))
        return -1;

    result = wc_AesSetKey(&aes, counter_key, AES_BLOCK, NULL, AES_DECRYPTION);
    if (!result)
        result = decrypt_bytes(&aes, prefix, header + 0x30, 0, 0x30);
    wc_AesFree(&aes);
    return result;
}

int psbridge_pgd_open(PspPgd *pgd, int descriptor, const u8 key[16])
{
    u8 header[PGD_HEADER_SIZE];
    u32 key_index, drm_type, size, block_size, data_offset;
    u64 aligned_size, table_size;
    off_t end;
    int count;

    pgd->active = 0;
    if (lseek(descriptor, pgd->header_offset, SEEK_SET) < 0)
        return -1;

    count = read(descriptor, header, sizeof(header));
    if (count < 0)
        return -1;
    if (count < 4 || memcmp(header, "\x00" "PGD", 4))
        return lseek(descriptor, pgd->header_offset, SEEK_SET) < 0 ? -1 : 0;

    if (!key || count != sizeof(header))
        return -1;

    memcpy(&key_index, header + 4, sizeof(key_index));
    memcpy(&drm_type, header + 8, sizeof(drm_type));
    if (drm_type != 1 || verify_header_mac(header, 0x80, fixed_version_key, key_index > 1) ||
        verify_header_mac(header, 0x70, key, key_index > 1) ||
        decrypt_descriptor(header, key))
        return -1;

    memcpy(&size, header + 0x44, sizeof(size));
    memcpy(&block_size, header + 0x48, sizeof(block_size));
    memcpy(&data_offset, header + 0x4c, sizeof(data_offset));
    if (block_size < AES_BLOCK || (block_size & (block_size - 1)) ||
        data_offset < PGD_HEADER_SIZE || size > INT32_MAX)
        return -1;

    aligned_size = ((u64)size + AES_BLOCK - 1) & ~(u64)(AES_BLOCK - 1);
    table_size = ((aligned_size + block_size - 1) / block_size) * AES_BLOCK;
    end = lseek(descriptor, 0, SEEK_END);
    if (end < 0 || pgd->header_offset + (u64)data_offset + aligned_size + table_size > (u64)end ||
        derive_prefix(pgd->counter_prefix, header + 0x30, key) < 0)
        return -1;

    pgd->data_offset = pgd->header_offset + data_offset;
    pgd->size = size;
    pgd->position = 0;
    pgd->active = 1;
    return 0;
}

int psbridge_pgd_read(PspPgd *pgd, int descriptor, void *destination, u32 size)
{
    Aes aes;
    u8 data[PGD_READ_CHUNK];
    u32 done = 0;

    if (pgd->position >= pgd->size || !size)
        return 0;

    if (size > pgd->size - pgd->position)
        size = pgd->size - pgd->position;

    if (wc_AesInit(&aes, NULL, INVALID_DEVID))
        return -1;
    if (wc_AesSetKey(&aes, counter_key, AES_BLOCK, NULL, AES_DECRYPTION)) {
        wc_AesFree(&aes);
        return -1;
    }

    while (done < size) {
        u32 aligned = pgd->position & ~(AES_BLOCK - 1);
        u32 skip = pgd->position - aligned;
        u32 amount = size - done;
        u32 length;

        if (amount > sizeof(data) - skip)
            amount = sizeof(data) - skip;

        length = (skip + amount + AES_BLOCK - 1) & ~(AES_BLOCK - 1);
        if (lseek(descriptor, pgd->data_offset + aligned, SEEK_SET) < 0 ||
            read(descriptor, data, length) != length ||
            decrypt_bytes(&aes, pgd->counter_prefix, data, aligned, length))
            break;

        memcpy((u8 *)destination + done, data + skip, amount);
        pgd->position += amount;
        done += amount;
    }

    wc_AesFree(&aes);
    return done ? (int)done : -1;
}

s64 psbridge_pgd_seek(PspPgd *pgd, s64 offset, int whence)
{
    u32 base = 0;

    if (whence == SEEK_CUR)
        base = pgd->position;
    else if (whence == SEEK_END)
        base = pgd->size;
    else if (whence != SEEK_SET)
        return -1;

    if (offset < -(s64)base || offset > (s64)INT32_MAX - base)
        return -1;

    pgd->position = base + offset;
    return pgd->position;
}
