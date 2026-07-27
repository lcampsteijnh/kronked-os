; isr_stubs.s low-level assembly glue for GDT and interrupts
;
; The CPU invokes interrupt handlers with no regard for our C calling
; convention, so each handler needs a tiny assembly stub that saves all
; registers, calls into C, restores registers, and properly returns with
; iret. This file provides that glue for the GDT flush, the 32 CPU
; exception vectors (ISRs 0-31), and the 16 PIC-remapped hardware IRQs
; (32-47).

global gdt_flush
gdt_flush:
    mov eax, [esp+4]      ; pointer to gdt_ptr struct, passed as arg
    lgdt [eax]            ; loads the Global Descriptor Table at this address (arg)

    mov ax, 0x10          ; kernel data segment selector (index 2 * 8)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.flush       ; kernel code segment (CS) selector (index 1 * 8)
.flush:
    ret

global idt_flush
idt_flush:
    mov eax, [esp+4]
    lidt [eax]
    ret

; ---- CPU exception stubs (0-31) ----
; Some exceptions push an error code automatically; others don't.
; We push a dummy 0 for the ones that don't, so the C-side struct layout
; is consistent for every vector.

%macro ISR_NOERR 1
global isr%1
isr%1:
    cli
    push dword 0        ; dummy error code
    push dword %1        ; interrupt number
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    cli
    push dword %1        ; interrupt number (error code already pushed by CPU)
    jmp isr_common_stub
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_NOERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; ---- Hardware IRQ stubs (remapped to 32-47) ----
%macro IRQ 2
global irq%1
irq%1:
    cli
    push dword 0
    push dword %2
    jmp irq_common_stub
%endmacro

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

extern isr_handler
extern irq_handler
extern syscall_handler

global isr128
isr128:
    cli
    push dword 0
    push dword 128
    jmp syscall_common_stub

syscall_common_stub:        ; push all rewgisters to save the complete CPU state, then 
    pusha
    mov ax, ds
    push eax

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp            ; pointer to the register struct, for a return value in eax
    call syscall_handler
    add esp, 4

; A forked child task's very first resume lands *here* directly (via
; context_switch's `ret`, never having gone through pusha/call above)
; with a byte-for-byte copy of its parent's syscall-entry stack frame
; already sitting where this code expects it. From the CPU's
; perspective this is indistinguishable from any other syscall
; returning normally -- it just happens that nobody called
; syscall_handler this time, because eax (and everything else) was
; already set up in advance by task_create_forked() in task.c.
global syscall_return_tail
syscall_return_tail:
    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popa
    add esp, 8
    sti
    iret

isr_common_stub:
    pusha
    mov ax, ds
    push eax

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call isr_handler

    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popa
    add esp, 8
    sti
    iret

irq_common_stub:
    pusha
    mov ax, ds
    push eax

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call irq_handler

    pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    popa
    add esp, 8
    sti
    iret
