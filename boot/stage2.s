; ###################################################################
; Stage 2 bootloader
; Loaded by Stage 1 at 0x7E00, real mode.
;
; Job: enable A20, gather a real memory map (INT 15h/E820) and pack
; it into the multiboot_info format the kernel's pmm_init()
; expects, load the kernel off disk, switch to 32-bit
; protected mode, and jump in with EAX/EBX set exactly the way
; GRUB/QEMU's multiboot loader used to set them.
; ###################################################################

BITS 16
ORG 0x7E00

KERNEL_STAGE_SEG equ 0x1000     ; staging buffer: linear 0x10000
KERNEL_LBA_START equ 9          ; LBA 0=stage1, 1-8=stage2, 9.. = kernel
KERNEL_SECTORS   equ 200        ; ensure this matches Makefile's padded kernel size
CHUNK_SECTORS    equ 32         ; sectors per INT13h call
CHUNK_SEG_STEP   equ (CHUNK_SECTORS*512)/16   ; paragraphs per chunk

MMAP_BUF equ 0x9000              ; multiboot-format mmap entries are here
MB_INFO  equ 0x9400               ; multiboot_info struct is here

start:
    mov [boot_drive], dl

    mov si, msg_stage2
    call print_string

    ; Fast A20 to avoid memory wrap-around
    in al, 0x92
    or al, 2
    out 0x92, al

    ; Build the multiboot-compatible memory map via INT 15h/E820
    call build_memory_map

    ; ---- Load the kernel from disk in chunks ----
    mov cx, KERNEL_SECTORS
    mov bx, KERNEL_STAGE_SEG      ; current destination segment
    mov dword [cur_lba], KERNEL_LBA_START

.load_chunk:
    mov ax, CHUNK_SECTORS
    cmp cx, ax
    jae .chunk_size_ok
    mov ax, cx                    ; last chunk: fewer than CHUNK_SECTORS left
.chunk_size_ok:
    push ax                       ; remember how many we're reading this round

    mov word [dap.count], ax
    mov word [dap.offset], 0
    mov [dap.segment], bx
    mov eax, [cur_lba]
    mov [dap.lba_lo], eax
    mov dword [dap.lba_hi], 0

    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    pop ax                        ; sectors just read
    sub cx, ax                    ; sectors remaining
    add word [cur_lba], ax
    add bx, CHUNK_SEG_STEP

    cmp cx, 0
    jne .load_chunk

    mov si, msg_kernel_loaded
    call print_string

    ; ---- Switch to protected mode ----
    cli
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp CODE_SEG:protected_mode_entry

disk_error:
    mov si, msg_disk_err
    call print_string
    jmp $

; #######################################################################
; Gather the real memory map (E820) and write it directly in
; multiboot_mmap_entry format: {size(4), addr(8), len(8), type(4)}.
; #######################################################################
build_memory_map:
    pusha                         ; save general-purpose registers to avoid clobbering the shit out of everything when calling
    mov edi, MMAP_BUF + 4
    xor ebx, ebx
    xor ebp, ebp                  ; entry count

.e820_loop:
    mov eax, 0xE820
    mov ecx, 20
    mov edx, 0x534D4150            ; 'SMAP'
    int 0x15
    jc .e820_done                   ; carry = unsupported or error (jc = jump if carry)
    cmp eax, 0x534D4150
    jne .e820_done

    mov dword [edi - 4], 20         ; backfill this entry's "size" field
    inc ebp
    add edi, 24
    cmp ebp, 32
    jae .e820_done
    test ebx, ebx
    jz .e820_done                   ; ebx=0 after success = that was the last one
    jmp .e820_loop

.e820_done:
    cmp ebp, 0
    jne .have_map

    ; Fallback for BIOSes without E820 support: one generous entry.
    ; pmm_init() re-reserves the first 1MiB and the kernel's own image
    ; regardless of what the map says, so a crude map here is safe.
    mov dword [MMAP_BUF + 0], 20
    mov dword [MMAP_BUF + 4], 0
    mov dword [MMAP_BUF + 8], 0
    mov dword [MMAP_BUF + 12], 0x4000000   ; 64 MiB
    mov dword [MMAP_BUF + 16], 0
    mov dword [MMAP_BUF + 20], 1            ; MULTIBOOT_MEMORY_AVAILABLE
    mov ebp, 1

.have_map:
    ; zero the multiboot_info struct, then fill in what pmm_init reads
    mov edi, MB_INFO
    mov ecx, 52
    xor eax, eax
    push edi
    cld
    rep stosb
    pop edi

    mov dword [MB_INFO + 0], 0x40   ; flags: bit6 = mmap present (informational)
    mov eax, ebp
    imul eax, eax, 24
    mov dword [MB_INFO + 44], eax   ; mmap_length
    mov dword [MB_INFO + 48], MMAP_BUF ; mmap_addr

    popa
    ret

print_string:
    lodsb
    or al, al
    jz .done
    call serial_putc
    jmp print_string
.done:
    ret

serial_putc:
    push dx
    push ax
.wait:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jz .wait
    pop ax
    mov dx, 0x3F8
    out dx, al
    pop dx
    ret

boot_drive: db 0x80
cur_lba:    dd 0
msg_stage2:        db "Stage2: A20 + E820 + loading kernel...", 13, 10, 0
msg_kernel_loaded: db "Stage2: kernel loaded, entering protected mode.", 13, 10, 0
msg_disk_err:      db "Stage2: disk read error!", 13, 10, 0

align 4
dap:
    .size:    db 0x10
    .reserved db 0
    .count:   dw 0
    .offset:  dw 0
    .segment: dw 0
    .lba_lo:  dd 0
    .lba_hi:  dd 0

align 8
gdt_start:
    dq 0
gdt_code:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10011010b
    db 11001111b
    db 0x00
gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b
    db 11001111b
    db 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

; ============================================================
; 32-bit protected mode
; ============================================================
BITS 32
protected_mode_entry:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    ; copy the kernel from the real-mode staging buffer to 0x100000
    mov esi, KERNEL_STAGE_SEG * 16
    mov edi, 0x100000
    mov ecx, (KERNEL_SECTORS * 512) / 4
    rep movsd

    ; hand off exactly the way a multiboot loader would:
    ; EAX = magic, EBX = multiboot_info pointer
    mov eax, 0x2BADB002
    mov ebx, MB_INFO
    jmp 0x100000

times (8*512)-($-$$) db 0
