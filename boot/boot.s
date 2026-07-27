; boot.s -- kernel entry point
;
; stage2 bootloader (boot/stage2.s) loads a flat binary
; -- not the ELF, just raw section bytes -- to this same address
; (0x100000), and jumps here directly. It constructs an
; equivalent multiboot_info struct itself (via real-mode INT 15h/E820)
; and sets EAX/EBX before jumping. Here we just zero .bbs before
; any C code that assumes zero-initialized globals runs.

section .bss
align 16
stack_bottom:
    resb 16384                        ; 16 KiB kernel stack
stack_top:

section .text
global _start
extern kernel_main
extern _bss_start
extern _kernel_end

_start:
    mov esp, stack_top

    ; stash stage2's multiboot-style args before the bss-zeroing loop
    ; below clobbers eax/ecx/edi -- we still need them intact for the
    ; call to kernel_main further down
    mov edx, eax                      ; edx = multiboot magic
    mov esi, ebx                      ; esi = multiboot_info pointer

    ; zero .bss: our loader only copied real file bytes (.text/.rodata/
    ; .data) to 0x100000, not the bss region that follows it in memory,
    ; so every global here is currently garbage, not zero
    mov edi, _bss_start
    mov ecx, _kernel_end
    sub ecx, edi
    xor eax, eax
    cld
    rep stosb

    push esi                          ; mb_info_addr
    push edx                          ; magic

    cli                                ; interrupts are not set up yet, keep them off
    call kernel_main

    ; if kernel_main ever returns, halt forever
    cli
.hang:
    hlt
    jmp .hang
