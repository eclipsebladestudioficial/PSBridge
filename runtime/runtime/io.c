#include <fcntl.h>
#include <dirent.h>
#include <delaythread.h>
#include <kernel.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <timer.h>

#include "psbridge.h"
#include "psbridge_log.h"


#define PATH_SIZE 512
#define UMD_DESCRIPTOR 0x4000
#define UMD_HANDLES 8

#define UMD_FILES 64

typedef struct {
    int used;
    int raw;
    int file_index;
    s64 position;
    u64 start;
    u64 length;
} UmdHandle;

typedef struct {
    int used;
    int descriptor;
    char path[PATH_SIZE];
    u64 size;
    u32 first_sector;
    u32 sector_count;
} UmdFile;

static UmdHandle umd_handles[UMD_HANDLES];
static int umd_active = 1;
static int umd_present = -1;
static int umd_callback = -1;

#define PSP_DIR_LIMIT 12
#define PSP_DIR_BASE 0x5000

typedef struct {
    int used;
    DIR *stream;
    char path[PATH_SIZE];
} PspDirHandle;

static PspDirHandle directory_handles[PSP_DIR_LIMIT];
static char current_psp_dir[PATH_SIZE] = "disc0:/";

static UmdFile umd_files[UMD_FILES];
static u32 next_umd_sector;
static int umd_file_lock = -1;
typedef struct {
    u16 year;
    u16 month;
    u16 day;
    u16 hour;
    u16 minute;
    u16 second;
    u32 microsecond;
} PspDateTime;

typedef struct {
    u32 mode;
    u32 attributes;
    u64 size;
    PspDateTime created;
    PspDateTime accessed;
    PspDateTime modified;
    u32 private_data[6];
} PspFileStat;
typedef struct {
    PspFileStat stat;
    char name[256];
    u32 private_data;
    u32 dummy;
} PspDirent;

static int host_path(const char *path, char *output)
{
    const char *game_path = psbridge_game_pointer(path);
    const char *prefix;
    const char *relative;
    char resolved[PATH_SIZE];
    int length;

    if (!game_path)
        return -1;

    if (strncmp(game_path, "disc0:/", 7) && strncmp(game_path, "ms0:/", 5) &&
        strncmp(game_path, "disc0:", 6) && strncmp(game_path, "ms0:", 4)) {
        length = snprintf(resolved, sizeof(resolved), "%s%s", current_psp_dir, game_path);
        if (length < 0 || length >= (int)sizeof(resolved))
            return -1;
        game_path = resolved;
    }

    if (!strncmp(game_path, "disc0:/sce_lbn", 14))
        return -1;

    if (!strncmp(game_path, "disc0:/", 7)) {
        prefix = "host:";
        relative = game_path + 7;
    } else if (!strncmp(game_path, "ms0:/", 5)) {
        prefix = "host:ms0/";
        relative = game_path + 5;
    } else if (!strcmp(game_path, "disc0:")) {
        prefix = "host:";
        relative = "";
    } else if (!strcmp(game_path, "ms0:")) {
        prefix = "host:ms0/";
        relative = "";
    } else {
        return -1;
    }

    length = snprintf(output, PATH_SIZE, "%s%s", prefix, relative);
    return length >= 0 && length < PATH_SIZE ? 0 : -1;
}

static int umd_path(const char *path)
{
    const char *game_path = psbridge_game_pointer(path);

    if (!game_path)
        return 0;

    return !strcmp(game_path, "umd0:") || !strcmp(game_path, "umd1:");
}

static UmdHandle *umd_handle(int descriptor)
{
    int index = descriptor - UMD_DESCRIPTOR;

    if (index < 0 || index >= UMD_HANDLES || !umd_handles[index].used)
        return NULL;

    return &umd_handles[index];
}

static int host_file_exists(const char *path)
{
    int descriptor = open(path, O_RDONLY);

    if (descriptor < 0)
        return 0;

    close(descriptor);
    return 1;
}

static UmdFile *register_umd_file(const char *path, u64 size)
{
    u64 sectors;
    size_t path_length;
    int index;
    int descriptor;

    for (index = 0; index < UMD_FILES; index++) {
        if (umd_files[index].used && !strcmp(umd_files[index].path, path))
            return &umd_files[index];
    }

    sectors = (size + 2047) / 2048;
    path_length = strlen(path);

    if (path_length >= PATH_SIZE || sectors > 0xffffffff - next_umd_sector) {
        return NULL;
    }

    descriptor = open(path, O_RDONLY);

    if (descriptor < 0)
        return NULL;

    for (index = 0; index < UMD_FILES; index++) {
        UmdFile *file = &umd_files[index];

        if (file->used)
            continue;

        memset(file, 0, sizeof(*file));

        memcpy(file->path, path, path_length + 1);

        file->used = 1;
        file->descriptor = descriptor;
        file->size = size;
        file->first_sector = next_umd_sector;
        file->sector_count = sectors;

        next_umd_sector += file->sector_count;
        return file;
    }

    close(descriptor);
    return NULL;
}

static int read_umd_file(UmdFile *file, u64 offset, void *buffer, u32 size)
{
    ee_sema_t config;
    int result;

    if (umd_file_lock < 0) {
        memset(&config, 0, sizeof(config));
        config.init_count = 1;
        config.max_count = 1;
        umd_file_lock = CreateSema(&config);

        if (umd_file_lock < 0)
            return -1;
    }

    result = WaitSema(umd_file_lock);

    if (result < 0)
        return result;

    if (lseek(file->descriptor, offset, SEEK_SET) < 0)
        result = -1;
    else
        result = read(file->descriptor, buffer, size);

    SignalSema(umd_file_lock);
    return result;
}

static UmdFile *find_umd_file(s64 sector)
{
    int index;

    if (sector < 0)
        return NULL;

    for (index = 0; index < UMD_FILES; index++) {
        UmdFile *file = &umd_files[index];
        u64 end;

        if (!file->used)
            continue;

        end = (u64)file->first_sector + file->sector_count;

        if (
            (u64)sector >= file->first_sector &&
            (u64)sector < end
        )
            return file;
    }

    return NULL;
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;

    return -1;
}

static int read_hex(const char **text, u64 *value)
{
    int digit;
    int count = 0;

    *value = 0;

    while ((digit = hex_value(**text)) >= 0) {
        if (*value > 0x0fffffffffffffffULL)
            return -1;

        *value = (*value << 4) | digit;
        (*text)++;
        count++;
    }

    return count ? 0 : -1;
}

static int lbn_path(const char *path, u64 *sector, u64 *size)
{
    const char *game_path = psbridge_game_pointer(path);
    const char *cursor;

    if (!game_path || strncmp(game_path, "disc0:/sce_lbn0x", 16))
        return 0;

    cursor = game_path + 16;

    if (read_hex(&cursor, sector) < 0 || strncmp(cursor, "_size0x", 7))
        return -1;

    cursor += 7;

    if (read_hex(&cursor, size) < 0 || *cursor)
        return -1;

    return 1;
}

static int new_umd_handle(void)
{
    int index;

    for (index = 0; index < UMD_HANDLES; index++) {
        if (!umd_handles[index].used) {
            memset(&umd_handles[index], 0, sizeof(umd_handles[index]));
            umd_handles[index].used = 1;
            return UMD_DESCRIPTOR + index;
        }
    }

    return -1;
}

int psbridge_umd_present(void)
{
    if (umd_present < 0) {
        umd_present = host_file_exists("host:UMD_DATA.BIN") &&
            host_file_exists("host:PSP_GAME/PARAM.SFO");
    }

    return umd_present;
}

u32 psbridge_umd_state(void)
{
    if (!psbridge_umd_present())
        return 0x01;

    return umd_active ? 0x32 : 0x12;
}

int psbridge_umd_activate(int mode, const char *name)
{
    const char *device = psbridge_game_pointer(name);

    if ((mode != 1 && mode != 2) || !device || strcmp(device, "disc0:"))
        return -1;

    if (!psbridge_umd_present())
        return -1;

    umd_active = 1;

    if (umd_callback >= 0)
        psbridge_notify_callback(umd_callback, psbridge_umd_state());

    return 0;
}

int psbridge_umd_deactivate(int mode, const char *name)
{
    const char *device = psbridge_game_pointer(name);

    if ((mode != 1 && mode != 2) || !device || strcmp(device, "disc0:"))
        return -1;

    umd_active = 0;

    if (umd_callback >= 0)
        psbridge_notify_callback(umd_callback, psbridge_umd_state());

    return 0;
}

int psbridge_umd_wait(u32 state)
{
    return (psbridge_umd_state() & state) ? 0 : -1;
}

int psbridge_umd_register_callback(int uid)
{
    if (psbridge_callback_count(uid) < 0)
        return -1;

    umd_callback = uid;
    return 0;
}

int psbridge_umd_unregister_callback(int uid)
{
    if (umd_callback != uid)
        return -1;

    umd_callback = -1;
    return 0;
}

int psbridge_umd_wait_cb(u32 state, u32 timeout)
{
    u32 seconds;
    u32 microseconds;
    u32 start_seconds;
    u32 start_microseconds;
    u64 started;

    TimerBusClock2USec(
        GetTimerSystemTime(),
        &start_seconds,
        &start_microseconds
    );

    started = (u64)start_seconds * 1000000 + start_microseconds;

    for (;;) {
        u64 now;

        if (psbridge_umd_state() & state)
            return 0;

        psbridge_check_callbacks();
        TimerBusClock2USec(GetTimerSystemTime(), &seconds, &microseconds);
        now = (u64)seconds * 1000000 + microseconds;

        if (timeout && now - started >= timeout)
            return (int)0x800201a8u;

        DelayThread(1000);
    }
}

static int open_flags(int flags)
{
    int result;

    if ((flags & 3) == 2)
        result = O_WRONLY;
    else if ((flags & 3) == 3)
        result = O_RDWR;
    else
        result = O_RDONLY;

    if (flags & 0x0100)
        result |= O_APPEND;
    if (flags & 0x0200)
        result |= O_CREAT;
    if (flags & 0x0400)
        result |= O_TRUNC;
    if (flags & 0x0800)
        result |= O_EXCL;

    return result;
}

int psbridge_file_chdir(const char *path)
{
    const char *guest = psbridge_game_pointer(path);
    char translated[PATH_SIZE];
    char resolved[PATH_SIZE];
    struct stat info;
    int count;
    size_t length;

    if (!guest || host_path(path, translated) < 0)
        return -1;

    if (strcmp(translated, "host:") && strcmp(translated, "host:ms0/") &&
        (stat(translated, &info) < 0 || !S_ISDIR(info.st_mode)))
        return -1;

    if (strncmp(guest, "disc0:/", 7) && strncmp(guest, "ms0:/", 5))
        count = snprintf(resolved, sizeof(resolved), "%s%s", current_psp_dir, guest);
    else
        count = snprintf(resolved, sizeof(resolved), "%s", guest);

    if (count < 0 || count >= (int)sizeof(resolved))
        return -1;

    length = strlen(resolved);
    if (!length)
        return -1;
    if (resolved[length - 1] != '/') {
        if (length + 1 >= sizeof(resolved))
            return -1;
        resolved[length++] = '/';
        resolved[length] = 0;
    }
    memcpy(current_psp_dir, resolved, length + 1);
    return 0;
}

int psbridge_file_dopen(const char *path)
{
    char translated[PATH_SIZE];
    DIR *dir;
    int index;

    if (host_path(path, translated) < 0)
        return -1;
    dir = opendir(translated);
    if (!dir)
        return -1;

    for (index = 0; index < PSP_DIR_LIMIT; index++) {
        if (!directory_handles[index].used) {
            PspDirHandle *handle = &directory_handles[index];
            memset(handle, 0, sizeof(*handle));
            handle->used = 1;
            handle->stream = dir;
            snprintf(handle->path, sizeof(handle->path), "%s", translated);
            return PSP_DIR_BASE + index;
        }
    }
    closedir(dir);
    return -1;
}

int psbridge_file_dread(int uid, void *output)
{
    int index = uid - PSP_DIR_BASE;
    PspDirHandle *handle;
    PspDirent *destination = psbridge_game_pointer(output);
    struct dirent *entry;
    char full_path[PATH_SIZE];
    struct stat source;
    int n;

    if (index < 0 || index >= PSP_DIR_LIMIT || !directory_handles[index].used || !destination)
        return -1;

    handle = &directory_handles[index];
    entry = readdir(handle->stream);
    if (!entry)
        return 0;

    memset(destination, 0, sizeof(*destination));
    snprintf(destination->name, sizeof(destination->name), "%s", entry->d_name);
    n = snprintf(full_path, sizeof(full_path), "%s/%s", handle->path, entry->d_name);
    if (n >= 0 && n < (int)sizeof(full_path) && stat(full_path, &source) >= 0) {
        int is_directory = S_ISDIR(source.st_mode);
        destination->stat.mode = is_directory ? 0x11ff : 0x21ff;
        destination->stat.attributes = is_directory ? 0x10 : 0x20;
        destination->stat.size = (u64)source.st_size;
    }
    return 1;
}

int psbridge_file_dclose(int uid)
{
    int index = uid - PSP_DIR_BASE;
    PspDirHandle *handle;

    if (index < 0 || index >= PSP_DIR_LIMIT || !directory_handles[index].used)
        return -1;
    handle = &directory_handles[index];
    closedir(handle->stream);
    memset(handle, 0, sizeof(*handle));
    return 0;
}

int psbridge_file_open(const char *path, int flags, int mode)
{
    char translated[PATH_SIZE];
    UmdFile *file;
    UmdHandle *handle;
    u64 sector = 0;
    u64 size = 0;
    u64 offset;
    int descriptor;
    int lbn;

    if (umd_path(path)) {
        if ((flags & 3) == 2 || (flags & 3) == 3 || !psbridge_umd_present())
            return -1;

        descriptor = new_umd_handle();

        if (descriptor < 0)
            return -1;

        umd_handle(descriptor)->raw = 1;
        return descriptor;
    }

    lbn = lbn_path(path, &sector, &size);

    if (lbn) {
        if (lbn < 0 || (flags & 3) == 2 || (flags & 3) == 3)
            return -1;

        file = find_umd_file(sector);

        if (!file)
            return -1;

        offset = (sector - file->first_sector) * 2048;

        if (offset > file->size || size > file->size - offset)
            return -1;

        descriptor = new_umd_handle();

        if (descriptor < 0)
            return -1;

        handle = umd_handle(descriptor);
        handle->file_index = file - umd_files;
        handle->start = offset;
        handle->length = size;

        return descriptor;
    }

    if (host_path(path, translated) < 0)
        return -1;

    return open(translated, open_flags(flags), mode);
}

#define PSP_ASYNC_LIMIT 64

typedef struct {
    int used;
    int descriptor;
    s64 result;
} PspAsyncHandle;

static PspAsyncHandle async_handles[PSP_ASYNC_LIMIT];

static int register_async_result(int descriptor, s64 result)
{
    int index;
    int free_slot = -1;
    for (index = 0; index < PSP_ASYNC_LIMIT; index++) {
        if (async_handles[index].used && async_handles[index].descriptor == descriptor) {
            async_handles[index].result = result;
            return 0;
        }
        if (!async_handles[index].used && free_slot < 0)
            free_slot = index;
    }
    if (free_slot < 0)
        return -1;
    async_handles[free_slot].used = 1;
    async_handles[free_slot].descriptor = descriptor;
    async_handles[free_slot].result = result;
    return 0;
}

int psbridge_file_open_async(const char *path, int flags, int mode)
{
    int descriptor = psbridge_file_open(path, flags, mode);
    if (descriptor < 0)
        return descriptor;
    if (register_async_result(descriptor, descriptor) < 0) {
        psbridge_file_close(descriptor);
        return -1;
    }
    return descriptor;
}

int psbridge_file_async_wait(int descriptor, void *output, int polling)
{
    s64 *result = psbridge_game_pointer(output);
    int index;
    if (!result)
        return -1;
    for (index = 0; index < PSP_ASYNC_LIMIT; index++) {
        if (!async_handles[index].used || async_handles[index].descriptor != descriptor)
            continue;
        *result = async_handles[index].result;
        if (!polling)
            memset(&async_handles[index], 0, sizeof(async_handles[index]));
        return 0;
    }
    return -1;
}

int psbridge_file_read_async(int descriptor, void *buffer, u32 size)
{
    int result = psbridge_file_read(descriptor, buffer, size);
    if (result < 0)
        return result;
    return register_async_result(descriptor, result);
}

int psbridge_file_write_async(int descriptor, const void *buffer, u32 size)
{
    int result = psbridge_file_write(descriptor, buffer, size);
    if (result < 0)
        return result;
    return register_async_result(descriptor, result);
}

int psbridge_file_close_async(int descriptor)
{
    int result = psbridge_file_close(descriptor);
    if (result < 0)
        return result;
    return register_async_result(descriptor, result);
}

int psbridge_file_close(int descriptor)
{
    UmdHandle *handle = umd_handle(descriptor);

    if (handle) {
        memset(handle, 0, sizeof(*handle));
        return 0;
    }

    return close(descriptor);
}

int psbridge_file_read(int descriptor, void *buffer, u32 size)
{
    void *destination = psbridge_game_pointer(buffer);
    UmdHandle *handle = umd_handle(descriptor);

    if (!destination && size)
        return -1;

    if (handle) {
        if (!handle->raw) {
            UmdFile *file = &umd_files[handle->file_index];
            u64 remaining;
            int bytes_read;

            if ((u64)handle->position >= handle->length)
                return 0;

            remaining = handle->length - handle->position;

            if (size > remaining)
                size = remaining;

            bytes_read = read_umd_file(
                file,
                handle->start + handle->position,
                destination,
                size
            );

            if (bytes_read > 0)
                handle->position += bytes_read;

            return bytes_read;
        }

        UmdFile *file = find_umd_file(handle->position);

        u64 sector_offset;
        u64 byte_offset;
        u64 byte_count;
        int bytes_read;

        if (!file || size > 0x7fffffff / 2048)
            return -1;

        sector_offset = handle->position - file->first_sector;

        if (size > file->sector_count - sector_offset)
            return -1;

        byte_offset = sector_offset * 2048;
        byte_count = size * 2048;

        if (byte_offset + byte_count > file->size)
            return -1;

        bytes_read = read_umd_file(file, byte_offset, destination, byte_count);

        if (bytes_read != byte_count)
            return -1;

        handle->position += size;
        return size;
        
    }

    return read(descriptor, destination, size);
}

int psbridge_file_write(int descriptor, const void *buffer, u32 size)
{
    const void *source = psbridge_game_pointer(buffer);

    if (umd_handle(descriptor))
        return -1;

    if (!source && size)
        return -1;

    if ((descriptor == 1 || descriptor == 2) && size >= 30 && size < 1024) {
        char message[1024];
        const char *found;
        size_t length = size;
        if (length >= sizeof(message))
            length = sizeof(message) - 1;
        memcpy(message, source, length);
        message[length] = 0;
        if (strstr(message, "DrawList")) {
            PRINT_LOG("lista de desenho veio gozar na tela\n");
            return size;
        }
        if (strstr(message, "CallbackFunction")) {
            PRINT_LOG("umd chamou a callback pra putaria\n");
            return size;
        }
        if (strstr(message, "SCE_UMD_READABLE")) {
            PRINT_LOG("umd ta prontinha pra ser lida\n");
            return size;
        }
        found = strstr(message, "couldn't find file ");
        if (found) {
            const char *name = found + sizeof("couldn't find file ") - 1;
            const char *end = strchr(name, ' ');
            int len = end ? (int)(end - name) : (int)strlen(name);
            if (len > 0 && len < 256)
                PRINT_LOG("cache deu uma broxada procurando %.*s, vou tentar abrir o arquivo direto\n", len, name);
            return size;
        }
    }

    return write(descriptor, source, size);
}

s64 psbridge_file_seek(int descriptor, u32 low, u32 high, int whence)
{
    s64 offset = (s64)(((u64)high << 32) | low);
    UmdHandle *handle = umd_handle(descriptor);

    if (handle) {
        s64 position;

        if (whence == SEEK_SET)
            position = offset;
        else if (whence == SEEK_CUR)
            position = handle->position + offset;
        else if (whence == SEEK_END && !handle->raw)
            position = handle->length + offset;
        else
            return -1;

        if (position < 0 || (!handle->raw && (u64)position > handle->length))
            return -1;

        handle->position = position;
        return position;
    }

    return lseek(descriptor, offset, whence);
}

int psbridge_file_stat(const char *path, void *output)
{
    char translated[PATH_SIZE];
    struct stat source;
    PspFileStat *destination = psbridge_game_pointer(output);

    const char *game_path = psbridge_game_pointer(path);
    UmdFile *umd_file = NULL;

    if (!destination)
        return -1;

    if (umd_path(path)) {
        if (!psbridge_umd_present())
            return -1;

        memset(destination, 0, sizeof(*destination));
        destination->mode = 0x21ff;
        destination->attributes = 0x20;
        return 0;
    }

    if (host_path(path, translated) < 0)
        return -1;

    if (stat(translated, &source) < 0)
        return -1;

    if (game_path && !strncmp(game_path, "disc0:/", 7))
        umd_file = register_umd_file(translated, source.st_size);

    memset(destination, 0, sizeof(*destination));
    destination->mode = S_ISDIR(source.st_mode) ? 0x11ff : 0x21ff;
    destination->attributes = S_ISDIR(source.st_mode) ? 0x10 : 0x20;
    destination->size = source.st_size;

    if (umd_file)
        destination ->private_data[0] = umd_file->first_sector;

    return 0;
}

int psbridge_file_remove(const char *path)
{
    char translated[PATH_SIZE];

    if (host_path(path, translated) < 0)
        return -1;

    return remove(translated);
}

int psbridge_file_rename(const char *old_path, const char *new_path)
{
    char old_translated[PATH_SIZE];
    char new_translated[PATH_SIZE];

    if (
        host_path(old_path, old_translated) < 0 ||
        host_path(new_path, new_translated) < 0
    )
        return -1;

    return rename(old_translated, new_translated);
}

int psbridge_file_ioctl(
    int descriptor,
    u32 command,
    void *input,
    u32 input_size,
    void *output,
    u32 output_size
)
{
    UmdHandle *handle = umd_handle(descriptor);
    void *input_data = psbridge_game_pointer(input);
    void *output_data = psbridge_game_pointer(output);
    UmdFile *file;
    u64 offset;
    u32 whence;
    u32 amount;
    s64 position;

    if (!handle)
        return -1;

    file = handle->raw ? NULL : &umd_files[handle->file_index];

    if (command == 0x01020003) {
        if (!output_data || output_size < 4)
            return -1;

        *(u32 *)output_data = 2048;
        return 0;
    }

    if (command == 0x01020004) {
        if (!output_data || output_size < 4)
            return -1;

        *(u32 *)output_data = handle->position;
        return 0;
    }

    if (command == 0x01010005 || command == 0x01f100a6) {
        if (!input_data || input_size < 4)
            return -1;

        memcpy(&offset, input_data, sizeof(offset));
        memcpy(&whence, (u8 *)input_data + 12, sizeof(whence));

        if (whence == SEEK_SET)
            position = offset;
        else if (whence == SEEK_CUR)
            position = handle->position + offset;
        else if (whence == SEEK_END)
            position = (handle->raw ? 0 : handle->length) + offset;
        else
            return -1;

        if (position < 0 || (!handle->raw && (u64)position > handle->length))
            return -1;

        handle->position = position;
        return 0;
    }

    if (command == 0x01020006) {
        if (!file || !output_data || output_size < 4)
            return -1;

        *(u32 *)output_data = file->first_sector + handle->start / 2048;
        return 0;
    }

    if (command == 0x01020007) {
        if (!file || !output_data || output_size < 8)
            return -1;

        *(u64 *)output_data = handle->length;
        return 0;
    }

    if (command == 0x01030008) {
        if (!input_data || input_size < 4)
            return -1;

        amount = *(u32 *)input_data;

        if (!output_data || amount > output_size)
            return -1;

        return psbridge_file_read(descriptor, output, amount);
    }

    if (command == 0x01d20001) {
        if (!handle->raw || !output_data || output_size < 4)
            return -1;

        *(u32 *)output_data = handle->position;
        return 0;
    }

    if (command == 0x01f30003) {
        if (!handle->raw || !input_data || input_size < 4)
            return -1;

        amount = *(u32 *)input_data;

        if (!output_data || amount > output_size / 2048)
            return -1;

        return psbridge_file_read(descriptor, output, amount);
    }

    return -1;
}
