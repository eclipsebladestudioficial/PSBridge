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

typedef struct {
    const void **binding;
    const void *target;
} ModuleExport;

typedef struct {
    u64 identity;
    u32 file_size;
    u32 address;
    u32 image_size;
    const void *image;
    const ModuleExport *exports;
    u32 export_count;
} LibraryModule;

extern const u32 psp_module_count;
extern const LibraryModule psp_modules[];

static struct {
    enum ModuleState state;
    int uid;
    const LibraryModule *library;
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

static int register_module(const LibraryModule *library)
{
    unsigned int index, free_index = MODULE_COUNT;
    int uid = NO_MEMORY;
    int interrupts = DIntr();

    for (index = 0; index < MODULE_COUNT; index++) {
        if (modules[index].state == MODULE_UNUSED) {
            if (free_index == MODULE_COUNT)
                free_index = index;
        } else if (library && modules[index].library == library) {
            uid = UNKNOWN_MODULE;
            break;
        }
    }
    if (uid == NO_MEMORY && free_index != MODULE_COUNT) {
        void *image = library ? psbridge_ram_pointer(library->address, library->image_size) : NULL;

        if (!library || image) {
            if (library) {
                memcpy(image, library->image, library->image_size);
                for (index = 0; index < library->export_count; index++)
                    *library->exports[index].binding = library->exports[index].target;
            }
            uid = next_uid++;
            modules[free_index].uid = uid;
            modules[free_index].library = library;
            modules[free_index].state = MODULE_LOADED;
        }
    }
    if (interrupts)
        EIntr();
    return uid;
}

static int load_library_module(int descriptor, s64 start)
{
    u8 buffer[512];
    u64 identity = 14695981039346656037ull;
    s64 size;
    u32 index;

    if (!psp_module_count)
        return UNSUPPORTED_PRX;

    size = psbridge_file_seek(descriptor, 0, 0, SEEK_END);
    if (size < start)
        return FILE_ERROR;
    size -= start;
    for (index = 0; index < psp_module_count; index++) {
        if (psp_modules[index].file_size == size)
            break;
    }
    if (index == psp_module_count)
        return UNSUPPORTED_PRX;
    if (psbridge_file_seek(descriptor, (u32)start, (u64)start >> 32, SEEK_SET) != start)
        return FILE_ERROR;

    for (s64 remaining = size; remaining > 0;) {
        unsigned int count = remaining < sizeof(buffer) ? (unsigned int)remaining : sizeof(buffer);

        if (psbridge_file_read(descriptor, buffer, count) != (int)count)
            return FILE_ERROR;
        for (index = 0; index < count; index++)
            identity = (identity ^ buffer[index]) * 1099511628211ull;
        remaining -= count;
    }

    for (index = 0; index < psp_module_count; index++) {
        if (psp_modules[index].file_size == size && psp_modules[index].identity == identity)
            return register_module(&psp_modules[index]);
    }
    return UNSUPPORTED_PRX;
}

static void unbind_library_module(const LibraryModule *library)
{
    u32 index;

    if (!library)
        return;
    for (index = 0; index < library->export_count; index++) {
        const void **binding = library->exports[index].binding;
        const void *target = NULL;
        int uid = -1;
        unsigned int slot;

        for (slot = 0; slot < MODULE_COUNT; slot++) {
            const LibraryModule *other = modules[slot].library;
            u32 entry;

            if (modules[slot].state == MODULE_UNUSED || !other || modules[slot].uid <= uid)
                continue;
            for (entry = 0; entry < other->export_count; entry++) {
                if (other->exports[entry].binding == binding) {
                    uid = modules[slot].uid;
                    target = other->exports[entry].target;
                    break;
                }
            }
        }
        *binding = target;
    }
}

int psbridge_load_firmware_module(int descriptor)
{
    u8 header[0x30];
    s64 start = psbridge_file_seek(descriptor, 0, 0, SEEK_CUR);
    s64 end;
    u32 size;
    unsigned int index;

    if (start < 0 || psbridge_file_read(descriptor, header, sizeof(header)) != sizeof(header))
        return FILE_ERROR;
    if (!memcmp(header, "\177ELF", 4))
        return load_library_module(descriptor, start);
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

    return register_module(NULL);
}

extern inline __attribute__((always_inline)) int psbridge_change_firmware_module(int uid, u32 operation, u32 status_address)
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
                unbind_library_module(modules[index].library);
                modules[index].library = NULL;
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
