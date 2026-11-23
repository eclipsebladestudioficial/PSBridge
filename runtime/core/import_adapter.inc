    .macro psbridge_import_call target, float_result
    addiu $sp, $sp, -64
    sd $ra, 56($sp)
    sd $26, 48($sp)
    sd $27, 40($sp)
    sd $gp, 32($sp)

    or $10, $ra, $0
    lui $gp, %hi(_gp)
    jal \target
    addiu $gp, $gp, %lo(_gp)

    .if \float_result
    mtc1 $2, $f0
    .endif

    j psbridge_import_return
    dsrl32 $3, $2, 0
    .endm
