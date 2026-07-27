; ##################################################################################
; Stage 1 Bootloader (Master Boot Record)
; BIOS loads this at 0x7C00 and jumps to it in 16-bit real mode.
; Jon: load Stage 2 off disk, jump to it. Thats all we can cram into 512 bytes lol.
; ##################################################################################

BITS 16
ORG 0x7C00

start:
    cli                     ; no interrupts until we control the machine
    xor ax, ax              ; set ax to 0 using xor, then set all segments registers to zero to flatten memory
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00          ; stack grows down from here; nothing below us yet

    mov [boot_drive], dl    ; BIOS passes the boot drive number in DL at entry, so
                            ; we save it immediately before any call clobbers it

    mov si, msg_hello
    call print_string

    mov si, msg_boot
    call print_string

    ; ---- load Stage 2: 4 sectors from LBA 1, to 0x0000:0x7E00 ----
    mov word [dap.count], 4             ; number of sectors
    mov word [dap.offset], 0x7E00       ; destination offset
    mov word [dap.segment], 0x0000      ; destination segment
    mov dword [dap.lba_lo], 1           ; start reading at sector immediately after boot sector (set smaller part of 32-bit series to 1)
    mov dword [dap.lba_hi], 0           ; DAP stores LBA as 64-bit number consisting of two 32-bit series, set larger part to 0

    mov si, dap
    mov ah, 0x42            ; INT 13h extended read
    mov dl, [boot_drive]
    int 0x13
    jc disk_error           ; jump to disk error if carry flag is set (which BIOS does to indicate failure) after 0x13 interupt

    jmp 0x0000:0x7E00       ; hand off to Stage 2

disk_error:
    mov si, msg_disk_err
    call print_string       ; print error message 
    jmp $                   ; jump to current address, in effect repeatedly printing (hanging forever)

; ---- serial (COM1) string printer ----
print_string:
    lodsb
    or al, al
    jz .done                ; if al was 0, stop (strings end with 0, so we effectively stop when string ends)
    call serial_putc        ; given that character was not 0, send
    jmp print_string        ; repeat function
.done:
    ret

serial_putc:                ; AL = char to send
    push dx
    push ax
.wait:
    mov dx, 0x3FD            ; COM1 line status register
    in al, dx
    test al, 0x20             ; transmitter holding register empty?
    jz .wait
    pop ax
    mov dx, 0x3F8             ; COM1 data register
    out dx, al
    pop dx
    ret

boot_drive: db 0
msg_hello:    db "Hello, World!", 13, 10, 0
msg_boot:     db "Stage1: loading stage2...", 13, 10, 0
msg_disk_err: db "Stage1: disk read error!", 13, 10, 0

; ---- Disk Address Packet for INT 13h/AH=42h ----
align 4
dap:
    .size:    db 0x10
    .reserved db 0
    .count:   dw 0
    .offset:  dw 0
    .segment: dw 0
    .lba_lo:  dd 0
    .lba_hi:  dd 0

times 510-($-$$) db 0       ; pad to 510 bytes, then add boot signature as remaining two bytes
dw 0xAA55                   ; boot signature. BIOS refuses to boot without this