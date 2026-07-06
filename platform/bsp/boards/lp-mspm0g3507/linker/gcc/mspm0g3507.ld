/*****************************************************************************
 * MSPM0G3507 GCC linker script
 *
 * Symbols match startup_mspm0g350x_gcc.c (TI MSPM0 SDK).
 * Memory layout from CCS device_linker.cmd (SysConfig generated).
 *****************************************************************************/

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 128K
    SRAM  (rwx) : ORIGIN = 0x20200000, LENGTH =  32K
}

__StackTop = ORIGIN(SRAM) + LENGTH(SRAM);
end = __StackTop;

SECTIONS
{
    .intvecs :
    {
        KEEP(*(.intvecs))
    } > FLASH

    .text :
    {
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        KEEP(*(.init))
        KEEP(*(.fini))
    } > FLASH

    .rodata : { *(.rodata*) } > FLASH

    .ARM.extab   : { *(.ARM.extab*) } > FLASH
    .ARM.exidx   : { *(.ARM.exidx*) } > FLASH

    .preinit_array :
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > FLASH

    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > FLASH

    .fini_array :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(.fini_array*))
        KEEP(*(SORT(.fini_array.*)))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > FLASH

    /* initialized data: FLASH LMA, SRAM VMA */
    __data_load__ = LOADADDR(.data);

    .data :
    {
        __data_start__ = .;
        *(.data*)
        __data_end__ = .;
    } > SRAM AT > FLASH

    /* RAM functions (TI .TI.ramfunc equivalent) */
    .ramfunc :
    {
        __ramfunct_load__  = LOADADDR(.ramfunc);
        __ramfunct_start__ = .;
        *(.ramfunc*)
        *(.TI.ramfunc*)
        __ramfunct_end__   = .;
    } > SRAM AT > FLASH

    /* BSS */
    .bss :
    {
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        __bss_end__ = .;
    } > SRAM

    /* stack (minimum 512B per CCS .cmd, 2KB here) */
    .stack (NOLOAD) :
    {
        . = ALIGN(8);
        . = . + 0x800;
    } > SRAM
}
