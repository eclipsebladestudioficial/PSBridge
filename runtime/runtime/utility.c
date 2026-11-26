#include <stdint.h>

#include "psbridge.h"
#include "psbridge_log.h"

#define DIALOG_NONE 0
#define DIALOG_INIT 1
#define DIALOG_VISIBLE 2
#define DIALOG_QUIT 3
#define DIALOG_FINISHED 4

#define SAVEDATA_LOAD_NO_DATA 0x80110307

typedef struct {
    u32 size;
    s32 language;
    s32 button_swap;
    s32 graphics_thread;
    s32 access_thread;
    s32 font_thread;
    s32 sound_thread;
    s32 result;
    s32 reserved[4];
} UtilityDialog;

typedef struct {
    UtilityDialog dialog;
    s32 mode;
} SavedataParams;

typedef struct {
    UtilityDialog dialog;
    s32 unknown;
    s32 mode;
    u32 error;
    char message[512];
    u32 options;
    s32 button;
} MessageParams;

typedef struct {
    s32 unknown0;
    s32 unknown1;
    s32 language;
    s32 unknown2;
    s32 input_type;
    s32 lines;
    s32 unknown3;
    u32 description;
    u32 input;
    u32 output_length;
    u32 output;
    s32 result;
    u32 output_limit;
} OskData;

typedef struct {
    UtilityDialog dialog;
    s32 field_count;
    u32 fields;
    s32 state;
    s32 unknown;
} OskParams;

static int osk_status;

static int savedata_status;
static int message_status;

static u32 osk_params_address;

static OskParams *osk_params(void)
{
    return psbridge_game_pointer((void *)(uintptr_t)osk_params_address);
}

static int osk_init(void *address)
{
    OskParams *params = psbridge_game_pointer(address);

    if (!params || params->field_count < 1 || params->field_count > 8 || !params->fields)
        return -1;

    if (!psbridge_game_pointer((void *)(uintptr_t)params->fields))
        return -1;

    params->dialog.result = 0;
    params->state = 1;
    osk_params_address = (u32)(uintptr_t)address;
    osk_status = DIALOG_INIT;

    PRINT_LOG("osk gozou inicio parametros %08lx campos %ld dados %08lx\n",
        (unsigned long)osk_params_address,
        (long)params->field_count,
        (unsigned long)params->fields);
    return 0;
}

static int osk_update(void)
{
    OskParams *params = osk_params();
    OskData *fields;
    int index;

    if (!params || osk_status != DIALOG_VISIBLE)
        return 0;

    fields = psbridge_game_pointer((void *)(uintptr_t)params->fields);

    if(!fields)
        return -1;

    for (index = 0; index < params->field_count; index++) {
        static const u16 fallback[] = {'g', 'o', 'z', 'e', 'i'};
        u16 *input = psbridge_game_pointer((void *) (uintptr_t)fields[index].input);
        u16 *output = psbridge_game_pointer((void *) (uintptr_t)fields[index].output);
        u32 capacity = fields[index].output_length;
        u32 limit;
        u32 length = 0;

        if (!output || !capacity)
            return -1;

        limit = capacity - 1;
        if (fields[index].output_limit && fields[index].output_limit < limit)
            limit = fields[index].output_limit;

        if (input) {
            while (length < limit && input[length]) {
                output[length] = input[length];
                length++;
            }
        }

        if (!length) {
            u32 fallback_length = sizeof(fallback) / sizeof(fallback[0]);

            while (length < limit && length < fallback_length) {
                output[length] = fallback[length];
                length++;
            }

            fields[index].result = 2;
        } else {
            fields[index].result = 0;
        }

        output[length] = 0;
    }

    params->state = 4;
    osk_status = DIALOG_QUIT;
    return 0;
}

static int osk_get_status(void)
{
    OskParams *params = osk_params();
    int current = osk_status;

    if (osk_status == DIALOG_INIT) {
        osk_status = DIALOG_VISIBLE;

        if (params)
            params->state = 3;
        } else if (osk_status == DIALOG_FINISHED) {
            osk_status = DIALOG_NONE;
            osk_params_address = 0;

            if (params)
                params->state = 0;
        }

    return current;
}

static int osk_shutdown(void)
{
    OskParams *params = osk_params();

    if (params)
        params->state = 5;
    osk_status = DIALOG_FINISHED;

    PRINT_LOG("osk terminou de gozar e liberou pra nós\n");
    return 0;
}

static int next_status(int *status)
{
    int current = *status;

    if (current == DIALOG_INIT)
        *status = DIALOG_VISIBLE;
    else if (current == DIALOG_FINISHED)
        *status = DIALOG_NONE;

    return current;
}

static int savedata_init(void *address)
{
    SavedataParams *params = psbridge_game_pointer(address);

    if (!params)
        return -1;

    params->dialog.result = 0;

    if (
        params->mode == 0 ||
        params->mode == 2 ||
        params->mode == 4 ||
        params->mode == 15 ||
        params->mode == 16
    )
        params->dialog.result = SAVEDATA_LOAD_NO_DATA;

    savedata_status = DIALOG_INIT;
    return 0;
}

static int message_init(void *address)
{
    MessageParams *params = psbridge_game_pointer(address);

    if (!params)
        return -1;

    params->dialog.result = 0;
    params->button = 1;
    message_status = DIALOG_INIT;
    return 0;
}

extern inline __attribute__((always_inline)) int psbridge_utility(u32 nid, void *argument)
{
    if (nid == 0x50c4cd57)
        return savedata_init(argument);

    if (nid == 0x4b85c861)
        return osk_update();

    if (nid == 0xf6269b82)
        return osk_init(argument);

    if (nid == 0xf3f76017)
        return osk_get_status();

    if (nid == 0x3dfaeba9)
        return osk_shutdown();

    if (nid == 0xd4b95ffb) {
        if (savedata_status == DIALOG_VISIBLE)
            savedata_status = DIALOG_QUIT;
        return 0;
    }

    if (nid == 0x9790b33c) {
        savedata_status = DIALOG_FINISHED;
        return 0;
    }

    if (nid == 0x8874dbe0)
        return next_status(&savedata_status);

    if (nid == 0x2ad8e239)
        return message_init(argument);

    if (nid == 0x95fc253b) {
        if (message_status == DIALOG_VISIBLE)
            message_status = DIALOG_QUIT;
        return 0;
    }

    if (nid == 0x67af3428) {
        message_status = DIALOG_FINISHED;
        return 0;
    }

    if (nid == 0x9a1c91d7)
        return next_status(&message_status);

    if (nid == 0x4928bd96) {
        message_status = DIALOG_QUIT;
        return 0;
    }

    return 0;
}
