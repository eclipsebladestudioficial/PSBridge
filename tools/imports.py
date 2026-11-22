import struct
import sys
from pathlib import Path

from elf_reader import read_header, read_segments

def import_symbol(item) :
    library = "".join(
        char if char.isalnum() or char == "_" else "_"
        for char in item ["library"]
    )

    return f"psp_{library}_{item['nid']:08x}"

def library_id(name):
    value = 2166136261

    for byte in name.encode("ascii"):
        value ^= byte
        value = value * 16777619 & 0xffffffff

    return value

def import_binding(library, nid):
    return f"psp_binding_{library:08x}_{nid:08x}"


def link_imports(lines, imports, exports=()):
    variables = [item for item in imports if item ["kind"] == "variable"]

    if variables:
        raise ValueError("gozei e não sei ligar ainda esse import")

    functions = {}

    for item in sorted(imports, key=lambda entry: entry["stub"]):
        if item["kind"] != "function":
            continue
        

        functions.setdefault((item["library"], item["nid"]), item)

    functions = list(functions.values())

    libraries = {
        name: f"psp_import_library_{index}"
        for index, name in enumerate(sorted({item["library"] for item in functions}))
    }

    lines.extend([
        '.section .text.psp_imports,"ax",@progbits',
        ".balign 4",
        ".extern psbridge_import",
    ])

    for index, item in enumerate(functions):
        symbol = import_symbol(item)
        descriptor = f"psp_import_{index}"
        key = (library_id(item["library"]), item["nid"])

        lines.extend([
            f".globl {symbol}",
            f".type {symbol}, @function",
            f"{symbol}:",
        ])
        if key in exports:
            binding = import_binding(*key)
            lines.extend([
                f"    lui $25, %hi({binding})",
                f"    lw $25, %lo({binding})($25)",
                f"    beq $25, $0, {symbol}_hle",
                "    nop",
                "    jr $25",
                "    nop",
                f"{symbol}_hle:",
            ])
        lines.extend([
            f"    lui $25, %hi({descriptor})",
            "    j psbridge_import",
            f"    addiu $25, $25, %lo({descriptor})",
            f".size {symbol}, . - {symbol}",
        ])

    lines.extend([
        '.section .rodata.psp_imports,"a",@progbits',
        ".balign 4",
        ".globl psp_import_count",
        ".globl psp_import_table",
        ".globl psp_import_table_end",
        "psp_import_count:",
        f".word {len(functions)}",
        "psp_import_table:",
    ])

    for index, item in enumerate(functions):
        lines.extend([
            f"psp_import_{index}:",
            f".word 0x{item['nid']:08x}",
            f".word 0x{library_id(item ['library']):08x}",
            f".word 0x{item['stub']:08x}",
            f".word {import_symbol(item)}",
        ])

    lines.append("psp_import_table_end:")

    for library, symbol in libraries.items():
        escaped = library.replace("\\", "\\\\").replace('"', '\\"')

        lines.extend([
            f"{symbol}:",
            f'.asciz "{escaped}"',
        ])

    return len(functions)


def file_offset(segments, address, size=1):
    for offset, vaddr, file_size, _, _, _ in segments:
        if vaddr <= address and address + size <= vaddr + file_size:
            return offset + address - vaddr

    raise ValueError(f"esse endereço sumiu do mapa 0x{address:08x}")


def read_string(data, segments, address):
    offset = file_offset(segments, address)
    end = data.find(b"\0", offset)

    if end == -1:
        raise ValueError(f"essa string não aguentou e gozou em 0x{address:08x}")

    return data[offset:end].decode("ascii")


def valid_address(segments, address, size=0, file_only=False):
    if address == 0:
        return True

    for _, vaddr, file_size, memory_size, _, _ in segments:
        limit = file_size if file_only else memory_size
        if vaddr <= address and address + size <= vaddr + limit:
            return True

    return False


def find_module(data, segments, allow_no_imports=False):
    found = []

    for segment_offset, _, file_size, _, _, _ in segments:
        end = segment_offset + file_size - 52

        for offset in range(segment_offset, end + 1, 4):
            raw_name = data[offset + 4:offset + 32]

            if b"\0" not in raw_name:
                continue

            name = raw_name.split(b"\0", 1)[0]
            if not name or not all(32 <= char < 127 for char in name):
                continue

            attributes, version = struct.unpack_from("<HH", data, offset)
            gp, export_start, export_end, import_start, import_end = struct.unpack_from(
                "<IIIII", data, offset + 32
            )
            addresses = (export_start, export_end, import_start, import_end)

            if not all(valid_address(segments, address) for address in addresses):
                continue
            if export_start > export_end or import_start > import_end:
                continue
            if import_end == import_start and (not allow_no_imports or export_end == export_start):
                continue

            try:
                imports = read_imports(data, segments, {
                    "import_start": import_start,
                    "import_end": import_end,
                })
            except (UnicodeDecodeError, ValueError, struct.error):
                continue

            found.append({
                "name": name.decode("ascii"),
                "attributes": attributes,
                "version": version,
                "gp": gp,
                "export_start": export_start,
                "export_end": export_end,
                "import_start": import_start,
                "import_end": import_end,
                "import_count": len(imports),
            })

    if not found:
        raise ValueError("o módulo do psp foi pra puta que pariu")

    return max(found, key=lambda module: (module["import_count"], module["version"] == 0x0101))


def read_library_exports(data, segments, module):
    exports = {}
    address = module["export_start"]

    while address < module["export_end"]:
        offset = file_offset(segments, address, 16)
        name, _, _, length, variables, functions, table = struct.unpack_from("<IHHBBHI", data, offset)
        if length != 4 or address + 16 > module["export_end"]:
            raise ValueError("a tabela de exports veio toda torta")

        if functions:
            offset = file_offset(segments, table, (variables + functions) * 8)
            library = library_id(read_string(data, segments, name)) if name else None
            for index in range(functions):
                nid = struct.unpack_from("<I", data, offset + index * 4)[0]
                entry = struct.unpack_from("<I", data, offset + (variables + functions + index) * 4)[0]
                exports[library, nid] = entry
        address += 16

    return exports


def read_imports(data, segments, module):
    imports = []
    address = module["import_start"]

    while address < module["import_end"]:
        offset = file_offset(segments, address, 20)
        name_address, _, _, length, variables, functions, nid_address, stub_address = struct.unpack_from(
            "<IHHBBHII", data, offset
        )

        entry_size = length * 4
        if length < 5 or address + entry_size > module["import_end"]:
            raise ValueError(f"esse import tá mais torto que minha pica em 0x{address:08x}")

        library = read_string(data, segments, name_address)

        if functions and not valid_address(segments, nid_address, functions * 4, True):
            raise ValueError(f"os nids sumiram em 0x{address:08x}")
        if functions and not valid_address(segments, stub_address, functions * 8, True):
            raise ValueError(f"os stubs sumiram em 0x{address:08x}")

        for index in range(functions):
            nid_offset = file_offset(segments, nid_address + index * 4, 4)
            nid = struct.unpack_from("<I", data, nid_offset)[0]
            imports.append({
                "library": library,
                "kind": "function",
                "nid": nid,
                "stub": stub_address + index * 8,
            })

        if variables:
            if length < 6:
                raise ValueError(f"as variáveis vieram sem endereço em 0x{address:08x}")

            variable_address = struct.unpack_from("<I", data, offset + 20)[0]
            if not valid_address(segments, variable_address, variables * 8, True):
                raise ValueError(f"as variáveis sumiram em 0x{address:08x}")

            for index in range(variables):
                variable_offset = file_offset(segments, variable_address + index * 8, 8)
                references, nid = struct.unpack_from("<II", data, variable_offset)
                imports.append({
                    "library": library,
                    "kind": "variable",
                    "nid": nid,
                    "references": references,
                })

        address += entry_size

    if address != module["import_end"]:
        raise ValueError("a tabela de imports terminou no lugar errado")

    return imports


def inspect(path):
    data = path.read_bytes()
    _, _, ph_offset, ph_size, ph_count = read_header(data)
    segments = read_segments(data, ph_offset, ph_size, ph_count)
    module = find_module(data, segments)
    imports = read_imports(data, segments, module)
    libraries = sorted({item["library"] for item in imports})

    print("gozei nas parada do psp")
    print(f"arquivo {path}")
    print(f"módulo {module['name']}")
    print(f"bibliotecas {len(libraries)}")
    functions = sum(item["kind"] == "function" for item in imports)
    variables = sum(item["kind"] == "variable" for item in imports)
    print(f"funções {functions}")
    print(f"variáveis {variables}")

    for library in libraries:
        print()
        print(library)

        for item in imports:
            if item["library"] == library:
                if item["kind"] == "function":
                    print(f"  stub 0x{item['stub']:08x}  nid 0x{item['nid']:08x}")
                else:
                    print(f"  variável 0x{item['references']:08x}  nid 0x{item['nid']:08x}")


def main():
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("jogo.elf")

    try:
        inspect(path)
        return 0
    except (OSError, UnicodeDecodeError, ValueError, struct.error) as error:
        print(f"deu ruim {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
