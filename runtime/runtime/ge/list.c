#include <string.h>

#include "ge_internal.h"

static int next_list_id = 1;
static int next_callback_id = 1;
static GeCallback ge_callbacks[16];
static u32 diag_ge_callbacks;

GeCallback *find_ge_callback(int id)
{
    int i;
    for (i = 0; i < 16; i++) {
        if (ge_callbacks[i].used && ge_callbacks[i].id == id)
            return &ge_callbacks[i];
    }
    return NULL;
}

int register_ge_callback(u32 address)
{
    const u32 *data = psbridge_game_pointer((const void *)(uintptr_t)address);
    int i;
    if (!data)
        return -1;
    for (i = 0; i < 16; i++) {
        if (!ge_callbacks[i].used) {
            GeCallback *cb = &ge_callbacks[i];
            cb->used = 1;
            cb->id = next_callback_id++;
            cb->signal_function = data[0];
            cb->signal_arg = data[1];
            cb->finish_function = data[2];
            cb->finish_arg = data[3];
            PRINT_LOG("ge cadastrou callback safada id %d sinal %08x fim %08x\n",
                cb->id, cb->signal_function, cb->finish_function);
            return cb->id;
        }
    }
    return -1;
}

static void fire_ge_callback(int callback_id, int finish, u32 data)
{
    GeCallback *cb = find_ge_callback(callback_id);
    u32 function;
    u32 argument;
    if (!cb)
        return;
    function = finish ? cb->finish_function : cb->signal_function;
    argument = finish ? cb->finish_arg : cb->signal_arg;
    if (!function)
        return;
    diag_ge_callbacks++;
    if (diag_ge_callbacks <= 16 || (diag_ge_callbacks % 128 == 0))
        PRINT_LOG("ge callback %s gozou id %d dado %08x endereco %08x vezes %u\n",
            finish ? "fim" : "sinal", callback_id, data, function, diag_ge_callbacks);
    psbridge_call_callback((void *)(uintptr_t)function, data, argument, 0);
}




void *ge_pointer(u32 address)
{
    u32 video = (u32)(uintptr_t)psbridge_video_memory();

    address &= 0x3fffffff;

    if (address >= 0x04000000 && address < 0x04200000)
        return (u8 *)psbridge_video_memory() + address - 0x04000000;

    if (address < 0x00200000)
        return (u8 *)psbridge_video_memory() + address;

    if (address >= video && address < video + 0x00200000)
        return (void *)(uintptr_t)address;

    {
        void *result = psbridge_game_pointer((void *)(uintptr_t)address);
        if ((u32)(uintptr_t)result == address && address >= 0x00200000 && address < 0x02000000)
            return (void *)(uintptr_t)(address | 0x80000000);
        return result;
    }
}

u32 relative_address(u32 value)
{
    u32 base = (ge_state.command[GE_BASE] & 0x000f0000) << 8;

    return (ge_state.offset_address + base + (value & 0x00ffffff)) & 0x0fffffff;
}

u32 buffer_address(int address_command, int width_command)
{
    return
        (ge_state.command[address_command] & 0x00fffff0) |
        ((ge_state.command[width_command] << 8) & 0x0f000000);
}

static void transfer_pixels(void)
{
    int source_width = ge_state.command[GE_TRANSFERSRCW] & 0x7f8;
    int destination_width = ge_state.command[GE_TRANSFERDSTW] & 0x7f8;
    int source_x = ge_state.command[GE_TRANSFERSRCPOS] & 0x3ff;
    int source_y = (ge_state.command[GE_TRANSFERSRCPOS] >> 10) & 0x3ff;
    int destination_x = ge_state.command[GE_TRANSFERDSTPOS] & 0x3ff;
    int destination_y = (ge_state.command[GE_TRANSFERDSTPOS] >> 10) & 0x3ff;
    int width = (ge_state.command[GE_TRANSFERSIZE] & 0x3ff) + 1;
    int height = ((ge_state.command[GE_TRANSFERSIZE] >> 10) & 0x3ff) + 1;
    int bytes = (ge_state.command[GE_TRANSFERSTART] & 1) ? 4 : 2;
    const u8 *source = ge_pointer(buffer_address(GE_TRANSFERSRC, GE_TRANSFERSRCW));
    u8 *destination = ge_pointer(buffer_address(GE_TRANSFERDST, GE_TRANSFERDSTW));
    int y;

    if (!source || !destination || !source_width || !destination_width)
        return;

    for (y = 0; y < height; y++) {
        memcpy(
            destination + ((destination_y + y) * destination_width + destination_x) * bytes,
            source + ((source_y + y) * source_width + source_x) * bytes,
            width * bytes
        );
    }
}

GeList *find_list(int id)
{
    int index;

    for (index = 0; index < GE_LIST_LIMIT; index++) {
        if (ge_lists[index].used && ge_lists[index].id == id)
            return &ge_lists[index];
    }

    return NULL;
}

void execute_list(GeList *list)
{
    int commands = 0;

    while (!list->done && commands++ < GE_COMMAND_LIMIT) {
        const u32 *instruction;
        u32 word;
        u32 command;
        u32 argument;

        if (list->stall && list->pc == list->stall)
            break;

        if (!list->pc || (list->pc & 3)) {
            list->done = 1;
            break;
        }

        instruction = ge_pointer(list->pc);

        if (!instruction) {
            list->done = 1;
            break;
        }

        word = *instruction;
        command = word >> 24;
        argument = word & 0x00ffffff;
        list->pc += 4;
        ge_state.command[command] = argument;
        if (command == GE_BLENDMODE)
            ge_blend_mode_seen = 1;
        if (command >= GE_WORLDMATRIXNUMBER && command <= GE_PROJMATRIXDATA)
            ge3d_matrix_command(command, argument);

        if (command == GE_TEXFUNC || command == GE_VERTEXTYPE || command == GE_TEXFORMAT) {

            static int ge_rgb01_logs = 0;

            if (ge_rgb01_logs < 18) {
                ge_rgb01_logs++;

                PSB_LOG(
                    GE,
                    "ge recebeu uma leitada %08x com a potencia de mil homens %06x",
                    (unsigned int)command,
                    (unsigned int)argument
                );
            }

        }


        if (command == GE_VADDR)
            ge_state.vertex_address = relative_address(argument);
        else if (command == GE_IADDR)
            ge_state.index_address = relative_address(argument);
        else if (command == GE_OFFSETADDR)
            ge_state.offset_address = argument << 8;
        else if (command == GE_ORIGIN)
            ge_state.offset_address = list->pc - 4;
        else if (command == GE_PRIM) {
            list->draws++;
            draw_primitive(argument);
        } else if (command == GE_TRANSFERSTART) {
            transfer_pixels();
        }
        else if (command == GE_LOADCLUT)
            load_clut(argument);
        else if (command == GE_JUMP)
            list->pc = relative_address(argument & 0x00fffffc);
        else if (command == GE_CALL && list->stack_size < GE_STACK_LIMIT) {
            list->stack[list->stack_size] = list->pc;
            list->stack_offset[list->stack_size] = ge_state.offset_address;
            list->stack_size++;
            list->pc = relative_address(argument & 0x00fffffc);
        } else if (command == GE_RET && list->stack_size > 0) {
            list->stack_size--;
            list->pc = list->stack[list->stack_size];
            ge_state.offset_address = list->stack_offset[list->stack_size];
        } else if (command == GE_SIGNAL) {
            list->signal_id = argument & 0xffff;
            list->pending_signal = 1;
        } else if (command == GE_FINISH) {
            list->finish_id = argument;
            list->pending_finish = 1;
        } else if (command == GE_END) {
            list->done = 1;
            if (list->draws) {
                u32 target = buffer_address(GE_VADDR_BASE, GE_VADDR_WIDTH);
                if (target <= 0x200000u - 0x88000u && (ge_state.command[GE_FRAMEBUF_FORMAT] & 3u) == 3u) {
                    ge_ready_buffer = target;
                    ge_ready_sequence++;
                }
            }
            if (list->pending_signal) {
                u32 signal = list->signal_id;
                list->pending_signal = 0;
                fire_ge_callback(list->callback_id, 0, signal);
            }
            if (list->pending_finish) {
                u32 finish = list->finish_id;
                list->pending_finish = 0;
                fire_ge_callback(list->callback_id, 1, finish);
            }
        }
    }
    if (commands >= GE_COMMAND_LIMIT) {
        list->done = 1;
    }
}

int enqueue_list(u32 address, u32 stall, int callback_id)
{
    GeList *list = NULL;
    int index;

    address &= 0x3fffffff;
    stall &= 0x3fffffff;

    if (!address || (address & 3) || (stall & 3))
        return -1;

    for (index = 0; index < GE_LIST_LIMIT; index++) {
        if (!ge_lists[index].used || ge_lists[index].done) {
            list = &ge_lists[index];
            break;
        }
    }

    if (!list) {
        return -1;
    }
    memset(list, 0, sizeof(*list));
    list->used = 1;
    list->id = next_list_id++;
    list->pc = address;
    list->stall = stall;
    list->callback_id = callback_id;
    execute_list(list);
    return list->id;
}

