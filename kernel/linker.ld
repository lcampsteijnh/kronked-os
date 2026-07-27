/* linker.ld -- place the kernel at the 1MiB mark, standard for multiboot */
ENTRY(_start)

SECTIONS
{
    . = 1M;
    _kernel_start = .;

    .multiboot : {
        *(.multiboot)
    }

    .text : {
        *(.text)
    }

    .rodata : {
        *(.rodata)
    }

    .data : {
        *(.data)
    }

    _bss_start = .;
    .bss : {
        *(COMMON)
        *(.bss)
    }

    _kernel_end = .;
}
