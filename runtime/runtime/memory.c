#include "psbridge_log.h"
#include <stdint.h>
#include <kernel.h>
#include <string.h>
#include <stdio.h>

#include <psbridge.h>

extern unsigned char __psbridge_heap_start[];
extern unsigned char __psbridge_heap_end[];

#define game_heap __psbridge_heap_start
#define HEAP_SIZE ((u32)((uintptr_t)__psbridge_heap_end - (uintptr_t)__psbridge_heap_start))
#define BLOCK_LIMIT 64
#define FPL_LIMIT 16
#define HEAP_ALLOCATION_LIMIT 24576
#define VPL_LIMIT 16

static int memory_guard = -1;

typedef struct {
    u32 start;
    u32 size;
} HeapAllocation;

typedef struct {
    int used;
    int uid;
    void *address;
    u32 size;
} MemoryBlock;

typedef struct {
    int used;
    int uid;
    void *address;
    u32 block_size;
    u32 block_count;
    u32 used_blocks;
} FixedPool;

typedef struct {
    int used;
    int uid;
    u32 size;
    u32 allocated;
    int count;
    u32 attributes;
    char name[32];
} VariablePool;

typedef struct {
    u32 size;
    char name[32];
    u32 attributes;
    s32 pool_size;
    s32 free_size;
    s32 waiting_threads;
} PspVplInfo;

static VariablePool variable_pools[VPL_LIMIT];

int psbridge_memory_init(void)
{
    ee_sema_t config;

    if (memory_guard >= 0)
        return 0;

    memset(&config, 0, sizeof(config));
    config.init_count = 1;
    config.max_count = 1;
    memory_guard = CreateSema(&config);
    if (memory_guard < 0)
        return memory_guard;
    return 0;
}

static int psbridge_memory_wait(void)
{
    return memory_guard >= 0 ? WaitSema(memory_guard) : -1;
}

static void psbridge_memory_signal(void)
{
    SignalSema(memory_guard);
}


static HeapAllocation heap_allocations[HEAP_ALLOCATION_LIMIT];
static MemoryBlock blocks[BLOCK_LIMIT];
static FixedPool pools[FPL_LIMIT];

static void * volatile_memory;
static int volatile_locked;

int psbridge_map_game_memory(void)
{
    const u32 page_size = 0x100000;
    const u32 pair_size = page_size * 2;
    const u32 page_mask = 0x001fe000;
    const u32 flags = 0x1f;
    u32 virtual_start;
    u32 physical_start;
    u32 mapped_size;
    u32 offset;

    virtual_start = guest_info.base & ~(pair_size - 1);
    physical_start = (u32)(uintptr_t)guest_info.memory & 0x1fffffff;
    physical_start -= guest_info.base - virtual_start;

    if ((physical_start & (page_size - 1)) != 0)
        return -1;

    mapped_size = guest_info.base - virtual_start + guest_info.memory_size;

    for (offset = 0; offset < mapped_size; offset += pair_size) {
        u32 physical = physical_start + offset;
        int result = _SetTLBEntry(
            31 + offset / pair_size,
            page_mask,
            virtual_start + offset,
            (physical >> 6) | flags,
            ((physical + page_size) >> 6) | flags
        );

        if (result < 0)
            return result;
    }

    for (offset = 0x00800000; offset < 0x02000000; offset += 0x00800000) {
        int result = _SetTLBEntry(
            39 + (offset - 0x00800000) / 0x00800000,
            0x007fe000,
            0x40000000 + offset,
            (offset >> 6) | 0x17,
            ((offset + 0x00400000) >> 6) | 0x17
        );
        if (result < 0)
            return result;
    }

    return 0;
}

void *psbridge_game_pointer(const void *pointer) 
{
    u32 address = (u32) (uintptr_t)pointer;

    if (!address)
        return NULL;

    if (
        address < guest_info.base ||
        address - guest_info.base >= guest_info.memory_size
    )
        return (void *) (uintptr_t) address;

    return (u8 *)guest_info.memory + address - guest_info.base;
    
}

static u32 heap_count;

static u32 align_up(u32 value, u32 alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static u32 align_down(u32 value, u32 alignment)
{
    return value & ~(alignment - 1);
}

static u32 heap_span(int index)
{
    return heap_allocations[index].size & ~63u;
}

static void *heap_take(u32 start, u32 size, int index, u32 owner)
{
    if (heap_count >= HEAP_ALLOCATION_LIMIT || owner > VPL_LIMIT || (size & 63u))
        return NULL;

    if (index < (int)heap_count)
        memmove(&heap_allocations[index + 1], &heap_allocations[index],
                (heap_count - index) * sizeof(heap_allocations[0]));
    heap_allocations[index].start = start;
    heap_allocations[index].size = size | owner;
    heap_count++;
    return game_heap + start;
}

static void *heap_alloc_tag(u32 size, u32 alignment, int high, u32 owner)
{
    u32 cursor;
    u32 position;
    u32 boundary;
    int index;

    if (alignment < 64)
        alignment = 64;

    if ((alignment & (alignment - 1)) || !size || alignment > HEAP_SIZE ||
        size > HEAP_SIZE || size > HEAP_SIZE - 63 ||
        heap_count >= HEAP_ALLOCATION_LIMIT)
        return NULL;

    size = align_up(size, 64);
    if (!high) {
        cursor = 0;
        for (index = 0; index <= (int)heap_count; index++) {
            boundary = index == (int)heap_count ? HEAP_SIZE : heap_allocations[index].start;
            position = align_up(cursor, alignment);
            if (position <= boundary && size <= boundary - position)
                return heap_take(position, size, index, owner);
            if (index < (int)heap_count)
                cursor = heap_allocations[index].start + heap_span(index);
        }
        return NULL;
    }

    cursor = HEAP_SIZE;
    for (index = (int)heap_count; index >= 0; index--) {
        boundary = index == 0 ? 0 :
            heap_allocations[index - 1].start + heap_span(index - 1);
        if (cursor >= size) {
            position = align_down(cursor - size, alignment);
            if (position >= boundary)
                return heap_take(position, size, index, owner);
        }
        if (index > 0)
            cursor = heap_allocations[index - 1].start;
    }
    return NULL;
}

static void *heap_alloc(u32 size, u32 alignment, int high)
{
    return heap_alloc_tag(size, alignment, high, 0);
}

static void *heap_alloc_at(void *requested, u32 size)
{
    uintptr_t address = (uintptr_t)requested;
    uintptr_t base = (uintptr_t)game_heap;
    u32 start;
    int low;
    int high;
    int index;

    if (address < base || address >= base + HEAP_SIZE || !size ||
        size > HEAP_SIZE - 63 || heap_count >= HEAP_ALLOCATION_LIMIT)
        return NULL;

    start = (u32)(address - base);
    size = align_up(size, 64);
    if (size > HEAP_SIZE - start)
        return NULL;

    low = 0;
    high = (int)heap_count;
    while (low < high) {
        int middle = low + (high - low) / 2;
        if (heap_allocations[middle].start < start)
            low = middle + 1;
        else
            high = middle;
    }
    index = low;
    if (index && heap_allocations[index - 1].start + heap_span(index - 1) > start)
        return NULL;
    if (index < (int)heap_count && start + size > heap_allocations[index].start)
        return NULL;
    return heap_take(start, size, index, 0);
}

static int heap_lookup(void *pointer)
{
    uintptr_t address = (uintptr_t)pointer;
    uintptr_t base = (uintptr_t)game_heap;
    u32 start;
    int low;
    int high;

    if (address < base || address >= base + HEAP_SIZE)
        return -1;

    start = (u32)(address - base);
    low = 0;
    high = (int)heap_count;
    while (low < high) {
        int middle = low + (high - low) / 2;
        if (heap_allocations[middle].start < start)
            low = middle + 1;
        else
            high = middle;
    }
    if (low >= (int)heap_count || heap_allocations[low].start != start)
        return -1;
    return low;
}

static void heap_drop(int index)
{
    if (index + 1 < (int)heap_count)
        memmove(&heap_allocations[index], &heap_allocations[index + 1],
                (heap_count - index - 1) * sizeof(heap_allocations[0]));
    heap_count--;
    memset(&heap_allocations[heap_count], 0, sizeof(heap_allocations[0]));
}

static int heap_free(void *pointer)
{
    int index = heap_lookup(pointer);
    if (index < 0)
        return -1;
    heap_drop(index);
    return 0;
}

static int psbridge_volatile_lock_raw(void *address, void *size)
{
    u32 *output_address = psbridge_game_pointer(address);
    u32 *output_size = psbridge_game_pointer(size);

    if (!output_address || !output_size || volatile_locked)
        return -1;

    if (!volatile_memory)
        volatile_memory = heap_alloc(0x400000, 64, 0);

    if (!volatile_memory)
        return -1;

    volatile_locked = 1;
    *output_address = (u32) (uintptr_t)volatile_memory;
    *output_size = 0x400000;

    return 0;

}

static int psbridge_volatile_unlock_raw(void)
{
    if (!volatile_locked)
        return -1;

    volatile_locked = 0;
    return 0;
}

static int psbridge_alloc_partition_raw(int partition, const char *name, int type, u32 size, void *address)
{
    int index;
    void *memory;
    u32 alignment = 64;

    (void)partition;

    for (index = 0; index < BLOCK_LIMIT; index++) {
        if (!blocks[index].used)
            break;
    }

    if (index == BLOCK_LIMIT)
        return -1;

    if (type == 2) {
        memory = heap_alloc_at(address, size);
    } else {
        if (type == 3 || type == 4)
            alignment = (u32)(uintptr_t)address;

        if (type < 0 || type > 4)
            return -1;

        memory = heap_alloc(size, alignment, type == 1 || type == 4);
    }

    if (!memory) {
        static int failures;
        if (failures++ < 12)
            PRINT_LOG("heap peidou alocacao particao %d tipo %d tamanho %lu alinhamento %lu livre %lu\n",
                partition, type, (unsigned long)size, (unsigned long)alignment,
                (unsigned long)HEAP_SIZE);
        return -1;
    }

    blocks[index].used = 1;
    blocks[index].uid = 0x400 + index;
    blocks[index].address = memory;
    blocks[index].size = size;

    return blocks[index].uid;
}

static void * psbridge_block_address_raw(int uid)
{
    int index;

    for (index = 0; index < BLOCK_LIMIT; index++) {
        if (blocks[index].used && blocks[index].uid == uid)
            return blocks[index].address;
    }

    return NULL;
}

static int psbridge_free_partition_raw(int uid)
{
    int index;

    for (index = 0; index < BLOCK_LIMIT; index++) {
        if (blocks[index].used && blocks[index].uid == uid) {
            if (heap_free(blocks[index].address) < 0)
                return -1;

            memset(&blocks[index], 0, sizeof(blocks[index]));
            return 0;
        }
    }

    return -1;
}

static int psbridge_create_fpl_raw(const char *name, int partition, u32 attributes, u32 block_size, u32 block_count, void *options)
{
    int index;
    void *memory;

    (void)name;
    (void)partition;
    (void)attributes;
    (void)options;

    if (!block_size || !block_count || block_count > 32)
        return -1;

    for (index = 0; index < FPL_LIMIT; index++) {
        if (!pools[index].used)
            break;
    }

    if (index == FPL_LIMIT)
        return -1;

    block_size = (block_size + 63) & ~63;
    memory = heap_alloc(block_size * block_count, 64, 0);

    if (!memory)
        return -1;

    pools[index].used = 1;
    pools[index].uid = 0x800 + index;
    pools[index].address = memory;
    pools[index].block_size = block_size;
    pools[index].block_count = block_count;
    pools[index].used_blocks = 0;
    return pools[index].uid;

}

static int psbridge_allocate_fpl_raw(int uid, void **output)
{
    int index;
    void **real_output = psbridge_game_pointer(output);

    if (!real_output) 
        return -1;

    for (index = 0; index < FPL_LIMIT; index++) {
        FixedPool *pool = &pools[index];
        u32 block;

        if (!pool->used || pool->uid != uid)
            continue;

        for (block = 0; block < pool->block_count; block++) {
            if (pool->used_blocks & (1u << block))
                continue;

            pool->used_blocks |= 1u << block;
            *real_output = (u8 *)pool->address + block * pool->block_size;
            return 0;
        }
    }
    return -1;
}


static int psbridge_delete_fpl_raw(int uid)
{
    int index;

    for (index = 0; index < FPL_LIMIT; index++) {
        if (pools[index].used && pools[index].uid == uid) {
            if (heap_free(pools[index].address) < 0)
                return -1;

            memset(&pools[index], 0, sizeof(pools[index]));
            return 0;
        }
    }

    return -1;
}

static int psbridge_create_vpl_raw(const char *name, int partition, u32 attributes, u32 size, void *options)
{
    int index;
    (void)name;
    (void)partition;
    (void)attributes;
    (void)options;

    if (!size || size > 0x02000000)
        return -1;

    for (index = 0; index < VPL_LIMIT; index++) {
        if (!variable_pools[index].used)
            break;
    }
    if (index == VPL_LIMIT)
        return -1;

    memset(&variable_pools[index], 0, sizeof(variable_pools[index]));
    variable_pools[index].used = 1;
    variable_pools[index].uid = 0x2000 + index;
    variable_pools[index].size = size;
    variable_pools[index].attributes = attributes;
    if (name) {
        const char *source = psbridge_game_pointer(name);
        int i;
        if (source) {
            for (i = 0; i < 31 && source[i]; i++)
                variable_pools[index].name[i] = source[i];
            variable_pools[index].name[i] = 0;
        }
    }
    {
        static int logs;
        if (logs++ < 12)
            PRINT_LOG("vpl nasceu na putaria uid %d tamanho %u memoria sob demanda\n",
                variable_pools[index].uid, size);
    }
    return variable_pools[index].uid;
}

static int psbridge_allocate_vpl_raw(int uid, u32 size, void *output, int try_only)
{
    int index;
    u8 *memory;
    u32 *result = psbridge_game_pointer(output);
    (void)try_only;

    if (!result || !size || size > 0xfffffff0u)
        return -1;

    size = align_up(size + 8, 8);

    for (index = 0; index < VPL_LIMIT; index++) {
        VariablePool *pool = &variable_pools[index];

        if (!pool->used || pool->uid != uid)
            continue;

        if (pool->allocated > pool->size || size > pool->size - pool->allocated) {
            static int logs;
            if (logs++ < 12)
                PRINT_LOG("vpl estourou limite uid %d pedido %u alocado %u teto %u\n", uid, size, pool->allocated, pool->size);
            return -1;
        }

        memory = heap_alloc_tag(size, 64, 0, (u32)(index + 1));
        if (!memory) {
            static int logs;
            u32 free_total = HEAP_SIZE;
            u32 biggest = 0;
            u32 cursor = 0;
            u32 n;

            for (n = 0; n < heap_count; n++) {
                u32 begin = heap_allocations[n].start;
                u32 span = heap_span(n);
                if (begin > cursor && begin - cursor > biggest)
                    biggest = begin - cursor;
                free_total -= span;
                cursor = begin + span;
            }
            if (HEAP_SIZE > cursor && HEAP_SIZE - cursor > biggest)
                biggest = HEAP_SIZE - cursor;
            if (logs++ < 16)
                PRINT_LOG("vpl peidou alocacao uid %d tamanho %u heap %u livre %u maior buraco %u blocos %u limite %u\n",
                    uid, size, HEAP_SIZE, free_total, biggest, heap_count, HEAP_ALLOCATION_LIMIT);
            return -1;
        }

        *(u32 *)memory = size;
        *(u32 *)(memory + 4) = (u32)(uintptr_t)(memory + 8);
        pool->count++;
        pool->allocated += size;
        *result = (u32)(uintptr_t)(memory + 8);
        if (size >= 0x800000u)
            PRINT_LOG("vpl gigante gozou uid %d tamanho %u endereco %08x heap %u\n",
                uid, size, *result, HEAP_SIZE);
        {
            static int logs;
            if (logs++ < 12)
                PRINT_LOG("vpl gozou uid %d tamanho %u endereco %08x\n", uid, size, *result);
        }
        return 0;
    }
    {
        static int logs;
        if (logs++ < 12)
            PRINT_LOG("vpl sumiu uid %d tamanho %u saida %08x\n", uid, size, (u32)(uintptr_t)output);
    }
    return -1;
}

static int psbridge_reject_vpl_free(int uid, u32 address, u32 actual_uid, int kind)
{
    (void)uid;
    (void)actual_uid;
    return kind == 2 && (address >= 0xf0000000u || address < 0x00010000u) ?
        (int)0x800200d3u : (int)0x800201b6u;
}

static int psbridge_free_vpl_raw(int uid, void *address)
{
    int index;
    uintptr_t pointer = (uintptr_t)address;

    for (index = 0; index < VPL_LIMIT; index++) {
        VariablePool *pool = &variable_pools[index];
        int entry;
        u32 stored;
        u32 charged;
        u32 owner;

        if (!pool->used || pool->uid != uid)
            continue;

        if (pointer < (uintptr_t)game_heap + 8 ||
            pointer >= (uintptr_t)game_heap + HEAP_SIZE + 8)
            return psbridge_reject_vpl_free(uid, (u32)pointer, 0, 2);

        entry = heap_lookup((void *)(pointer - 8));
        if (entry < 0)
            return psbridge_reject_vpl_free(uid, (u32)pointer, 0, 2);

        owner = heap_allocations[entry].size & 63u;
        if (owner != (u32)(index + 1)) {
            u32 actual_uid = owner > 0 && owner <= VPL_LIMIT &&
                variable_pools[owner - 1].used ?
                (u32)variable_pools[owner - 1].uid : 0;
            return psbridge_reject_vpl_free(uid, (u32)pointer, actual_uid, 1);
        }

        stored = heap_span(entry);
        charged = *(u32 *)(pointer - 8);
        if (charged < 8 || charged > stored || (charged & 7u))
            return psbridge_reject_vpl_free(uid, (u32)pointer, (u32)uid, 3);

        heap_drop(entry);
        if (pool->allocated >= charged)
            pool->allocated -= charged;
        else
            pool->allocated = 0;
        if (pool->count > 0)
            pool->count--;
        return 0;
    }
    return (int)0x8002019cu;
}

static int psbridge_refer_vpl_status_raw(int uid, void *output)
{
    PspVplInfo *info = psbridge_game_pointer(output);
    int index;

    if (!info)
        return (int)0x800200d3u;

    for (index = 0; index < VPL_LIMIT; index++) {
        VariablePool *pool = &variable_pools[index];
        PspVplInfo status;
        u32 length;

        if (!pool->used || pool->uid != uid)
            continue;

        memset(&status, 0, sizeof(status));
        status.size = sizeof(status);
        memcpy(status.name, pool->name, sizeof(status.name));
        status.attributes = pool->attributes;
        status.pool_size = (s32)pool->size;
        status.free_size = (s32)(pool->size >= pool->allocated ? pool->size - pool->allocated : 0);
        status.waiting_threads = 0;
        length = info->size;
        if (length < sizeof(u32) || length > sizeof(status)) {
            static u32 abnormal_sizes;
            if (++abnormal_sizes <= 8 || (abnormal_sizes & 1023u) == 0)
                PRINT_LOG("vpl leu tamanho doido uid %d tamanho %u e entregou estado inteiro vezes %u\n",
                    uid, length, abnormal_sizes);
            length = sizeof(status);
        }
        memcpy(info, &status, length);

        {
            static u32 logs;
            if (logs++ < 20 || (logs & 255u) == 0)
                PRINT_LOG("vpl abriu o livro uid %d total %u livre %u lotes %d\n",
                    uid, pool->size, (u32)status.free_size, pool->count);
        }
        return 0;
    }

    {
        static int logs;
        if (logs++ < 12)
            PRINT_LOG("vpl consultou um fantasma uid %d\n", uid);
    }
    return -1;
}

static int psbridge_delete_vpl_raw(int uid)
{
    int index;

    for (index = 0; index < VPL_LIMIT; index++) {
        VariablePool *pool = &variable_pools[index];
        int n;

        if (!pool->used || pool->uid != uid)
            continue;

        for (n = (int)heap_count - 1; n >= 0; n--) {
            if ((heap_allocations[n].size & 63u) == (u32)(index + 1))
                heap_drop(n);
        }
        memset(pool, 0, sizeof(*pool));
        return 0;
    }
    return -1;
}


int psbridge_volatile_lock(void *address, void *size)
{
    int result;
    if (psbridge_memory_wait() < 0)
        return -1;
    result = psbridge_volatile_lock_raw(address, size);
    psbridge_memory_signal();
    return result;
}

int psbridge_volatile_unlock(void)
{
    int result;
    if (psbridge_memory_wait() < 0)
        return -1;
    result = psbridge_volatile_unlock_raw();
    psbridge_memory_signal();
    return result;
}

int psbridge_alloc_partition(int partition, const char *name, int type, u32 size, void *address)
{
    int result;
    if (psbridge_memory_wait() < 0)
        return -1;
    result = psbridge_alloc_partition_raw(partition, name, type, size, address);
    psbridge_memory_signal();
    return result;
}

void * psbridge_block_address(int uid)
{
    void * result;
    if (psbridge_memory_wait() < 0)
        return NULL;
    result = psbridge_block_address_raw(uid);
    psbridge_memory_signal();
    return result;
}

int psbridge_free_partition(int uid)
{
    int result;
    if (psbridge_memory_wait() < 0)
        return -1;
    result = psbridge_free_partition_raw(uid);
    psbridge_memory_signal();
    return result;
}

int psbridge_create_fpl(const char *name, int partition, u32 attributes, u32 block_size, u32 block_count, void *options)
{
    int result;
    if (psbridge_memory_wait() < 0)
        return -1;
    result = psbridge_create_fpl_raw(name, partition, attributes, block_size, block_count, options);
    psbridge_memory_signal();
    return result;
}

int psbridge_allocate_fpl(int uid, void **output)
{
    int result;
    if (psbridge_memory_wait() < 0)
        return -1;
    result = psbridge_allocate_fpl_raw(uid, output);
    psbridge_memory_signal();
    return result;
}

int psbridge_delete_fpl(int uid)
{
    int result;
    if (psbridge_memory_wait() < 0)
        return -1;
    result = psbridge_delete_fpl_raw(uid);
    psbridge_memory_signal();
    return result;
}

int psbridge_create_vpl(const char *name, int partition, u32 attributes, u32 size, void *options)
{
    int result;
    if (psbridge_memory_wait() < 0)
        return -1;
    result = psbridge_create_vpl_raw(name, partition, attributes, size, options);
    psbridge_memory_signal();
    return result;
}

int psbridge_allocate_vpl(int uid, u32 size, void *output, int try_only)
{
    int result;
    if (psbridge_memory_wait() < 0)
        return -1;
    result = psbridge_allocate_vpl_raw(uid, size, output, try_only);
    psbridge_memory_signal();
    return result;
}

int psbridge_free_vpl(int uid, void *address)
{
    int result;
    if (psbridge_memory_wait() < 0)
        return -1;
    result = psbridge_free_vpl_raw(uid, address);
    psbridge_memory_signal();
    return result;
}

int psbridge_refer_vpl_status(int uid, void *output)
{
    int result;
    if (psbridge_memory_wait() < 0)
        return -1;
    result = psbridge_refer_vpl_status_raw(uid, output);
    psbridge_memory_signal();
    return result;
}

int psbridge_delete_vpl(int uid)
{
    int result;
    if (psbridge_memory_wait() < 0)
        return -1;
    result = psbridge_delete_vpl_raw(uid);
    psbridge_memory_signal();
    return result;
}
