MEMORY
{
 FLASH (rx) : ORIGIN = 0x08004800, LENGTH = 14K
 RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 8K
}
_estack = ORIGIN(RAM) + LENGTH(RAM);
_min_stack_size = 1K;
ENTRY(Reset_Handler)
SECTIONS
{
 .isr_vector ORIGIN(FLASH) :
 {
  . = ALIGN(4);
  KEEP(*(.isr_vector))
  . = ALIGN(4);
 } > FLASH
 .image_header ORIGIN(FLASH) + 0x100 :
 {
  KEEP(*(.image_header))
 } > FLASH
 .text :
 {
  . = ALIGN(4);
  *(.text)
  *(.text*)
  *(.glue_7)
  *(.glue_7t)
  *(.eh_frame)
  . = ALIGN(4);
 } > FLASH
 .rodata :
 {
  . = ALIGN(4);
  *(.rodata)
  *(.rodata*)
  . = ALIGN(4);
 } > FLASH
 .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
 .ARM : {
  __exidx_start = .;
  *(.ARM.exidx*)
  __exidx_end = .;
 } > FLASH
 _siramfunc = LOADADDR(.ramfunc);
 .ramfunc :
 {
  . = ALIGN(4);
  _sramfunc = .;
  *(.ramfunc)
  *(.ramfunc*)
  . = ALIGN(4);
  _eramfunc = .;
 } > RAM AT > FLASH
 _sidata = LOADADDR(.data);
 .data :
 {
  . = ALIGN(4);
  _sdata = .;
  *(.data)
  *(.data*)
  . = ALIGN(4);
  _edata = .;
 } > RAM AT > FLASH
 .bss :
 {
  . = ALIGN(4);
  _sbss = .;
  __bss_start__ = _sbss;
  *(.bss)
  *(.bss*)
  *(COMMON)
  . = ALIGN(4);
  _ebss = .;
  __bss_end__ = _ebss;
 } > RAM
 ._user_heap_stack :
 {
  . = ALIGN(8);
  . = . + _min_stack_size;
  . = ALIGN(8);
 } > RAM
 /DISCARD/ :
 {
  libm.a ( * )
  libgcc.a ( * )
 }
 .ARM.attributes 0 : { *(.ARM.attributes) }
}
