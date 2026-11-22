from allegrex import branch_names, decode
from elf_reader import read_function


callback_imports = {
    ("ThreadManForUser", 0x446d8de6): 5,
    ("ThreadManForUser", 0xbc80ec7c): 5,
}

writes_register = {
    "sll", "srl", "sra", "sllv", "srlv", "srav", "addu", "sub", "subu",
    "and", "or", "xor", "nor", "slt", "sltu", "movz", "movn", "rotr", "rotrv",
    "min", "max", "clz", "mfhi", "mflo", "addiu", "slti", "sltiu",
    "andi", "ori", "xori", "addi", "lui", "lb", "lbu", "lh", "lhu", "lwl",
    "lwr", "lw", "mfc1", "cfc1", "ext", "ins", "wsbw", "bitrev", "seb", "seh",
}


def is_code(segments, address):
    for _, vaddr, file_size, _, flags, _ in segments:
        if flags & 1 and vaddr <= address <= vaddr + file_size - 4:
            return True
    return False


def output_register(instruction):
    if instruction[0] in writes_register:
        return instruction[1]

    if instruction[0] == "jal":
        return 31

    if instruction[0] == "jalr":
        return instruction[1]

    return None


def result_value(instructions, index, instruction, seen):
    name = instruction[0]

    if name == "lui":
        return instruction[2] << 16

    if name in ("addiu", "ori"):
        source = register_value(instructions, index, instruction[2], seen.copy())
        if source is None:
            return None

        if name == "addiu":
            return (source + instruction[3]) & 0xffffffff

        return source | instruction[3]

    if name in ("addu", "or"):
        left = register_value(instructions, index, instruction[2], seen.copy())
        right = register_value(instructions, index, instruction[3], seen.copy())

        if left is None or right is None:
            return None

        if name == "addu":
            return (left + right) & 0xffffffff

        return left | right

    if name == "sll":
        value = register_value(instructions, index, instruction[2], seen.copy())
        if value is not None:
            return (value << instruction[3]) & 0xffffffff

    return None


def register_value(instructions, index, register, seen=None):
    if register == 0:
        return 0

    seen = seen or set()
    key = index, register
    if key in seen:
        return None

    seen.add(key)

    controls = ("j", "jal", "jr", "jalr") + branch_names

    for position in range(index - 1, max(-1, index - 32), -1):
        instruction = instructions[position][1]

        if instruction[0] in controls:
            return None
        if output_register(instruction) == register:
            return result_value(instructions, position, instruction, seen)

    return None


def callback_value(instructions, index, register):
    value = register_value(instructions, index, register)
    call_address = instructions[index][0]

    if index + 1 >= len(instructions):
        return value

    delay_address, delay = instructions[index + 1]
    if delay_address != call_address + 4 or output_register(delay) != register:
        return value

    return result_value(instructions, index, delay, set())


def find_jump_tables(code, word_locations):
    instructions = sorted(code.items())
    tables = {}

    for index, (address, instruction) in enumerate(instructions):
        if instruction[0] != "jr" or instruction[1] == 31:
            continue

        register = instruction[1]
        load_index = None

        for position in range(index - 1, max(-1, index - 4), -1):
            candidate_address, candidate = instructions[position]

            if address - candidate_address > 12:
                break

            if candidate[0] == "lw" and candidate[1] == register:
                load_index = position
                break

        if load_index is None:
            continue

        load = instructions[load_index][1]
        base_register = load[2]
        offset = load[3]
        add_index = None

        load_address = instructions[load_index][0]

        for position in range(load_index - 1, max(-1, load_index - 8), -1):
            candidate_address, candidate = instructions[position]

            if load_address - candidate_address > 28:
                break

            if candidate[0] in ("addu", "addiu", "or") and candidate[1] == base_register:
                add_index = position
                break

        if add_index is None:
            continue

        add = instructions[add_index][1]

        if add[0] == "addiu":
            source = register_value(instructions, add_index, add[2])
            values = [] if source is None else [(source + add[3]) & 0xffffffff]
        else:
            values = [
                register_value(instructions, add_index, add[2]),
                register_value(instructions, add_index, add[3]),
            ]

        bases = {
            (value + offset) & 0xffffffff
            for value in values
            if value is not None
        }
        bases &= word_locations

        if not bases:
            continue
        if len(bases) != 1:
            raise ValueError(f"a tabela do jr em 0x{address:08x} sumiu")

        tables[address] = bases.pop()

    return tables


def find_callbacks(code, import_map):
    instructions = sorted(code.items())
    callbacks = set()

    for index, (_, instruction) in enumerate(instructions):
        if instruction[0] != "jal" or instruction[1] not in import_map:
            continue

        imported = import_map[instruction[1]]
        callback_register = callback_imports.get(
            (imported["library"], imported["nid"])
        )
        if callback_register is None:
            continue

        callback = callback_value(instructions, index, callback_register)
        if callback in code:
            callbacks.add(callback)

    return callbacks


def find_functions(data, segments, entry, import_map, indirect_targets=()):
    functions = {}
    callbacks = set()
    pending = sorted(indirect_targets, reverse=True)
    pending.append(entry)

    while pending:
        start = pending.pop()
        if start in functions:
            continue

        instructions = []
        for address, word in read_function(data, segments, start, import_map):
            instruction = decode(address, word)
            if instruction[0] == "word":
                raise ValueError(f"que porra de instrução é essa em 0x{address:08x}")
            instructions.append((address, instruction))

            if instruction[0] != "jal":
                continue

            target = instruction[1]
            if target not in import_map and is_code(segments, target) and target not in functions:
                pending.append(target)

        for index, (_, instruction) in enumerate(instructions):
            if instruction[0] != "jal" or instruction[1] not in import_map:
                continue

            imported = import_map[instruction[1]]
            callback_register = callback_imports.get(
                (imported["library"], imported["nid"])
            )
            if callback_register is None:
                continue

            callback = callback_value(instructions, index, callback_register)
            if callback is None or not is_code(segments, callback):
                continue

            callbacks.add(callback)
            if callback not in functions:
                pending.append(callback)

        functions[start] = instructions

    return functions, callbacks
