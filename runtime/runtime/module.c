#include <kernel.h>
#include <stdio.h>
#include <string.h>

#include "psbridge.h"

#define MODULE_UID_BASE 0xa000
#define MODULE_COUNT 32
#define FILE_ERROR ((int)0x80020130u)
#define UNKNOWN_MODULE ((int)0x8002012eu)
#define UNSUPPORTED_PRX ((int)0x80020148u)
#define NO_MEMORY ((int)0x80020190u)
#define ALREADY_STARTED ((int)0x80020133u)
#define NOT_STARTED ((int)0x80020134u)
#define NOT_STOPPED ((int)0x80020137u)

enum ModuleState {
    MODULE_UNUSED,
    MODULE_LOADED,
    MODULE_STARTED,
    MODULE_STOPPED,
};

static struct {
    enum ModuleState state;
    int uid;
} modules[MODULE_COUNT];
static int next_uid = MODULE_UID_BASE;

static const char *const resident_modules[] = {
    "sceSAScore",
    "sceAudiocodec_Driver",
    "sceATRAC3plus_Library",
    "sceVideocodec_Driver",
    "sceMpegbase_Driver",
    "sceMpeg_library",
    "scePsmf_library",
    "scePsmfP_library",
    "sceFont_Library",
};

static u32 read_le32(const u8 *data)
{
    return data[0] | (u32)data[1] << 8 | (u32)data[2] << 16 | (u32)data[3] << 24;
}

int psbridge_load_firmware_module(int descriptor)
{
    u8 header[0x30];
    s64 start = psbridge_file_seek(descriptor, 0, 0, SEEK_CUR);
    s64 end;
    u32 size;
    unsigned int index;
    int uid = NO_MEMORY;
    int interrupts;

    if (start < 0 || psbridge_file_read(descriptor, header, sizeof(header)) != sizeof(header))
        return FILE_ERROR;
    if (!memcmp(header, "~SCE", 4)) {
        u32 offset = read_le32(header + 4);

        if (offset < sizeof(header) ||
            psbridge_file_seek(descriptor, (u32)(start + offset),
                               (u64)(start + offset) >> 32, SEEK_SET) < 0 ||
            psbridge_file_read(descriptor, header, sizeof(header)) != sizeof(header))
            return FILE_ERROR;
        start += offset;
    }
    if (memcmp(header, "~PSP", 4))
        return UNSUPPORTED_PRX;

    size = read_le32(header + 0x2c);
    end = psbridge_file_seek(descriptor, 0, 0, SEEK_END);
    if (size < 0x150 || end < start || (u64)(end - start) < size)
        return FILE_ERROR;

    for (index = 0; index < sizeof(resident_modules) / sizeof(resident_modules[0]); index++) {
        if (!strncmp((const char *)header + 0x0a, resident_modules[index], 28))
            break;
    }
    if (index == sizeof(resident_modules) / sizeof(resident_modules[0]))
        return UNSUPPORTED_PRX;

    interrupts = DIntr();
    for (index = 0; index < MODULE_COUNT; index++) {
        if (modules[index].state == MODULE_UNUSED) {
            uid = next_uid++;
            modules[index].uid = uid;
            modules[index].state = MODULE_LOADED;
            break;
        }
    }
    if (interrupts)
        EIntr();
    return uid;
}

int psbridge_change_firmware_module(int uid, u32 operation, u32 status_address)
{
    int interrupts = DIntr();
    int result = UNKNOWN_MODULE;
    unsigned int index;

    for (index = 0; index < MODULE_COUNT; index++) {
        enum ModuleState state = modules[index].state;
        int *status;

        if (state == MODULE_UNUSED || modules[index].uid != uid)
            continue;
        if (operation == 0x2e0911aa) {
            result = NOT_STOPPED;
            if (state != MODULE_STARTED) {
                modules[index].state = MODULE_UNUSED;
                result = uid;
            }
            break;
        }

        if (operation == 0x50f0c1ec) {
            result = ALREADY_STARTED;
            if (state != MODULE_STARTED) {
                modules[index].state = MODULE_STARTED;
                result = uid;
            }
        } else {
            result = NOT_STARTED;
            if (state == MODULE_STARTED) {
                modules[index].state = MODULE_STOPPED;
                result = uid;
            }
        }
        status = status_address ? psbridge_game_pointer((void *)status_address) : NULL;
        if (status && result >= 0)
            *status = 0;
        break;
    }
    if (interrupts)
        EIntr();
    return result;
}
