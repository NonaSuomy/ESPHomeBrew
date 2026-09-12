/*
 * PAPP linker script for C++ apps.
 *
 * Same layout as RetroESP32-P4's tools/psram_app.ld (flat binary linked at the
 * PSRAM exec window 0x4A000000, entry point first, .bss after the image), plus
 * the constructor tables: a C++ app has global objects whose constructors must
 * run before main(). papp_main.cpp walks __init_array_start..__init_array_end.
 */

OUTPUT_FORMAT("elf32-littleriscv")
OUTPUT_ARCH(riscv)
ENTRY(app_entry)

SECTIONS
{
    . = 0x4A000000;

    .text : {
        KEEP(*(.text.entry))
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    }

    . = ALIGN(4);
    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        __init_array_end = .;
    }

    .fini_array : {
        __fini_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        __fini_array_end = .;
    }

    _text_end = .;

    . = ALIGN(16);
    _data_start = .;
    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
        *(.gcc_except_table .gcc_except_table.*)
    }
    _data_end = .;

    . = ALIGN(16);
    _bss_start = .;
    .bss (NOLOAD) : {
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
    }
    _bss_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.riscv.attributes)
        *(.debug*)
    }
}
