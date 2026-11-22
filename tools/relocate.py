import struct
import sys
from pathlib import Path


PSP_USER_LOAD_ADDRESS = 0x08804000


def signed(value):
    return value - 0x10000 if value & 0x8000 else value


def read_headers(data):
    ph_offset = struct.unpack_from("<I", data, 28)[0]
    sh_offset = struct.unpack_from("<I", data, 32)[0]
    ph_size, ph_count = struct.unpack_from("<HH", data, 42)
    sh_size, sh_count = struct.unpack_from("<HH", data, 46)

    programs = [
        struct.unpack_from("<IIIIIIII", data, ph_offset + index * ph_size)
        for index in range(ph_count)
    ]
    sections = [
        struct.unpack_from("<IIIIIIIIII", data, sh_offset + index * sh_size)
        for index in range(sh_count)
    ]
    return programs, sections


def build_image(data, programs):
    loads = [program for program in programs if program[0] == 1]

    if not loads:
        raise ValueError("não tem nenhum segmento pra carregar nessa porra")

    start = min(program[2] for program in loads)
    end = max(program[2] + program[5] for program in loads)
    image = bytearray(end - start)

    for program in loads:
        _, offset, address, _, file_size, _, _, _ = program
        image_offset = address - start
        image[image_offset:image_offset + file_size] = data[offset:offset + file_size]

    return image, start


def read_relocations(data, sections):
    tables = []

    for section in sections:
        kind = section[1]
        offset = section[4]
        size = section[5]

        if kind != 0x700000a0 or size == 0:
            continue
        if size % 8:
            raise ValueError("a tabela de relocação veio mais torta que minha pica")

        tables.append([
            struct.unpack_from("<II", data, offset + position)
            for position in range(0, size, 8)
        ])

    return tables


def patch_offset(programs, relocation, image_start):
    offset, info = relocation
    segment = (info >> 8) & 0xff

    if segment >= len(programs) or programs[segment][0] != 1:
        raise ValueError(f"a relocação apontou pro segmento do caralho {segment}")

    return programs[segment][2] + offset - image_start


def target_address(programs, relocation, base):
    target = (relocation[1] >> 16) & 0xff

    if target >= len(programs) or programs[target][0] != 1:
        raise ValueError(f"a relocação quer um segmento que nem existe {target}")

    return base + programs[target][2]


def find_low_relocation(table, index):
    info = table[index][1]

    for next_index in range(index + 1, len(table)):
        next_relocation = table[next_index]
        kind = next_relocation[1] & 0xff

        if kind == 5:
            continue
        if kind not in (1, 6):
            continue
        if (next_relocation[1] >> 8) == (info >> 8):
            return next_index

    return None


def apply_table(image, programs, table, base, image_start, counts):
    words = []

    for relocation in table:
        offset = patch_offset(programs, relocation, image_start)
        if offset < 0 or offset + 4 > len(image):
            raise ValueError(f"a relocação saiu passeando em 0x{offset:08x}")
        words.append(struct.unpack_from("<I", image, offset)[0])

    for index, relocation in enumerate(table):
        offset = patch_offset(programs, relocation, image_start)
        info = relocation[1]
        kind = info & 0xff
        word = words[index]
        target = target_address(programs, relocation, base)

        if kind == 2:
            patched = word + target
        elif kind == 4:
            patched = (word & 0xfc000000) | (((word & 0x03ffffff) + (target >> 2)) & 0x03ffffff)
        elif kind == 5:
            next_index = find_low_relocation(table, index)
            if next_index is None:
                raise ValueError(f"o hi16 ficou solteiro em 0x{offset:08x}")

            value = ((word & 0xffff) << 16) + signed(words[next_index] & 0xffff) + target
            patched = (word & 0xffff0000) | (((value + 0x8000) >> 16) & 0xffff)
        elif kind in (1, 6):
            patched = (word & 0xffff0000) | (((word & 0xffff) + target) & 0xffff)
        else:
            raise ValueError(f"ainda não sei realocar o tipo {kind}")

        struct.pack_into("<I", image, offset, patched & 0xffffffff)
        counts[kind] = counts.get(kind, 0) + 1


def find_code_references(image, programs, sections, tables, base, image_start):
    ranges = []

    for section in sections:
        _, kind, flags, address, _, size, _, _, _, _ = section

        if flags & 4 and kind != 8:
            ranges.append((base + address, base + address + size))

    targets = set()
    words = {}
    high = {}
    low_fixups = {}

    def add_pointer(value):
        if value % 4:
            return False

        if any(start <= value < end for start, end in ranges):
            targets.add(value)
            return True

        return False

    for table in tables:
        for index, relocation in enumerate(table):
            kind = relocation[1] & 0xff
            offset = patch_offset(programs, relocation, image_start)
            word = struct.unpack_from("<I", image, offset)[0]

            if kind == 2:
                if add_pointer(word):
                    if offset in words and words[offset] != word:
                        raise ValueError(f"o ponteiro em 0x{offset:08x} apontou pra dois lugares")

                    words[offset] = word
                continue

            if kind != 5:
                continue

            next_index = find_low_relocation(table, index)
            if next_index is None:
                continue

            next_relocation = table[next_index]
            next_offset = patch_offset(programs, next_relocation, image_start)
            low_word = struct.unpack_from("<I", image, next_offset)[0]
            value = ((word & 0xffff) << 16) + signed(low_word & 0xffff)
            value &= 0xffffffff

            if not add_pointer(value):
                continue

            high_address = base + image_start + offset

            if high_address in high and high[high_address] != value:
                raise ValueError(f"o hi16 em 0x{high_address:08x} apontou pra dois lugares")

            high[high_address] = value
            low_address = base + image_start + next_offset

            if low_address in low_fixups and low_fixups[low_address] != value:
                raise ValueError(f"o lo16 em 0x{low_address:08x} apontou pra dois lugares")

            low_fixups[low_address] = value

    return {
        "targets": targets,
        "words": words,
        "high": high,
        "low": low_fixups,
    }


def relocate(data, base=PSP_USER_LOAD_ADDRESS):
    programs, sections = read_headers(data)
    image, image_start = build_image(data, programs)
    tables = read_relocations(data, sections)
    counts = {}

    for table in tables:
        apply_table(image, programs, table, base, image_start, counts)

    code_references = find_code_references(
        image,
        programs,
        sections,
        tables,
        base,
        image_start,
    )

    segments = []
    for kind, _, address, _, file_size, memory_size, flags, align in programs:
        if kind == 1:
            segments.append((
                address - image_start,
                base + address,
                file_size,
                memory_size,
                flags,
                align,
            ))

    entry = base + struct.unpack_from("<I", data, 24)[0]
    total = sum(counts.values())
    return bytes(image), segments, entry, total, code_references


def main():
    input_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("jogo.elf")
    output_path = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("build/game.bin")

    try:
        image, _, entry, count, code_references = relocate(input_path.read_bytes())
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(image)
        print(f"entrada realocada 0x{entry:08x}")
        print(f"relocações {count}")
        print(f"ponteiros de código {len(code_references['targets'])}")
        print(f"escrevi {output_path}")
        return 0
    except (OSError, ValueError, struct.error) as error:
        print(f"deu ruim {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
