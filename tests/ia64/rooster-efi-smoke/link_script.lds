OUTPUT_FORMAT("elf64-ia64-little")
OUTPUT_ARCH(ia64)
ENTRY(_start_plabel)

PHDRS
{
    text    PT_LOAD    FLAGS(0x05);
    rodata  PT_LOAD    FLAGS(0x04);
    data    PT_LOAD    FLAGS(0x06);
    dynamic PT_DYNAMIC FLAGS(0x06);
}

SECTIONS
{
    . = 0;
    __slide = .;
    __image_base = ABSOLUTE(.);
    __image_size = ABSOLUTE(__image_end - __image_base);

    .text : {
        KEEP(*(.pe_header))
        . = ALIGN(0x1000);
        __text_start = ABSOLUTE(.);
        *(.text .text.*)
        *(.plt)
    } :text

    . = ALIGN(0x1000);
    __text_end = ABSOLUTE(.);
    __text_size = ABSOLUTE(__text_end - __text_start);

    .rodata : {
        __reloc_start = ABSOLUTE(.);
        KEEP(*(.dummy_reloc))
        . = ALIGN(0x1000);
        __reloc_end = ABSOLUTE(.);
        __reloc_size = ABSOLUTE(__reloc_end - __reloc_start);
        __data_start = ABSOLUTE(.);
        *(.rodata .rodata.*)
    } :rodata

    __gp = ALIGN(8) + 0x200000;

    .dynamic : {
        _DYNAMIC = .;
        *(.dynamic)
    } :data :dynamic

    .data : {
        *(.got.plt) *(.got)
        *(.srodata) *(.sdata) *(.sbss) *(.scommon)
        *(.data .data.*)
    } :data

    . = ALIGN(0x1000);
    .plabel : {
        KEEP(*(.plabel))
    } :data

    .rela : {
        *(.rela.text*) *(.rela.sdata*) *(.rela.data*)
        *(.rela.got) *(.rela.dyn) *(.rela.plt)
    } :data

    __data_rawend = ABSOLUTE(ALIGN(__bss_start, 0x1000));
    __data_rawsize = ABSOLUTE(__data_rawend - __data_start);

    .bss : {
        __bss_start = ABSOLUTE(.);
        *(.bss .bss.*)
        *(COMMON)
    } :data

    ASSERT(ALIGNOF(.bss) <= 0x1000, ".bss alignment exceeds 4096")

    __bss_end = ABSOLUTE(ALIGN(0x1000));
    __bss_size = ABSOLUTE(__bss_end - __bss_start);
    __data_end = ABSOLUTE(ALIGN(0x1000));
    __data_size = ABSOLUTE(__data_end - __data_start);
    __image_end = ABSOLUTE(ALIGN(0x1000));

    /DISCARD/ : {
        *(.rela.plabel)
        *(.rela.dummy_reloc)
        *(.IA_64.unwind*)
        *(.IA64.unwind*)
        *(.eh_frame*)
        *(.note .note.*)
        *(.interp)
    }
}
