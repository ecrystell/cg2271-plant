ENTRY(run_tests)
MEMORY {
 FLASH (rx) : ORIGIN = 0x10000, LENGTH = 128K
 RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}
SECTIONS {
 .text : { *(.text*) *(.rodata*) } > FLASH
 .ARM.exidx : { *(.ARM.exidx*) } > FLASH
 .data : { *(.data*) } > RAM AT> FLASH
 .bss (NOLOAD) : { *(.bss*) *(COMMON) } > RAM
}
