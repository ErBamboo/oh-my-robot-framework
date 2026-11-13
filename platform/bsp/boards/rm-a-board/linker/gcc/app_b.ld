/*
 * STM32F427IIHx 链接脚本（app_b 槽）。
 * 槽布局：槽基址 0x08110000，链接基址 0x08110200（+0x200 镜像头）；地址与 om_bootcfg.h 的分区表同源。
 * 段布局沿用同目录 stm32f427iihx.ld。
 * Copyright (c) 2022 STMicroelectronics. All rights reserved.
 * 许可条款见 STM32Cube 包的 LICENSE 文件。
 */

/* Entry Point */
ENTRY(Reset_Handler)

/* Highest address of the user mode stack */
_estack = ORIGIN(RAM) + LENGTH(RAM); /* end of "RAM" Ram type memory */

_Min_Heap_Size = 0x200; /* required amount of heap */
_Min_Stack_Size = 0x400; /* required amount of stack */

/* Memories definition */
MEMORY
{
  CCMRAM    (xrw)    : ORIGIN = 0x10000000,   LENGTH = 64K
  RAM    (xrw)    : ORIGIN = 0x20000000,   LENGTH = 192K
  ROM    (rx)    : ORIGIN = 0x08110200,   LENGTH = 0xEFE00
}

/* Sections */
SECTIONS
{
  /* The startup code into "ROM" Rom type memory */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector)) /* Startup code */
    . = ALIGN(4);
  } >ROM

  /* OM init call tables — 每级一段，按级别顺序排列（级别顺序由链接器保证，见 core/om_init.h） */
  .om_init_0 :
  {
    . = ALIGN(4);
    PROVIDE(__om_init_0_start = .);
    KEEP(*(.om_init_0))
    PROVIDE(__om_init_0_end = .);
    . = ALIGN(4);
  } >ROM
  .om_init_1 :
  {
    . = ALIGN(4);
    PROVIDE(__om_init_1_start = .);
    KEEP(*(.om_init_1))
    PROVIDE(__om_init_1_end = .);
    . = ALIGN(4);
  } >ROM
  .om_init_2 :
  {
    . = ALIGN(4);
    PROVIDE(__om_init_2_start = .);
    KEEP(*(.om_init_2))
    PROVIDE(__om_init_2_end = .);
    . = ALIGN(4);
  } >ROM
  .om_init_3 :
  {
    . = ALIGN(4);
    PROVIDE(__om_init_3_start = .);
    KEEP(*(.om_init_3))
    PROVIDE(__om_init_3_end = .);
    . = ALIGN(4);
  } >ROM
  .om_init_4 :
  {
    . = ALIGN(4);
    PROVIDE(__om_init_4_start = .);
    KEEP(*(.om_init_4))
    PROVIDE(__om_init_4_end = .);
    . = ALIGN(4);
  } >ROM
  .om_init_5 :
  {
    . = ALIGN(4);
    PROVIDE(__om_init_5_start = .);
    KEEP(*(.om_init_5))
    PROVIDE(__om_init_5_end = .);
    . = ALIGN(4);
  } >ROM
  .om_init_6 :
  {
    . = ALIGN(4);
    PROVIDE(__om_init_6_start = .);
    KEEP(*(.om_init_6))
    PROVIDE(__om_init_6_end = .);
    . = ALIGN(4);
  } >ROM

  /* The program code and other data into "ROM" Rom type memory */
  .text :
  {
    . = ALIGN(4);
    *(.text)           /* .text sections (code) */
    *(.text*)          /* .text* sections (code) */
    *(.glue_7)         /* glue arm to thumb code */
    *(.glue_7t)        /* glue thumb to arm code */
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;        /* define a global symbols at end of code */
  } >ROM

  /* Constant data into "ROM" Rom type memory */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)         /* .rodata sections (constants, strings, etc.) */
    *(.rodata*)        /* .rodata* sections (constants, strings, etc.) */
    . = ALIGN(4);
  } >ROM

  .ARM.extab :
  {
    . = ALIGN(4);
    *(.ARM.extab* .gnu.linkonce.armextab.*)
    . = ALIGN(4);
  } >ROM

  .ARM :
  {
    . = ALIGN(4);
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
    . = ALIGN(4);
  } >ROM

  .preinit_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
    . = ALIGN(4);
  } >ROM

  .init_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
    . = ALIGN(4);
  } >ROM

  .fini_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
    . = ALIGN(4);
  } >ROM

  /* Used by the startup to initialize data */
  _sidata = LOADADDR(.data);

  /* Initialized data sections into "RAM" Ram type memory */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;        /* create a global symbol at data start */
    *(.data)           /* .data sections */
    *(.data*)          /* .data* sections */
    *(.RamFunc)        /* .RamFunc sections */
    *(.RamFunc*)       /* .RamFunc* sections */

    . = ALIGN(4);
    _edata = .;        /* define a global symbol at data end */

  } >RAM AT> ROM

  _siccmram = LOADADDR(.ccmram);

  /* CCM-RAM section
  *
  * IMPORTANT NOTE!
  * If initialized variables will be placed in this section,
  * the startup code needs to be modified to copy the init-values.
  */
  .ccmram :
  {
    . = ALIGN(4);
    _sccmram = .;       /* create a global symbol at ccmram start */
    *(.ccmram)
    *(.ccmram*)

    . = ALIGN(4);
    _eccmram = .;       /* create a global symbol at ccmram end */
  } >CCMRAM AT> ROM

  /* Uninitialized data section into "RAM" Ram type memory */
  . = ALIGN(4);
  .bss :
  {
    /* This is used by the startup in order to initialize the .bss section */
    _sbss = .;         /* define a global symbol at bss start */
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)

    . = ALIGN(4);
    _ebss = .;         /* define a global symbol at bss end */
    __bss_end__ = _ebss;
  } >RAM

  /* User_heap_stack section, used to check that there is enough "RAM" Ram  type memory left */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /* Remove information from the compiler libraries */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
