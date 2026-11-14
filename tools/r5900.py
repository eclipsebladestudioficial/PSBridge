from allegrex import float_branches, one_reg_branches, two_reg_branches, vfpu_branches


def reg(index):
    return f"${index}"


def freg(index):
    return f"$f{index}"


def load_value(register, value):
    high = (value >> 16) & 0xffff
    low = value & 0xffff

    if high == 0:
        return [f"ori {reg(register)}, $0, 0x{low:04x}"]
    if low == 0:
        return [f"lui {reg(register)}, 0x{high:04x}"]
    return [
        f"lui {reg(register)}, 0x{high:04x}",
        f"ori {reg(register)}, {reg(register)}, 0x{low:04x}",
    ]


def resolve_jump(source):
    lines = [
        "sw $25, -4($sp)",
        "sw $31, -8($sp)",
        f"or $25, {reg(source)}, $0",
        "addiu $sp, $sp, -8",
        "jal psbridge_resolve_guest",
        "nop",
    ]

    if source != 31:
        lines.append("lw $31, 0($sp)")

    lines.append(f"or {reg(source)}, $25, $0")

    if source != 25:
        lines.append("lw $25, 4($sp)")

    lines.append("addiu $sp, $sp, 8")
    return lines


def emit_vfpu_dispatch(opcode, address):
    return [
        "addiu $sp, $sp, -32",
        "sw $24, 0($sp)",
        "sw $25, 4($sp)",
        "sw $31, 8($sp)",
        "sw $27, 20($sp)",
        *load_value(24, opcode),
        "sw $24, 12($sp)",
        *load_value(25, address & 0xffffffff),
        "sw $25, 16($sp)",
        "jal psbridge_vfpu_entry",
        "nop",
        "lw $27, 20($sp)",
        "addiu $sp, $sp, 32"
    ]


def refresh_guest_kernel_register():
    registers = (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 27, 28, 31)
    lines = ["addiu $sp, $sp, -96"]
    for index, register in enumerate(registers):
        lines.append(f"sw {reg(register)}, {16 + index * 4}($sp)")
    lines.extend([
        "lui $gp, %hi(_gp)",
        "addiu $gp, $gp, %lo(_gp)",
        "jal psbridge_thread_kernel_work",
        "nop",
        "or $26, $2, $0",
    ])
    for index, register in enumerate(registers):
        lines.append(f"lw {reg(register)}, {16 + index * 4}($sp)")
    lines.append("addiu $sp, $sp, 96")
    return lines


def emit_msub(instruction):
    name, rs, rt, _ = instruction
    scratch = (26, 27, 1, 25)
    source_rs, source_rt = rs, rt
    lines = ["addiu $sp, $sp, -32"]

    for index, register in enumerate(scratch):
        lines.append(f"sw {reg(register)}, {index * 4}($sp)")

    lines.extend([
        "mfhi1 $26",
        "sw $26, 16($sp)",
        "mflo1 $26",
        "sw $26, 20($sp)",
    ])

    for index, register in enumerate(scratch):
        lines.append(f"lw {reg(register)}, {index * 4}($sp)")

    if rs == 29 or rt == 29:
        temporary = next(item for item in scratch if item not in (rs, rt))
        lines.append(f"addiu {reg(temporary)}, $sp, 32")
        source_rs = temporary if rs == 29 else rs
        source_rt = temporary if rt == 29 else rt

    lines.extend([
        f"{'multu1' if name == 'msubu' else 'mult1'} {reg(source_rs)}, {reg(source_rt)}",
        "mflo1 $26",
        "mfhi1 $27",
        "mflo $1",
        "sltu $25, $1, $26",
        "subu $1, $1, $26",
        "mtlo $1",
        "mfhi $1",
        "subu $1, $1, $27",
        "subu $1, $1, $25",
        "mthi $1",
        "lw $26, 20($sp)",
        "mtlo1 $26",
        "lw $26, 16($sp)",
        "mthi1 $26",
    ])

    for index, register in enumerate(scratch):
        lines.append(f"lw {reg(register)}, {index * 4}($sp)")

    lines.append("addiu $sp, $sp, 32")
    return lines


def emit_float_round(name, destination, source, address):
    scratch = (26, 27, 1, 25, 24)
    done = f"lower_{address:08x}_round_done"
    large = f"lower_{address:08x}_round_large"
    adjust = f"lower_{address:08x}_round_adjust"
    negative = f"lower_{address:08x}_round_negative"
    lines = ["addiu $sp, $sp, -32"]

    for index, register in enumerate(scratch):
        lines.append(f"sw {reg(register)}, {index * 4}($sp)")

    lines.extend([
        f"mfc1 $26, {freg(source)}",
        f"trunc.w.s {freg(destination)}, {freg(source)}",
        f"mfc1 $1, {freg(destination)}",
        "sll $27, $26, 1",
        "srl $27, $27, 1",
    ])

    if name == "ceil.w.s":
        lines.extend([
            f"bltz $26, {done}",
            "nop",
            f"beq $27, $0, {done}",
            "nop",
            "lui $25, 0x3f80",
            "sltu $24, $27, $25",
            f"beq $24, $0, {large}",
            "nop",
            "addiu $1, $0, 1",
            f"b {done}",
            "nop",
            f"{large}:",
        ])
    elif name == "floor.w.s":
        lines.extend([
            f"bgez $26, {done}",
            "nop",
            f"beq $27, $0, {done}",
            "nop",
            "lui $25, 0x3f80",
            "sltu $24, $27, $25",
            f"beq $24, $0, {large}",
            "nop",
            "addiu $1, $0, -1",
            f"b {done}",
            "nop",
            f"{large}:",
        ])
    else:
        lines.extend([
            "lui $25, 0x3f00",
            "sltu $24, $25, $27",
            f"beq $24, $0, {done}",
            "nop",
            "lui $25, 0x3f80",
            "sltu $24, $27, $25",
            f"beq $24, $0, {large}",
            "nop",
            "addiu $1, $0, 1",
            f"bgez $26, {done}",
            "nop",
            "addiu $1, $0, -1",
            f"b {done}",
            "nop",
            f"{large}:",
        ])

    lines.extend([
        "lui $25, 0x4b00",
        "sltu $24, $27, $25",
        f"beq $24, $0, {done}",
        "nop",
        "srl $24, $27, 23",
        "andi $24, $24, 255",
        "addiu $24, $24, -127",
        "lui $25, 0x007f",
        "ori $25, $25, 0xffff",
        "srlv $25, $25, $24",
        "and $24, $26, $25",
    ])

    if name in ("ceil.w.s", "floor.w.s"):
        lines.extend([
            f"beq $24, $0, {done}",
            "nop",
            "addiu $1, $1, 1" if name == "ceil.w.s" else "addiu $1, $1, -1",
        ])
    else:
        lines.extend([
            "srl $27, $25, 1",
            "addiu $27, $27, 1",
            "sltu $25, $27, $24",
            f"bne $25, $0, {adjust}",
            "nop",
            f"bne $24, $27, {done}",
            "nop",
            "andi $25, $1, 1",
            f"beq $25, $0, {done}",
            "nop",
            f"{adjust}:",
            f"bltz $26, {negative}",
            "nop",
            "addiu $1, $1, 1",
            f"b {done}",
            "nop",
            f"{negative}:",
            "addiu $1, $1, -1",
        ])

    lines.extend([
        f"{done}:",
        f"mtc1 $1, {freg(destination)}",
    ])

    for index, register in enumerate(scratch):
        lines.append(f"lw {reg(register)}, {index * 4}($sp)")

    lines.append("addiu $sp, $sp, 32")
    return lines


def emit(instruction, label, address):
    name = instruction[0]

    if name == "vfpu_call":
        return emit_vfpu_dispatch(instruction[1], address)

    if name == "nop":
        return [name]

    if name in ("sll", "srl", "sra"):
        _, rd, rt, shift = instruction
        return [f"{name} {reg(rd)}, {reg(rt)}, {shift}"]
    if name in ("sllv", "srlv", "srav"):
        _, rd, rt, rs = instruction
        return [f"{name} {reg(rd)}, {reg(rt)}, {reg(rs)}"]

    if name == "rotr":
        _, destination, source, shift = instruction

        if destination == 0:
            return ["nop"]
        if shift == 0:
            return [f"or {reg(destination)}, {reg(source)}, $0"]

        scratch = next(
            item for item in (26, 27, 1, 25, 24, 23, 22, 21)
            if item not in (destination, source)
        )

        return [
            "addiu $sp, $sp, -16",
            f"sw {reg(scratch)}, 0($sp)",
            f"or {reg(scratch)}, {reg(source)}, $0" if source != 29 else f"addiu {reg(scratch)}, $sp, 16",
            f"srl {reg(destination)}, {reg(scratch)}, {shift}",
            f"sll {reg(scratch)}, {reg(scratch)}, {32 - shift}",
            f"or {reg(destination)}, {reg(destination)}, {reg(scratch)}",
            f"lw {reg(scratch)}, 0($sp)",
            "addiu $sp, $sp, 16",
        ]

    if name == "rotrv":
        _, destination, source, shift = instruction

        if destination == 0:
            return ["nop"]

        scratch = [
            item for item in (26, 27, 1, 25, 24, 23, 22, 21)
            if item not in (destination, source, shift)
        ][:2]
        value, complement = scratch

        source_copy = (
            f"or {reg(value)}, {reg(source)}, $0"
            if source != 29
            else f"addiu {reg(value)}, $sp, 16"
        )
        shift_copy = (
            f"andi {reg(complement)}, {reg(shift)}, 31"
            if shift != 29
            else f"addiu {reg(complement)}, $sp, 16"
        )

        return [
            "addiu $sp, $sp, -16",
            f"sw {reg(value)}, 0($sp)",
            f"sw {reg(complement)}, 4($sp)",
            source_copy,
            shift_copy,
            f"andi {reg(complement)}, {reg(complement)}, 31" if shift == 29 else "nop",
            f"srlv {reg(destination)}, {reg(value)}, {reg(complement)}",
            f"subu {reg(complement)}, $0, {reg(complement)}",
            f"andi {reg(complement)}, {reg(complement)}, 31",
            f"sllv {reg(value)}, {reg(value)}, {reg(complement)}",
            f"or {reg(destination)}, {reg(destination)}, {reg(value)}",
            f"lw {reg(complement)}, 4($sp)",
            f"lw {reg(value)}, 0($sp)",
            "addiu $sp, $sp, 16",
        ]

    if name == "jr":
        source = instruction[1]

        if source == 0:
            return [".word 0x00000008"]

        if source == 31:
            return [f"jr {reg(source)}"]

        return resolve_jump(source) + [f"jr {reg(source)}"]
    if name == "jalr":
        _, link, source = instruction
        if link == source == 0:
            return [".word 0x00000009"]
        if link == source:
            raise ValueError(f"jalr com registradores iguais em 0x{address:08x}")
        return resolve_jump(source) + [f"jalr {reg(link)}, {reg(source)}"]

    if name in ("addu", "subu", "and", "or", "xor", "nor", "slt", "sltu", "movz", "movn"):
        _, rd, rs, rt = instruction
        return [f"{name} {reg(rd)}, {reg(rs)}, {reg(rt)}"]
    if name in ("min", "max"):
        _, rd, rs, rt = instruction
        host = "pminw" if name == "min" else "pmaxw"
        return [f"{host} {reg(rd)}, {reg(rs)}, {reg(rt)}", f"sll {reg(rd)}, {reg(rd)}, 0"]

    if name in ("mfhi", "mflo", "mthi", "mtlo"):
        return [f"{name} {reg(instruction[1])}"]
    if name in ("mult", "multu", "madd", "maddu"):
        return [f"{name} {reg(instruction[1])}, {reg(instruction[2])}"]
    if name in ("msub", "msubu"):
        return emit_msub(instruction)
    if name in ("div", "divu"):
        return [f".word 0x{instruction[3]:08x}"]

    if name == "clz":
        _, rd, rs = instruction
        zero = f"lower_{address:08x}_zero"
        done = f"lower_{address:08x}_done"

        return [
            f"beq {reg(rs)}, $0, {zero}",
            f"srl {reg(rd)}, {reg(rs)}, 1",
            f"plzcw {reg(rd)}, {reg(rd)}",
            f"b {done}",
            "nop",
            f"{zero}:",
            f"ori {reg(rd)}, $0, 32",
            f"{done}:",
        ]

    if name == "break":
        return [f"break {instruction[1]}"]

    if name in ("j", "jal"):
        if instruction[1] == 0:
            word = 0x08000000 if name == "j" else 0x0c000000
            return [f".word 0x{word:08x}"]

        return [f"{name} {label(instruction[1])}"]

    if name in two_reg_branches:
        _, rs, rt, target = instruction
        setup = refresh_guest_kernel_register() if 26 in (rs, rt) else []
        return setup + [f"{name} {reg(rs)}, {reg(rt)}, {label(target)}"]
    if name in one_reg_branches:
        _, rs, target = instruction
        setup = refresh_guest_kernel_register() if rs == 26 else []
        return setup + [f"{name} {reg(rs)}, {label(target)}"]
    if name in float_branches:
        return [f"{name} {label(instruction[1])}"]
    if name in vfpu_branches:
        _, cc, target = instruction
        jump = "bne" if name in ("bvt", "bvtl") else "beq"
        return [
            "addiu $sp, $sp, -16",
            "sw $1, 0($sp)",
            "sw $26, 4($sp)",
            "lui $26, %hi(vfpu_ctrl + 12)",
            "lw $1, %lo(vfpu_ctrl + 12)($26)",
            "lw $26, 4($sp)",
            "addiu $sp, $sp, 16",
            f"andi $1, $1, {1 << cc}",
            f"{jump} $1, $0, {label(target)}",
            "lw $1, -16($sp)",
        ]

    if name in ("addi", "addiu", "slti", "sltiu", "andi", "ori", "xori"):
        _, rt, rs, value = instruction
        shown = str(value) if name in ("addi", "addiu", "slti", "sltiu") else f"0x{value:x}"
        return [f"{name} {reg(rt)}, {reg(rs)}, {shown}"]
    if name == "lui":
        return [f"lui {reg(instruction[1])}, 0x{instruction[2]:x}"]

    if name in ("lb", "lbu", "lh", "lhu", "lwl", "lwr", "lw", "sb", "sh", "swl", "swr", "sw"):
        _, rt, rs, offset = instruction
        setup = refresh_guest_kernel_register() if rs == 26 else []
        return setup + [f"{name} {reg(rt)}, {offset}({reg(rs)})"]
    if name in ("lwc1", "swc1"):
        _, ft, rs, offset = instruction
        return [f"{name} {freg(ft)}, {offset}({reg(rs)})"]

    if name in ("round.w.s", "ceil.w.s", "floor.w.s"):
        return emit_float_round(name, instruction[1], instruction[2], address)

    if name in ("mfc1", "mtc1", "cfc1", "ctc1"):
        _, rt, fs = instruction
        return [f"{name} {reg(rt)}, {freg(fs)}"]
    if name in ("add.s", "sub.s", "mul.s", "div.s"):
        _, fd, fs, ft = instruction
        return [f"{name} {freg(fd)}, {freg(fs)}, {freg(ft)}"]
    if name in (
        "mov.s", "trunc.w.s", "cvt.s.w", "sqrt.s",
        "abs.s", "neg.s",
    ):
        _, fd, fs = instruction
        return [f"{name} {freg(fd)}, {freg(fs)}"]
    if name in ("c.eq.s", "c.lt.s", "c.le.s"):
        _, fs, ft = instruction
        return [f"{name} {freg(fs)}, {freg(ft)}"]

    if name in ("seb", "seh"):
        _, destination, source = instruction
        if destination == 0:
            return ["nop"]
        shift = 24 if name == "seb" else 16
        return [
            f"sll {reg(destination)}, {reg(source)}, {shift}",
            f"sra {reg(destination)}, {reg(destination)}, {shift}",
        ]

    if name == "bitrev":
        _, destination, source = instruction
        if destination == 0:
            return ["nop"]

        temporaries = [index for index in (26, 27, 1, 25, 24, 23, 22, 21) if index not in (destination, source)][:3]
        current, shifted, mask_register = temporaries
        lines = [
            "addiu $sp, $sp, -16",
            f"sw {reg(current)}, 0($sp)",
            f"sw {reg(shifted)}, 4($sp)",
            f"sw {reg(mask_register)}, 8($sp)",
        ]

        if source == 29:
            lines.append(f"addiu {reg(current)}, $sp, 16")
        else:
            lines.append(f"or {reg(current)}, {reg(source)}, $0")

        for count, mask in (
            (1, 0x55555555),
            (2, 0x33333333),
            (4, 0x0f0f0f0f),
            (8, 0x00ff00ff),
            (16, 0x0000ffff),
        ):
            lines.extend(load_value(mask_register, mask))
            lines.extend([
                f"srl {reg(shifted)}, {reg(current)}, {count}",
                f"xor {reg(shifted)}, {reg(shifted)}, {reg(current)}",
                f"and {reg(shifted)}, {reg(shifted)}, {reg(mask_register)}",
                f"xor {reg(current)}, {reg(current)}, {reg(shifted)}",
                f"sll {reg(shifted)}, {reg(shifted)}, {count}",
                f"xor {reg(current)}, {reg(current)}, {reg(shifted)}",
            ])

        if destination == 29:
            lines.append(f"sw {reg(current)}, 12($sp)")
        else:
            lines.append(f"or {reg(destination)}, {reg(current)}, $0")

        lines.extend([
            f"lw {reg(current)}, 0($sp)",
            f"lw {reg(shifted)}, 4($sp)",
            f"lw {reg(mask_register)}, 8($sp)",
        ])
        if destination == 29:
            lines.append("lw $sp, 12($sp)")
        else:
            lines.append("addiu $sp, $sp, 16")
        return lines

    if name == "wsbw":
        _, rd, rt = instruction
        if rd == 0:
            return ["nop"]

        scratch = [item for item in (26, 27, 1, 25, 24, 23, 22, 21) if item not in (rd, rt)][:2]
        first, second = scratch
        return [
            f"mthi1 {reg(first)}",
            f"mtlo1 {reg(second)}",
            f"sll {reg(first)}, {reg(rt)}, 8",
            f"lui {reg(second)}, 0xff00",
            f"ori {reg(second)}, {reg(second)}, 0xff00",
            f"and {reg(first)}, {reg(first)}, {reg(second)}",
            f"srl {reg(rd)}, {reg(rt)}, 8",
            f"nor {reg(second)}, {reg(second)}, $0",
            f"and {reg(rd)}, {reg(rd)}, {reg(second)}",
            f"or {reg(rd)}, {reg(rd)}, {reg(first)}",
            f"sll {reg(first)}, {reg(rd)}, 16",
            f"srl {reg(rd)}, {reg(rd)}, 16",
            f"or {reg(rd)}, {reg(rd)}, {reg(first)}",
            f"mflo1 {reg(second)}",
            f"mfhi1 {reg(first)}",
        ]

    if name == "cache":
        return ["nop"]

    if name == "ext":
        _, rt, rs, pos, size = instruction
        left = 32 - pos - size
        right = 32 - size
        lines = []

        if left:
            lines.append(f"sll {reg(rt)}, {reg(rs)}, {left}")
        elif rt != rs:
            lines.append(f"or {reg(rt)}, {reg(rs)}, $0")

        if right:
            lines.append(f"srl {reg(rt)}, {reg(rt)}, {right}")

        return lines or ["nop"]

    if name == "ins":
        _, rt, rs, pos, size = instruction

        if rt == 0:
            return ["nop"]

        mask = ((1 << size) - 1) << pos if size < 32 else 0xffffffff
        scratch = [item for item in (26, 27, 1, 25, 24, 23, 22, 21) if item not in (rs, rt)][:3]
        first, second, original = scratch
        lines = ["addiu $sp, $sp, -16"]
        for index, register in enumerate(scratch):
            lines.append(f"sw {reg(register)}, {index * 4}($sp)")

        if rs == rt:
            lines.append(f"or {reg(original)}, {reg(rs)}, $0")
            source = original
        elif rs == 29:
            lines.append(f"addiu {reg(original)}, $sp, 16")
            source = original
        else:
            source = rs

        lines.extend(load_value(first, mask))
        lines.extend([
            f"nor {reg(second)}, {reg(first)}, $0",
            f"and {reg(rt)}, {reg(rt)}, {reg(second)}",
        ])

        if pos:
            lines.append(f"sll {reg(second)}, {reg(source)}, {pos}")
        else:
            lines.append(f"or {reg(second)}, {reg(source)}, $0")

        lines.extend([
            f"and {reg(second)}, {reg(second)}, {reg(first)}",
            f"or {reg(rt)}, {reg(rt)}, {reg(second)}",
        ])

        for index, register in enumerate(scratch):
            lines.append(f"lw {reg(register)}, {index * 4}($sp)")
        lines.append("addiu $sp, $sp, 16")
        return lines

    raise ValueError(f"não sei cuspir {name} no r5900")
