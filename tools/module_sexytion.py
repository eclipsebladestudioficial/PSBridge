def file_end(segments):
    return max(offset + file_size for offset, _, file_size, _, _, _ in segments)


def quote_path(path):
    return str(path).replace("\\", "\\\\").replace('"', '\\"')


def add_image(lines, path, data, segments, image_base):
    file_size = file_end(segments)

    lines.extend([
        '.section .guest_image,"aw",@progbits',
        ".balign 1048576",
        *([f".space {image_base & 0xfffff}"] if image_base & 0xfffff else []),
        ".globl guest_memory",
        ".globl guest_file_end",
        "guest_memory:",
        ".align 0",
    ])

    if file_size:
        lines.append(f'.incbin "{quote_path(path)}", 0, {file_size}')

    lines.extend([
        "guest_file_end:",
        '.section .guest_bss,"aw",@nobits',
        ".balign 4",
        ".globl guest_bss",
        ".globl guest_memory_end",
        "guest_bss:",
        *([f".space {len(data) - file_size}"] if len(data) > file_size else []),
        "guest_memory_end:",
    ])

    return file_size


def add_map(lines, addresses, label):
    addresses = sorted(addresses)
    lines.extend([
        '.section .rodata.guest_map,"a",@progbits',
        ".balign 4",
        ".globl guest_map_count",
        ".globl guest_map",
        ".globl guest_map_end",
        "guest_map_count:",
        f".word {len(addresses)}",
        "guest_map:",
    ])

    for address in addresses:
        lines.append(f".word 0x{address:08x}, {label(address)}")

    lines.append("guest_map_end:")


def add_code_map(lines, addresses, image_base, label, single_instruction):
    blocks = {}
    for address in sorted(addresses):
        block, slot = divmod((address - image_base) // 4, 256)
        blocks.setdefault(block, {})[slot] = address

    lines.extend([
        '.section .rodata.guest_code_map,"a",@progbits',
        ".balign 4",
        ".globl guest_code_map",
        "guest_code_map:",
    ])

    for block in range(max(blocks, default=-1) + 1):
        entries = blocks.get(block)
        base = label(next(iter(entries.values()))) if entries else "0"
        lines.append(f".word {base}, guest_code_block_{block}")
    lines.extend([".globl guest_code_map_end", "guest_code_map_end:"])
    lines.append(f".word 0, guest_code_block_{max(blocks, default=-1) + 1}")

    for block in range(max(blocks, default=-1) + 1):
        lines.append(f"guest_code_block_{block}:")
        entries = blocks.get(block)
        if not entries:
            continue

        base = label(next(iter(entries.values())))
        previous = None
        for slot in range(256):
            address = entries.get(slot)
            if address is None:
                if previous is not None:
                    lines.append(f".word 0x{slot:02x}ffffff")
            elif previous is None or previous not in single_instruction:
                lines.append(f".word (0x{slot:02x}000000 | ({label(address)} - {base}))")
            previous = address
    lines.append(f"guest_code_block_{max(blocks, default=-1) + 1}:")


def add_overlay_map(lines, entries):
    lines.extend([
        '.section .rodata.overlay_map,"a",@progbits',
        ".balign 4",
        ".globl overlay_map_count",
        ".globl overlay_map",
        "overlay_map_count:",
        f".word {len(entries)}",
        "overlay_map:",
    ])

    for address, opcode, target in entries:
        lines.append(f".word 0x{address:08x}, 0x{opcode:08x}, {target}")


def add_info(lines, base, data_size, file_size, gp):
    lines.extend([
        '.section .rodata.guest_info,"a",@progbits',
        ".balign 4",
        ".globl guest_info",
        "guest_info:",
        ".word guest_memory",
        f".word 0x{base:08x}",
        f".word {data_size}",
        f".word {file_size}",
        f".word 0x{gp:08x}",
    ])


def write_map(path, addresses, label):
    text = "".join(
        f"0x{address:08x} {label(address)}\n"
        for address in sorted(addresses)
    )
    path.write_text(text)
