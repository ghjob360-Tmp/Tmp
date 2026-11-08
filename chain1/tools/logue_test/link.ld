ENTRY(_start)
SECTIONS
{
  . = 0x20000000;
  .text : { *(.text*) *(.rodata*) *(.glue_7*) }
  . = ALIGN(8);
  .data : { *(.data*) }
  . = ALIGN(8);
  .bss : { __bss_start = .; *(.bss*) *(COMMON) __bss_end = .; }
  . = ALIGN(8);
  end = .;
  _end = .;
  /DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}
