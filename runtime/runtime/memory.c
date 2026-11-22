#include "psbridge_log.h"
#include <stdint.h>
#include <kernel.h>
#include <string.h>
#include <stdio.h>

#include <psbridge.h>

extern unsigned char __psbridge_heap_start[];
extern unsigned char __psbridge_heap_end[];
extern unsigned char __psbridge_stack_start[];
extern unsigned char __psbridge_stack_end[];

#define game_heap __psbridge_heap_start
#define HEAP_SIZE ((u32)((uintptr_t)__psbridge_heap_end - (uintptr_t)game_heap))
#define PSP_USER_RAM_START 0x08800000u
#define PSP_USER_RAM_END   0x0a000000u
#define PSP_SCRATCH_START  0x00010000u
#define PSP_SCRATCH_SIZE   0x4000u
#define PSP_UNCACHED       0x40000000u
#define EE_RAM_MASK        0x1fffffffu
#define TLB_PAGE_1M        0x100000u
#define TLB_PAGE_4M        0x400000u
#define TLB_PAGE_16M       0x1000000u
#define TLB_FIRST_MIRROR   22
#define TLB_SDK_MIRROR_END 39
#define TLB_ENTRY_COUNT    48
#define EE_UNCACHED_MIRROR       0x20000000u
#define EE_ACCELERATED_MIRROR    0x30000000u
#define EE_GUEST_UNCACHED_MIRROR 0x40000000u
#define BLOCK_LIMIT 64
#define FPL_LIMIT 16
#define HEAP_ALLOCATION_LIMIT 24576
#define VPL_LIMIT 16
#define STACK_PAGE_SIZE 4096u

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


static HeapAllocation heap_allocations[HEAP_ALLOCATION_LIMIT]
    __attribute__((section(".runtime_scratch")));
static MemoryBlock blocks[BLOCK_LIMIT];
static FixedPool pools[FPL_LIMIT];
static u16 stack_pages[TLB_PAGE_1M / STACK_PAGE_SIZE];

static void * volatile_memory;
static int volatile_locked;

static u32 psp_heap_base;
static u32 psp_heap_physical;
static u32 psp_heap_size;
static u8 psp_scratchpad[PSP_SCRATCH_SIZE]
    __attribute__((aligned(PSP_SCRATCH_SIZE)));

typedef struct {
    u32 address;
    u32 mask;
    u32 even;
    u32 odd;
} RamTlbEntry;

typedef struct {
    int count;
    RamTlbEntry entries[TLB_ENTRY_COUNT - TLB_FIRST_MIRROR];
} RamTlb;

static int add_ram_entry(RamTlb *tlb, u32 address, u32 mask, u32 even, u32 odd)
{
    int i;

    for (i = 0; i < tlb->count; i++) {
        RamTlbEntry *entry = &tlb->entries[i];

        if (entry->address == address && entry->mask == mask) {
            if ((entry->even != 1 && even != 1) ||
                (entry->odd != 1 && odd != 1))
                return -1;
            entry->even |= even;
            entry->odd |= odd;
            return 0;
        }
    }

    if (tlb->count == TLB_ENTRY_COUNT - TLB_FIRST_MIRROR)
        return -1;

    tlb->entries[tlb->count++] = (RamTlbEntry){address, mask, even, odd};
    return 0;
}

static int map_ram_range(RamTlb *tlb, u32 address, u32 physical, u32 size,
                         u32 flags)
{
    while (size) {
        u32 page = TLB_PAGE_1M;
        u32 even, odd, bytes;
        int result;

        if (!((address | physical) & (TLB_PAGE_4M - 1)) &&
            !(address & TLB_PAGE_4M) &&
            (size == TLB_PAGE_4M || size >= 2 * TLB_PAGE_4M))
            page = TLB_PAGE_4M;

        if (address & page) {
            even = 1;
            odd = (physical >> 6) | flags;
            bytes = page;
        } else {
            even = (physical >> 6) | flags;
            odd = size >= 2 * page ? ((physical + page) >> 6) | flags : 1;
            bytes = size >= 2 * page ? 2 * page : page;
        }

        result = add_ram_entry(tlb, address & ~(page * 2 - 1),
                               (page - 0x1000) << 1, even, odd);
        if (result < 0)
            return result;

        address += bytes;
        physical += bytes;
        size -= bytes;
    }

    return 0;
}

static int map_psp_ram(RamTlb *tlb, u32 address, u32 physical, u32 size)
{
    int result = map_ram_range(tlb, address, physical, size, 0x1f);

    if (result < 0)
        return result;

    return map_ram_range(tlb, address | PSP_UNCACHED, physical, size, 0x17);
}

static int map_ee_mirror(RamTlb *tlb, u32 address, u32 flags)
{
    return add_ram_entry(tlb, address, (TLB_PAGE_16M - 0x1000) << 1,
                         flags, (TLB_PAGE_16M >> 6) | flags);
}

int psbridge_map_game_memory(void)
{
    const u32 page_mask = TLB_PAGE_1M - 1;
    u32 image_start = guest_info.base & ~page_mask;
    u32 image_end = (guest_info.base + guest_info.memory_size + page_mask) & ~page_mask;
    u32 image_physical = ((u32)(uintptr_t)guest_info.memory & EE_RAM_MASK)
        - (guest_info.base - image_start);
    u32 heap_end = (u32)(uintptr_t)__psbridge_heap_end & EE_RAM_MASK & ~page_mask;
    RamTlb tlb = {0};
    int result, i;

    if (image_physical & page_mask)
        return -1;

    result = map_ee_mirror(&tlb, EE_UNCACHED_MIRROR, 0x17);
    if (result < 0)
        return result;
    result = map_ee_mirror(&tlb, EE_ACCELERATED_MIRROR, 0x3f);
    if (result < 0)
        return result;
    result = map_ee_mirror(&tlb, EE_GUEST_UNCACHED_MIRROR, 0x17);
    if (result < 0)
        return result;

    result = add_ram_entry(&tlb, PSP_SCRATCH_START, (PSP_SCRATCH_SIZE - 0x1000) << 1,
                           (((u32)(uintptr_t)psp_scratchpad & EE_RAM_MASK) >> 6) | 0x1f, 1);
    if (result < 0)
        return result;

    result = map_psp_ram(&tlb, image_start, image_physical, image_end - image_start);
    if (result < 0)
        return result;

    psp_heap_base = PSP_USER_RAM_START;
    if (image_start < PSP_USER_RAM_END && image_end > PSP_USER_RAM_START)
        psp_heap_base = image_end;
    psp_heap_physical = (((u32)(uintptr_t)game_heap & EE_RAM_MASK) + page_mask) & ~page_mask;
    psp_heap_physical += (psp_heap_base - psp_heap_physical) & (TLB_PAGE_4M - 1);

    if (psp_heap_base < PSP_USER_RAM_END && psp_heap_physical < heap_end) {
        psp_heap_size = heap_end - psp_heap_physical;
        if (psp_heap_size > PSP_USER_RAM_END - psp_heap_base)
            psp_heap_size = PSP_USER_RAM_END - psp_heap_base;

        result = map_psp_ram(&tlb, psp_heap_base, psp_heap_physical, psp_heap_size);
        if (result < 0)
            return result;
    }

    for (i = TLB_FIRST_MIRROR + tlb.count; i < TLB_SDK_MIRROR_END; i++) {
        result = _SetTLBEntry(i, 0, 0xe0000000u + i * 0x2000u, 0, 0);
        if (result < 0)
            return result;
    }
    for (i = tlb.count - 1; i >= 0; i--) {
        const RamTlbEntry *entry = &tlb.entries[i];

        result = _SetTLBEntry(TLB_FIRST_MIRROR + i, entry->mask, entry->address,
                              entry->even, entry->odd);
        if (result < 0)
            return result;
    }
    return 0;
}

void *psbridge_ram_pointer(u32 address, u32 size)
{
    u32 uncached = address & PSP_UNCACHED;
    u32 canonical = address & ~PSP_UNCACHED;
    u32 offset = canonical - guest_info.base;
    u32 physical;

    if (address - PSP_SCRATCH_START < PSP_SCRATCH_SIZE &&
        size <= PSP_SCRATCH_SIZE - (address - PSP_SCRATCH_START))
        return psp_scratchpad + (address - PSP_SCRATCH_START);

    if (offset < guest_info.memory_size && size <= guest_info.memory_size - offset) {
        physical = ((u32)(uintptr_t)guest_info.memory & EE_RAM_MASK) + offset;
    } else {
        offset = canonical - psp_heap_base;
        if (offset >= psp_heap_size || size > psp_heap_size - offset)
            return NULL;
        physical = psp_heap_physical + offset;
    }

    return (void *)(uintptr_t)(physical | (uncached ? 0xa0000000 : 0x80000000));
}

void *psbridge_game_pointer(const void *pointer)
{
    u32 address = (u32)(uintptr_t)pointer;
    void *ram = psbridge_ram_pointer(address, 1);

    return ram ? ram : (void *)pointer;
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
    uintptr_t address = (uintptr_t)psbridge_game_pointer(requested);
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

void *psbridge_resize_thread_stack(void *old_stack, u32 size)
{
    uintptr_t start = (uintptr_t)__psbridge_stack_start;
    uintptr_t end = (uintptr_t)__psbridge_stack_end;
    uintptr_t old = (uintptr_t)old_stack;
    u32 pages = (size + STACK_PAGE_SIZE - 1) / STACK_PAGE_SIZE;
    u32 capacity = (end - start) / STACK_PAGE_SIZE;
    u32 first = 0;
    void *stack = NULL;

    if (psbridge_memory_wait() < 0)
        return NULL;

    for (u32 page = 0; page < capacity && pages <= capacity; page++) {
        if (stack_pages[page]) {
            page += stack_pages[page] - 1;
            first = page + 1;
        } else if (page - first + 1 == pages) {
            stack_pages[first] = pages;
            stack = (void *)(start + first * STACK_PAGE_SIZE);
            break;
        }
    }

    if (!stack)
        stack = heap_alloc(size, 64, 1);
    if (stack && old_stack) {
        if (old >= start && old < end)
            stack_pages[(old - start) / STACK_PAGE_SIZE] = 0;
        else
            heap_free(old_stack);
    }
    psbridge_memory_signal();
    return stack;
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
