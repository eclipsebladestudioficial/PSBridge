ENTRY(__start);

PHDRS {
  text PT_LOAD;
}

SECTIONS {
	.text 0x80100000 : AT(0x00100000) {
		_ftext = . ;
		*(.text)
		*(.text.*)
		*(.gnu.linkonce.t*)
		KEEP(*(.init))
		KEEP(*(.fini))
		QUAD(0)
	} :text

	PROVIDE(_etext = .);
	PROVIDE(etext = .);

	
	.ctors ALIGN(16) : AT(ADDR(.ctors) - 0x80000000) {
		KEEP(*crtbegin*.o(.ctors))
		KEEP(*(EXCLUDE_FILE(*crtend*.o) .ctors))
		KEEP(*(SORT(.ctors.*)))
		KEEP(*(.ctors))
	}
	.dtors ALIGN(16) : AT(ADDR(.dtors) - 0x80000000) {
		KEEP(*crtbegin*.o(.dtors))
		KEEP(*(EXCLUDE_FILE(*crtend*.o) .dtors))
		KEEP(*(SORT(.dtors.*)))
		KEEP(*(.dtors))
	}

	.reginfo : AT(ADDR(.reginfo) - 0x80000000) { *(.reginfo) }

	.data ALIGN(128) : AT(ADDR(.data) - 0x80000000) {
		_fdata = . ;
		*(.data)
		*(.data.*)
		*(.gnu.linkonce.d*)
		SORT(CONSTRUCTORS)
	}

	
	.rodata ALIGN(128) : AT(ADDR(.rodata) - 0x80000000) {
		*(.rodata)
		*(.rodata.*)
		*(.gnu.linkonce.r*)
	}

	.rdata ALIGN(128) : AT(ADDR(.rdata) - 0x80000000) { *(.rdata) }
	.gcc_except_table ALIGN(128) : AT(ADDR(.gcc_except_table) - 0x80000000) { *(.gcc_except_table) }

	_gp = ALIGN(128) + 0x7ff0;
	.lit4 ALIGN(128) : AT(ADDR(.lit4) - 0x80000000) { *(.lit4) }
	.lit8 ALIGN(128) : AT(ADDR(.lit8) - 0x80000000) { *(.lit8) }

	.sdata ALIGN(128) : AT(ADDR(.sdata) - 0x80000000) {
		*(.sdata)
		*(.sdata.*)
		*(.gnu.linkonce.s*)
	}

	_edata = .;
	PROVIDE(edata = .);

    .guest_image ALIGN(0x100000) : {
        *(.guest_image)
    }

    .guest_bss ALIGN(4) (NOLOAD) : AT(ADDR(.guest_bss) - 0x80000000) {
        *(.guest_bss)
    } :text

	
	.sbss ALIGN(128) : AT(ADDR(.sbss) - 0x80000000) {
		_fbss = . ;
		*(.sbss)
		*(.sbss.*)
		*(.gnu.linkonce.sb*)
		*(.scommon)
	}

	.bss ALIGN(128) : AT(ADDR(.bss) - 0x80000000) {
		*(.bss)
		*(.bss.*)
		*(.gnu.linkonce.b*)
		*(COMMON)
	}
    .psbridge_heap (NOLOAD) : AT(ADDR(.psbridge_heap) - 0x80000000) {
        . = ALIGN(4096);
        __psbridge_heap_start = .;
        . += 4;
    } :text
    .psbridge_heap_limit 0x81f00000 (NOLOAD) : AT(ADDR(.psbridge_heap_limit) - 0x80000000) {
        __psbridge_heap_end = .;
        . += 4;
        _end = . - 0x80000000;
        _end_bss = .;
    } :text
    ASSERT(__psbridge_heap_end >= __psbridge_heap_start + 0x400000, "o ELF arregaçou a RAM do PS2")



	PROVIDE(end = _end);

	.spad 0x70000000: {
		*(.spad)
	}

	
	PROVIDE(_heap_size = -1);
	PROVIDE(_stack = -1);
	PROVIDE(_stack_size = 128 * 1024);
}
