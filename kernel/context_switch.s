; context_switch.s -- the actual stack-pointer swap that makes multitasking real
;
; void context_switch(unsigned int *old_esp_store, unsigned int new_esp)
;
; Saves the callee-saved registers (which the C caller expects preserved
; across any function call, per cdecl) onto the CURRENT stack, records
; where that leaves esp into *old_esp_store, then loads new_esp into esp
; and pops what's sitting there as though it were OUR callee-saved
; registers, and returns.
;
; The trick that makes this a task switch rather than just weird stack
; math: whatever is sitting at new_esp was either (a) put there by a
; previous call to this exact function when that task was switched away
; from, so popping+ret resumes it exactly where it left off, or (b) put
; there deliberately by task_create() to look like case (a), so a brand
; new task starts running its entry point instead.

global context_switch
context_switch:
    push ebp
    push edi
    push esi
    push ebx

    mov eax, [esp+20]      ; old_esp_store (4 args-worth of pushes = 16, +4 retaddr)
    mov [eax], esp

    mov esp, [esp+24]      ; new_esp

    pop ebx
    pop esi
    pop edi
    pop ebp
    ret
