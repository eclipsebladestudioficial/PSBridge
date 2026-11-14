from vfpu_decode import identify_vfpu

registers = (
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
)

two_reg_branches = ("beq", "bne", "beql", "bnel")
one_reg_branches = (
    "bltz", "bgez", "bltzl", "bgezl",
    "bltzal", "bgezal", "bltzall", "bgezall",
    "blez", "bgtz", "blezl", "bgtzl",
)
float_branches = ("bc1f", "bc1t", "bc1fl", "bc1tl")
vfpu_branches = ("bvf", "bvt", "bvfl", "bvtl")
branch_names = two_reg_branches + one_reg_branches + float_branches + vfpu_branches


def reg(index):
    return f"${registers[index]}"


def freg(index):
    return f"$f{index}"


def signed(value):
    return value - 0x10000 if value & 0x8000 else value


def branch_target(address, value):
    return (address + 4 + (signed(value) << 2)) & 0xffffffff


def decode(address, word):
    op = word >> 26
    rs = (word >> 21) & 31
    rt = (word >> 16) & 31
    rd = (word >> 11) & 31
    shift = (word >> 6) & 31
    funct = word & 63
    value = word & 0xffff

    if word == 0:
        return "nop",

    if op == 0:
        if funct == 0:
            return "sll", rd, rt, shift
        if funct == 2:
            if rs == 1:
                return "rotr", rd, rt, shift
            return "srl", rd, rt, shift
        if funct == 3:
            return "sra", rd, rt, shift
        if funct in (4, 6, 7):
            if funct == 6 and shift == 1:
                return "rotrv", rd, rt, rs
            return {4: "sllv", 6: "srlv", 7: "srav"}[funct], rd, rt, rs

        if funct == 8:
            return "jr", rs
        if funct == 9:
            return "jalr", rd, rs

        if funct in (10, 11):
            return ("movz" if funct == 10 else "movn"), rd, rs, rt

        if funct in (16, 18):
            return ("mfhi" if funct == 16 else "mflo"), rd
        if funct in (17, 19):
            return ("mthi" if funct == 17 else "mtlo"), rs
        if funct == 0x16:
            return "clz", rd, rs

        if funct in (24, 25, 26, 27, 28, 29, 46, 47):
            name = {
                24: "mult", 25: "multu", 26: "div", 27: "divu",
                28: "madd", 29: "maddu", 46: "msub", 47: "msubu",
            }[funct]
            return name, rs, rt, word

        if funct == 13:
            return "break", (word >> 6) & 0xfffff

        if funct in (0x21, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2a, 0x2b, 0x2c, 0x2d):
            name = {
                0x21: "addu", 0x23: "subu", 0x24: "and", 0x25: "or",
                0x26: "xor", 0x27: "nor", 0x2a: "slt", 0x2b: "sltu",
                0x2c: "max", 0x2d: "min",
            }[funct]
            return name, rd, rs, rt

    if op in (2, 3):
        target = ((address + 4) & 0xf0000000) | ((word & 0x03ffffff) << 2)
        return ("j" if op == 2 else "jal"), target

    if op == 1 and rt in (0, 1, 2, 3, 16, 17, 18, 19):
        name = {
            0: "bltz", 1: "bgez", 2: "bltzl", 3: "bgezl",
            16: "bltzal", 17: "bgezal",
            18: "bltzall", 19: "bgezall",
        }[rt]
        return name, rs, branch_target(address, value)

    if op in (4, 5, 20, 21):
        name = {4: "beq", 5: "bne", 20: "beql", 21: "bnel"}[op]
        return name, rs, rt, branch_target(address, value)

    if op in (6, 7, 22, 23):
        name = {6: "blez", 7: "bgtz", 22: "blezl", 23: "bgtzl"}[op]
        return name, rs, branch_target(address, value)

    if op in (8, 9, 10, 11):
        return {8: "addi", 9: "addiu", 10: "slti", 11: "sltiu"}[op], rt, rs, signed(value)
    if op in (12, 13, 14):
        return {12: "andi", 13: "ori", 14: "xori"}[op], rt, rs, value
    if op == 15:
        return "lui", rt, value

    if op == 0x11:
        fmt = rs
        fs = rd
        fd = shift

        if fmt == 0:
            return "mfc1", rt, fs
        if fmt == 4:
            return "mtc1", rt, fs
        if fmt == 2:
            return "cfc1", rt, fs
        if fmt == 6:
            return "ctc1", rt, fs

        if fmt == 8 and (rt & 3) in (0, 1, 2, 3):
            name = ("bc1f", "bc1t", "bc1fl", "bc1tl")[rt & 3]
            return name, branch_target(address, value)

        if fmt == 16:
            if funct in (0, 1, 2, 3):
                return ("add.s", "sub.s", "mul.s", "div.s")[funct], fd, fs, rt
            if funct in (4, 5, 6, 7, 15):
                float_operations = {
                    4: "sqrt.s",
                    5: "abs.s",
                    6: "mov.s",
                    7: "neg.s",
                    15: "floor.w.s",
                }
                return float_operations[funct], fd, fs
            if funct in (0x0c, 0x0d, 0x0e):
                return {
                    0x0c: "round.w.s",
                    0x0d: "trunc.w.s",
                    0x0e: "ceil.w.s",
                }[funct], fd, fs
            if funct == 0x32:
                return "c.eq.s", fs, rt
            if funct in (0x3c, 0x3d):
                return "c.lt.s", fs, rt
            if funct in (0x3e, 0x3f):
                return "c.le.s", fs, rt

        if fmt == 20 and funct == 0x20:
            return "cvt.s.w", fd, fs

    memory = {
        32: "lb", 33: "lh", 34: "lwl", 35: "lw", 36: "lbu", 37: "lhu",
        38: "lwr", 40: "sb", 41: "sh", 42: "swl", 43: "sw", 46: "swr",
    }
    if op in memory:
        return memory[op], rt, rs, signed(value)

    if op in (49, 57):
        return ("lwc1" if op == 49 else "swc1"), rt, rs, signed(value)

    if op == 0x2f:
        return "cache", rt, rs, signed(value)

    if op == 0x1f and funct == 0x20 and rs == 0:
        if shift == 3:
            return "wsbw", rd, rt
        if shift == 20:
            return "bitrev", rd, rt
        if shift == 16:
            return "seb", rd, rt
        if shift == 24:
            return "seh", rd, rt

    if op == 0x1f and funct == 0:
        return "ext", rt, rs, shift, rd + 1
    if op == 0x1f and funct == 4:
        return "ins", rt, rs, shift, rd - shift + 1

    if op == 0x12 and rs == 8 and (rt & 3) in (0, 1, 2, 3) and (rt & 0xe0) == 0:
        return vfpu_branches[rt & 3], (word >> 18) & 7, branch_target(address, value)

    vfpu_name = identify_vfpu(word)
    if vfpu_name is not None:
        return "vfpu_call", word, vfpu_name
    return "word", word


def target_of(instruction):
    name = instruction[0]

    if name in ("j", "jal") + float_branches:
        return instruction[1]
    if name in one_reg_branches:
        return instruction[2]
    if name in vfpu_branches:
        return instruction[2]
    if name in two_reg_branches:
        return instruction[3]

    return None


def format_instruction(instruction):
    name = instruction[0]

    if name in ("nop", "cache"):
        return name

    if name in ("sll", "srl", "sra", "rotr"):
        _, rd, rt, shift = instruction
        return f"{name} {reg(rd)}, {reg(rt)}, {shift}"
    if name in ("sllv", "srlv", "srav", "rotrv"):
        _, rd, rt, rs = instruction
        return f"{name} {reg(rd)}, {reg(rt)}, {reg(rs)}"

    if name == "jr":
        return f"jr {reg(instruction[1])}"
    if name == "jalr":
        return f"jalr {reg(instruction[1])}, {reg(instruction[2])}"

    if name in ("addu", "subu", "and", "or", "xor", "nor", "slt", "sltu", "movz", "movn", "min", "max"):
        _, rd, rs, rt = instruction
        return f"{name} {reg(rd)}, {reg(rs)}, {reg(rt)}"

    if name in ("mfhi", "mflo", "mthi", "mtlo"):
        return f"{name} {reg(instruction[1])}"
    if name in ("mult", "multu", "div", "divu", "madd", "maddu", "msub", "msubu"):
        return f"{name} {reg(instruction[1])}, {reg(instruction[2])}"

    if name == "clz":
        return f"clz {reg(instruction[1])}, {reg(instruction[2])}"
    if name == "break":
        return f"break {instruction[1]}"

    if name in ("j", "jal"):
        return f"{name} 0x{instruction[1]:08x}"

    if name in two_reg_branches:
        _, rs, rt, target = instruction
        return f"{name} {reg(rs)}, {reg(rt)}, 0x{target:08x}"
    if name in one_reg_branches:
        _, rs, target = instruction
        return f"{name} {reg(rs)}, 0x{target:08x}"
    if name in vfpu_branches:
        _, cc, target = instruction
        return f"{name} {cc}, 0x{target:08x}"
    if name in float_branches:
        return f"{name} 0x{instruction[1]:08x}"

    if name in ("addi", "addiu", "slti", "sltiu", "andi", "ori", "xori"):
        _, rt, rs, value = instruction
        shown = str(value) if name in ("addi", "addiu", "slti", "sltiu") else f"0x{value:x}"
        return f"{name} {reg(rt)}, {reg(rs)}, {shown}"

    if name == "lui":
        return f"lui {reg(instruction[1])}, 0x{instruction[2]:x}"

    if name in ("lb", "lbu", "lh", "lhu", "lwl", "lwr", "lw", "sb", "sh", "swl", "swr", "sw"):
        _, rt, rs, offset = instruction
        return f"{name} {reg(rt)}, {offset}({reg(rs)})"
    if name in ("lwc1", "swc1"):
        _, ft, rs, offset = instruction
        return f"{name} {freg(ft)}, {offset}({reg(rs)})"

    if name in ("mfc1", "mtc1", "cfc1", "ctc1"):
        _, rt, fs = instruction
        return f"{name} {reg(rt)}, {freg(fs)}"
    if name in ("add.s", "sub.s", "mul.s", "div.s"):
        _, fd, fs, ft = instruction
        return f"{name} {freg(fd)}, {freg(fs)}, {freg(ft)}"
    if name in (
        "mov.s", "trunc.w.s", "cvt.s.w", "sqrt.s",
        "abs.s", "neg.s", "floor.w.s", "round.w.s", "ceil.w.s",
    ):
        _, fd, fs = instruction
        return f"{name} {freg(fd)}, {freg(fs)}"
    if name in ("c.eq.s", "c.lt.s", "c.le.s"):
        _, fs, ft = instruction
        return f"{name} {freg(fs)}, {freg(ft)}"

    if name in ("wsbw", "bitrev", "seb", "seh"):
        return f"{name} {reg(instruction[1])}, {reg(instruction[2])}"

    if name in ("ext", "ins"):
        _, rt, rs, pos, size = instruction
        return f"{name} {reg(rt)}, {reg(rs)}, {pos}, {size}"

    return f".word 0x{instruction[1]:08x}"
