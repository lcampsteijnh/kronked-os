; usermode.s -- the ring 0 -> ring 3 transition
;
; void enter_usermode(unsigned int entry, unsigned int user_esp);
;
; iret normally *returns* from an interrupt, restoring EIP/CS/EFLAGS
; (and ESP/SS, if the privilege level changes) from values the CPU
; pushed automatically when the interrupt fired. We exploit that: by
; pushing exactly those values ourselves -- with CS/SS selectors that
; have RPL=3 -- iret is tricked into "returning" into ring 3 code that
; never actually came from an interrupt. This is the standard technique
; for a kernel's very first jump into user mode.

global enter_usermode
enter_usermode:
    mov eax, [esp+4]    ; entry point (eip)
    mov ecx, [esp+8]    ; user stack pointer (esp)

    mov dx, 0x23         ; user data selector (GDT index 4, RPL 3)
    mov ds, dx
    mov es, dx
    mov fs, dx
    mov gs, dx            ; ss is loaded by iret itself, not here

    push dword 0x23        ; SS
    push ecx                ; ESP
    pushfd
    pop ebx
    or ebx, 0x200             ; force IF=1 (interupts enabled) so the usermode task can be preempted
    push ebx                   ; EFLAGS
    push dword 0x1B              ; CS (GDT index 3, RPL 3)
    push eax                      ; EIP
    iret
