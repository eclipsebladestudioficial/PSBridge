# EU VOU DEIXAR OS COMENTARIOS EM PORTUGUES PORQUE EU SOU 
# BRASILEIRO NESSE CARALHO ENTÃO VAI SE FUDER

import struct
import sys
import mmap
from pathlib import Path

from allegrex import branch_names, vfpu_branches, decode, target_of
from analyze import find_callbacks, find_jump_tables, output_register
from elf_reader import read_code, read_header, read_segments, read_word
from module_sexytion import add_code_map, add_image, add_info, add_map, add_overlay_map, write_map
from imports import find_module, read_imports, import_symbol, link_imports
from r5900 import emit, resolve_indirect_jump
from relocate import PSP_USER_LOAD_ADDRESS, relocate


def read_game_code(elf, data, segments, absolute=False):
    base = 0 if absolute else min(vaddr - offset for offset, vaddr, *_ in segments)
    code = {}
    names = set()

    for raw_address, _ in read_code(elf):
        address = base + raw_address
        instruction = decode(address, read_word(data, segments, address))
        if instruction[0] == "word":
            raise ValueError(f"que porra de instrução é essa em 0x{address:08x}")
        code[address] = instruction
        names.add(instruction[0])

    return code, names, base


def read_embedded_elf(path, position):
    with path.open("rb") as file:
        file.seek(position)
        header = file.read(52)

        if len(header) < 52:
            return None

        section_offset = struct.unpack_from("<I", header, 32)[0]
        section_size, section_count = struct.unpack_from("<HH", header, 46)
        _, _, program_offset, program_size, program_count = read_header(
            header + b"\0" * max(0, 32 * struct.unpack_from("<H", header, 44)[0])
        )
        table_end = max(
            52,
            program_offset + program_size * program_count,
            section_offset + section_size * section_count,
        )
        file.seek(position)
        tables = file.read(table_end)

        if len(tables) < table_end:
            return None

        elf_type, entry, program_offset, program_size, program_count = read_header(tables)
        segments = read_segments(tables, program_offset, program_size, program_count)
        size = max(
            table_end,
            *(offset + file_size for offset, _, file_size, _, _, _ in segments),
        )
        file.seek(position)
        data = file.read(size)

    if len(data) != size:
        return None

    return data, elf_type, entry, segments


def find_overlays(root, image_base, image_size, code_limit=0x14000):
    asset_root = root / "build" / "PSP_GAME"

    if not asset_root.is_dir():
        return None

    paths = sorted(
        (path for path in asset_root.rglob("*") if path.is_file()),
        key=lambda path: ("USRDIR" not in path.parts, str(path)),
    )
    image_end = image_base + image_size

    overlays = []
    code_size = 0

    for path in paths:
        try:
            with path.open("rb") as file:
                with mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as view:
                    position = view.find(b"\x7fELF", 1)

                    while position >= 0:
                        try:
                            found = read_embedded_elf(path, position)
                        except (ValueError, struct.error):
                            found = None

                        if found:
                            data, elf_type, entry, segments = found
                            loaded = [segment for segment in segments if segment[4] & 1]

                            if (
                                elf_type in (2, 0xffa0, 0xffa8)
                                and loaded
                                and all(
                                    image_base <= address
                                    and address + memory_size <= image_end
                                    for _, address, _, memory_size, _, _ in loaded
                                )
                            ):
                                size = sum(file_size for _, _, file_size, _, flags, _ in loaded if flags & 1)

                                if code_size + size > code_limit:
                                    return overlays

                                overlays.append((path, position, data, entry, segments))
                                code_size += size

                        position = view.find(b"\x7fELF", position + 4)
        except (OSError, ValueError):
            continue

    return overlays


def read_overlay_code(data):
    code = {}
    words = {}

    for address, word in read_code(data):
        instruction = decode(address, word)

        if instruction[0] == "word":
            continue

        code[address] = instruction
        words[address] = word

    return code, words


def add_line(lines, line):
    lines.append(line if line.endswith(":") else f"    {line}")


def add_branch_with_long_delay(lines, address, instruction, delay, label, emit_at):
    likely = instruction[0].endswith("l")
    taken = f"{label(address)}_taken"

    for line in emit_at(address, instruction, lambda _: taken):
        add_line(lines, line)

    add_line(lines, "nop")

    if not likely:
        for line in emit_at(address + 4, delay, label, address + 0x100000000):
            add_line(lines, line)

    add_line(lines, f"j {label(address + 8)}")
    add_line(lines, "nop")
    lines.append(f"{taken}:")

    for line in emit_at(address + 4, delay, label, address + 0x200000000):
        add_line(lines, line)

    add_line(lines, f"j {label(target_of(instruction))}")
    add_line(lines, "nop")


def add_jump_with_long_delay(lines, address, instruction, delay, label, emit_at):
    delay = delay or ("nop",)
    name = instruction[0]
    link = 31 if name == "jal" else instruction[1] if name == "jalr" else 0
    source = instruction[-1] if name in ("jr", "jalr") else None
    save_target = source and source in (link, output_register(delay))

    if save_target:
        add_line(lines, f"mthi1 ${source}")

    if link:
        add_line(lines, f"lui ${link}, %hi({label(address + 8)})")
        add_line(lines, f"addiu ${link}, ${link}, %lo({label(address + 8)})")

    for line in emit_at(address + 4, delay, label, address + 0x100000000, preserve_hi1=bool(save_target)):
        add_line(lines, line)

    if source and (name == "jalr" or source != 31 or save_target):
        for line in resolve_indirect_jump(None if save_target else source):
            add_line(lines, line)
        return

    if source is not None:
        add_line(lines, ".word 0x00000008" if source == 0 else f"jr ${source}")
    elif instruction[1] == 0:
        add_line(lines, ".word 0x08000000")
    else:
        add_line(lines, f"j {label(instruction[1])}")
    add_line(lines, "nop")


def needs_jump_lowering(instruction, delay_lines):
    name = instruction[0]
    if name == "jalr" and instruction[2] != 0:
        return True
    if name == "jr" and instruction[1] not in (0, 31):
        return True
    return name in ("j", "jal", "jr", "jalr") and len(delay_lines) > 1


def recompile(input_path, output_path):
    print("recompilando... deixa o cara gozar")

    elf = input_path.read_bytes()
    absolute = struct.unpack_from("<H", elf, 16)[0] == 2
    data, segments, entry, relocations, references = relocate(elf, base=0 if absolute else PSP_USER_LOAD_ADDRESS)
    module = find_module(data, segments)
    imports = read_imports(data, segments, module)
    code, names, base = read_game_code(elf, data, segments, absolute=absolute)
    image_base = min(vaddr - offset for offset, vaddr, *_ in segments)
    overlays = []

    for overlay in find_overlays(input_path.parent, image_base, len(data)):
        overlay_path, overlay_position, overlay_data, overlay_entry, overlay_segments = overlay
        overlay_code, overlay_words = read_overlay_code(overlay_data)
        overlays.append((
            overlay_path,
            overlay_position,
            overlay_data,
            overlay_entry,
            overlay_segments,
            overlay_code,
            overlay_words,
        ))

    import_map = {
        item["stub"]: item
        for item in imports
        if item["kind"] == "function"
    }

    for stub in import_map:
        code.pop(stub, None)
        code.pop(stub + 4, None)

    addresses = set(code)
    missing = references["targets"] - addresses - set(import_map)

    if missing:
        address = min(missing)
        raise ValueError(f"o alvo 0x{address:08x} sumiu do código")

    missing_fixups = (
        set(references["high"]) | set(references["low"])
    ) - addresses

    if missing_fixups:
        address = min(missing_fixups)
        raise ValueError(f"o ajuste em 0x{address:08x} caiu fora do código")

    callbacks = find_callbacks(code, import_map)
    calls = {
        instruction[1]
        for instruction in code.values()
        if instruction[0] == "jal" and instruction[1] in addresses
    }
    starts = {entry} | callbacks | calls | (references["targets"] & addresses)
    direct_targets = {
        target
        for instruction in code.values()
        if (target := target_of(instruction)) not in (None, 0)
    }
    return_followers = {
        address + 8
        for address, instruction in code.items()
        if instruction[0] == "jr" and instruction[1] == 31 and address + 8 in addresses
    }
    map_addresses = (
        {entry} |
        callbacks |
        references["targets"] |
        direct_targets |
        set(import_map) |
        return_followers
    )

    def label(address):
        if address in import_map:
            return import_symbol(import_map[address])

        name = "guest" if address in starts or address not in addresses else "code"
        return f"{name}_{address:08x}"

    external = (references["targets"] | direct_targets) - addresses
    unknown = external - set(import_map)

    unsupported_targets = set(unknown)

    word_locations = {base + offset for offset in references["words"]}
    jump_tables = find_jump_tables(code, word_locations) if word_locations else {}
    if not word_locations:
        print("sem relocations de jump tables")

    def emit_at(address, instruction, current_label=label, lower_address=None, preserve_hi1=False):
        emission_address = address if instruction[0] == "vfpu_call" else (lower_address or address)
        return emit(instruction, current_label, emission_address, preserve_hi1)

    lines = [".set noreorder", ".set noat", ".text", ".globl guest_entry"]

    single_instruction = set()
    for address in sorted(code):
        instruction = code[address]

        if address == entry:
            lines.append("guest_entry:")

        lines.append(f"{label(address)}:")

        delay = code.get(address + 4)
        delay_lines = emit_at(address + 4, delay) if delay else []

        if instruction[0] in branch_names and (len(delay_lines) > 1 or instruction[0] in vfpu_branches):
            add_branch_with_long_delay(
                lines,
                address,
                instruction,
                delay,
                label,
                emit_at,
            )
            continue

        if needs_jump_lowering(instruction, delay_lines):
            add_jump_with_long_delay(
                lines,
                address,
                instruction,
                delay,
                label,
                emit_at,
            )
            continue

        native = emit_at(address, instruction)
        if len(native) == 1:
            single_instruction.add(address)
        for line in native:
            add_line(lines, line)

    overlay_entries = []

    for overlay_index, overlay in enumerate(overlays):
        overlay_path, overlay_position, overlay_data, overlay_entry, overlay_segments, overlay_code, overlay_words = overlay
        overlay_addresses = set(overlay_code)

        def overlay_label(address, index=overlay_index, current=overlay_addresses):
            if address in current:
                return f"overlay_{index}_{address:08x}"

            return label(address)

        overlay_direct_targets = {
            target
            for instruction in overlay_code.values()
            if (target := target_of(instruction)) not in (None, 0)
        }
        overlay_external = overlay_direct_targets - overlay_addresses - addresses - set(import_map)
        unsupported_targets.update(overlay_external)
        def emit_overlay_at(address, instruction, current_label=overlay_label, lower_address=None, preserve_hi1=False):
            return emit_at(address, instruction, current_label, lower_address, preserve_hi1)

        for address in sorted(overlay_code):
            instruction = overlay_code[address]
            lines.append(f"{overlay_label(address)}:")
            delay = overlay_code.get(address + 4)
            delay_lines = emit_overlay_at(address + 4, delay) if delay else []

            if instruction[0] in branch_names and (len(delay_lines) > 1 or instruction[0] in vfpu_branches):
                add_branch_with_long_delay(
                    lines,
                    address,
                    instruction,
                    delay,
                    overlay_label,
                    emit_overlay_at,
                )
                continue

            if needs_jump_lowering(instruction, delay_lines):
                add_jump_with_long_delay(
                    lines,
                    address,
                    instruction,
                    delay,
                    overlay_label,
                    emit_overlay_at,
                )
                continue

            for line in emit_overlay_at(address, instruction):
                add_line(lines, line)

        overlay_map_addresses = set(overlay_addresses)

        for _, address, file_size, _, _, _ in overlay_segments:
            start = address
            end = address + file_size

            for location in range(start, end - 3, 4):
                try:
                    value = read_word(overlay_data, overlay_segments, location)
                except ValueError:
                    break

                if value in overlay_addresses:
                    overlay_map_addresses.add(value)
                elif value in addresses:
                    map_addresses.add(value)

        overlay_entries.extend(
            (address, overlay_words[address], overlay_label(address))
            for address in sorted(overlay_map_addresses)
            if address in overlay_words
        )

    for target in sorted(unsupported_targets):
        lines.extend([
            f"guest_{target:08x}:",
            "    jr $ra",
            "    nop",
        ])
    linked_imports = link_imports(lines, imports)

    binary_path = output_path.with_suffix(".bin")
    map_path = output_path.with_suffix(".map")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    binary_path.write_bytes(data)

    add_map(lines, map_addresses - addresses, label)
    add_code_map(lines, addresses, image_base, label, single_instruction)
    add_overlay_map(lines, overlay_entries)
    file_size = add_image(lines, binary_path, data, segments, image_base)
    add_info(lines, image_base, len(data), file_size, module["gp"])

    output_path.write_text("\n".join(lines) + "\n")
    write_map(map_path, map_addresses, label)

    print(f"destinos externosa deveras leviano sem código {len(unsupported_targets)}")
    print(f"o assembly saiu do forno {output_path}")
    print(f"o não binário caiu no prato do carnivoro {binary_path}")
    print(f"o mapa dessa safada ficou em {map_path}")
    print(f"entradas de código na pista {len(starts)}")
    print(f"callbacks pegos no flagra com a julia do segundo ano do ensino medio no canto esquerdo da sala em são paulo que ela largou ele porque ela tava querendo focar nos estudos{len(callbacks)}")
    print(f"instruções que foram pra chapa {len(code)}")
    print(f"tipos de instrução que fizeram o decoder gozar {len(names)}")
    print(f"relocações que pararam de apontar pra puta que pariu {relocations}")
    print(f"ponteiros de código que acharam o rumo {len(references['targets'])}")
    print(f"tabelas de salto que pararam de se esconder {len(jump_tables)}")
    print(f"endereços que não vão mais se perder no mapa {len(map_addresses)}")
    print(f"entradas de overlay que acharam o rumo {len(overlay_entries)}")
    for overlay_path, overlay_position, *_ in overlays:
        print(f"overlay recompilado {overlay_path} 0x{overlay_position:x}")
    print(f"imports amarrados na nega {linked_imports}")
    print(f"dados do jogo no pacote {file_size} bytes")
    print(f"bss zerada bonitinha {len(data) - file_size} bytes")
    print("agora sim está de pé diferente do meu pa papel")


def main():
    input_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("jogo.elf")
    output_path = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("build/game.s")

    try:
        recompile(input_path, output_path)
        return 0
    except (OSError, ValueError, struct.error) as error:
        print(f"deu ruim {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
