.set noreorder
.set noat
.text

.globl psbridge_vfpu_entry
.ent psbridge_vfpu_entry

psbridge_vfpu_entry:
    addiu $sp, $sp, -384

    sw $1, 68($sp)
    sw $2, 72($sp)
    sw $3, 76($sp)
    sw $4, 80($sp)
    sw $5, 84($sp)
    sw $6, 88($sp)
    sw $7, 92($sp)
    sw $8, 96($sp)
    sw $9, 100($sp)
    sw $10, 104($sp)
    sw $11, 108($sp)
    sw $12, 112($sp)
    sw $13, 116($sp)
    sw $14, 120($sp)
    sw $15, 124($sp)
    sw $16, 128($sp)
    sw $17, 132($sp)
    sw $18, 136($sp)
    sw $19, 140($sp)
    sw $20, 144($sp)
    sw $21, 148($sp)
    sw $22, 152($sp)
    sw $23, 156($sp)
    lw $8, 384($sp)
    sw $8, 160($sp)
    lw $8, 388($sp)
    sw $8, 164($sp)
    sw $26, 168($sp)
    sw $27, 172($sp)
    sw $28, 176($sp)
    addiu $8, $sp, 416
    sw $8, 180($sp)
    sw $30, 184($sp)
    lw $8, 392($sp)
    sw $8, 188($sp)
    sw $31, 352($sp)
    sw $26, 344($sp)
    sw $27, 348($sp)

    swc1 $f0, 192($sp)
    swc1 $f1, 196($sp)
    swc1 $f2, 200($sp)
    swc1 $f3, 204($sp)
    swc1 $f4, 208($sp)
    swc1 $f5, 212($sp)
    swc1 $f6, 216($sp)
    swc1 $f7, 220($sp)
    swc1 $f8, 224($sp)
    swc1 $f9, 228($sp)
    swc1 $f10, 232($sp)
    swc1 $f11, 236($sp)
    swc1 $f12, 240($sp)
    swc1 $f13, 244($sp)
    swc1 $f14, 248($sp)
    swc1 $f15, 252($sp)
    swc1 $f16, 256($sp)
    swc1 $f17, 260($sp)
    swc1 $f18, 264($sp)
    swc1 $f19, 268($sp)
    swc1 $f20, 272($sp)
    swc1 $f21, 276($sp)
    swc1 $f22, 280($sp)
    swc1 $f23, 284($sp)
    swc1 $f24, 288($sp)
    swc1 $f25, 292($sp)
    swc1 $f26, 296($sp)
    swc1 $f27, 300($sp)
    swc1 $f28, 304($sp)
    swc1 $f29, 308($sp)
    swc1 $f30, 312($sp)
    swc1 $f31, 316($sp)

    mfhi $8
    sw $8, 320($sp)
    mflo $8
    sw $8, 324($sp)
    mfhi1 $8
    sw $8, 328($sp)
    mflo1 $8
    sw $8, 332($sp)
    cfc1 $8, $31
    sw $8, 336($sp)

    lw $4, 396($sp)
    lw $5, 400($sp)
    addiu $6, $sp, 64

    lui $gp, %hi(_gp)
    addiu $gp, $gp, %lo(_gp)

    jal psbridge_vfpu_dispatch
    nop

    lw $8, 336($sp)
    ctc1 $8, $31
    lw $8, 320($sp)
    mthi $8
    lw $8, 324($sp)
    mtlo $8
    lw $8, 328($sp)
    mthi1 $8
    lw $8, 332($sp)
    mtlo1 $8

    lwc1 $f0, 192($sp)
    lwc1 $f1, 196($sp)
    lwc1 $f2, 200($sp)
    lwc1 $f3, 204($sp)
    lwc1 $f4, 208($sp)
    lwc1 $f5, 212($sp)
    lwc1 $f6, 216($sp)
    lwc1 $f7, 220($sp)
    lwc1 $f8, 224($sp)
    lwc1 $f9, 228($sp)
    lwc1 $f10, 232($sp)
    lwc1 $f11, 236($sp)
    lwc1 $f12, 240($sp)
    lwc1 $f13, 244($sp)
    lwc1 $f14, 248($sp)
    lwc1 $f15, 252($sp)
    lwc1 $f16, 256($sp)
    lwc1 $f17, 260($sp)
    lwc1 $f18, 264($sp)
    lwc1 $f19, 268($sp)
    lwc1 $f20, 272($sp)
    lwc1 $f21, 276($sp)
    lwc1 $f22, 280($sp)
    lwc1 $f23, 284($sp)
    lwc1 $f24, 288($sp)
    lwc1 $f25, 292($sp)
    lwc1 $f26, 296($sp)
    lwc1 $f27, 300($sp)
    lwc1 $f28, 304($sp)
    lwc1 $f29, 308($sp)
    lwc1 $f30, 312($sp)
    lwc1 $f31, 316($sp)

    lw $1, 68($sp)
    lw $2, 72($sp)
    lw $3, 76($sp)
    lw $4, 80($sp)
    lw $5, 84($sp)
    lw $6, 88($sp)
    lw $7, 92($sp)
    lw $8, 96($sp)
    lw $9, 100($sp)
    lw $10, 104($sp)
    lw $11, 108($sp)
    lw $12, 112($sp)
    lw $13, 116($sp)
    lw $14, 120($sp)
    lw $15, 124($sp)
    lw $16, 128($sp)
    lw $17, 132($sp)
    lw $18, 136($sp)
    lw $19, 140($sp)
    lw $20, 144($sp)
    lw $21, 148($sp)
    lw $22, 152($sp)
    lw $23, 156($sp)
    lw $24, 160($sp)
    lw $25, 164($sp)
    lw $28, 176($sp)
    lw $30, 184($sp)
    lw $31, 188($sp)
    lw $27, 352($sp)
    addiu $sp, $sp, 384

    jr $27
    nop

.end psbridge_vfpu_entry
