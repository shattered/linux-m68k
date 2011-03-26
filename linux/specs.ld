SECTIONS {
        .text 0x0000 : {
	    kernel_image_start = .;
            *(.init)
            *(.text)
            *(.fini)
	    etext = .;
	    _etext = .;
            . = ALIGN(0x1000);
        }
        .data ALIGN(0x1000) : {
            . = ALIGN(0x1000);
            built_rd_start = .;
            *(.rdsk)
            built_rd_end = .;
            . = ALIGN(0x1000);
            *(.data)
	    kernel_image_end = .;
	    edata = .;
	    _edata = .;
        }
        .bss SIZEOF(.data) + ADDR(.data) :  {
	    *(.bss)
	    *(COMMON)
	    end = ALIGN(0x8);
	    _end = ALIGN(0x8);
	}
}

kernel_bss_len = _end - _edata;
kernel_start = _start;

