/* Warp Shaders plugin layout: three page-aligned segments (code, read-only, read-write).
 * SaltyNX's loader writes each PT_LOAD at heap + p_vaddr and sets its permissions, and its
 * dynamic linker counts symbols as (dynstr - dynsym), so .dynstr must follow .dynsym. */
OUTPUT_ARCH(aarch64)
ENTRY(_start)

PHDRS
{
    code   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
    dyn    PT_DYNAMIC;
}

SECTIONS
{
    . = 0;
    .crt0 : { KEEP(*(.crt0)) . = ALIGN(8); } :code
    .text : { *(.text .text.*) . = ALIGN(8); } :code
    .plt  : { *(.plt .plt.*) } :code

    . = ALIGN(0x1000);
    .dynsym  : { *(.dynsym) } :rodata
    .dynstr  : { *(.dynstr) } :rodata
    .hash    : { *(.hash) } :rodata
    .gnu.hash : { *(.gnu.hash) } :rodata
    .rela.dyn : { *(.rela.*) } :rodata
    .rodata  : { *(.rodata .rodata.*) . = ALIGN(8); } :rodata
    .note.gnu.build-id : { *(.note.gnu.build-id) } :rodata
    .eh_frame_hdr : { *(.eh_frame_hdr) } :rodata
    .eh_frame : { KEEP(*(.eh_frame)) } :rodata

    . = ALIGN(0x1000);
    .dynamic : { *(.dynamic) } :data :dyn
    .got     : { *(.got) *(.got.plt) *(.igot.plt) } :data
    .data    : { *(.data .data.*) . = ALIGN(8); } :data
    .bss     : { __bss_start__ = .; *(.bss .bss.*) *(COMMON) . = ALIGN(16); __bss_end__ = .; } :data

    /DISCARD/ : { *(.interp) *(.comment) *(.note.GNU-stack) }
}
