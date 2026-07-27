CC := gcc
AS := nasm
LD := ld

CFLAGS := -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
          -Wall -Wextra -std=gnu11 -O2 -Ikernel
USER_CFLAGS := -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
               -Wall -Wextra -std=gnu11 -O2
ASFLAGS := -f elf32
LDFLAGS := -m elf_i386 -T kernel/linker.ld -nostdlib

BUILD := build
KERNEL_SRCS := $(wildcard kernel/*.c)
KERNEL_OBJS := $(patsubst kernel/%.c,$(BUILD)/%.o,$(KERNEL_SRCS))
ASM_OBJS := $(BUILD)/boot_asm.o $(BUILD)/isr_stubs.o $(BUILD)/context_switch.o $(BUILD)/usermode.o

USER_ELF := $(BUILD)/user_program.elf
USER_BLOB := $(BUILD)/user_blob.o

TARGET := $(BUILD)/myos.elf

.PHONY: all run clean

all: $(TARGET)

$(BUILD)/boot_asm.o: boot/boot.s | $(BUILD)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/isr_stubs.o: kernel/isr_stubs.s | $(BUILD)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/context_switch.o: kernel/context_switch.s | $(BUILD)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/usermode.o: kernel/usermode.s | $(BUILD)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/%.o: kernel/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# --- userspace program: compiled fully separately, then embedded ---

$(BUILD)/user_program.o: userland/user_program.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_ELF): $(BUILD)/user_program.o userland/user.ld
	$(LD) -m elf_i386 -T userland/user.ld -nostdlib -o $@ $(BUILD)/user_program.o

$(USER_BLOB): $(USER_ELF)
	cd $(BUILD) && $(LD) -m elf_i386 -r -b binary -o user_blob.o user_program.elf

$(BUILD)/fork_demo.o: userland/fork_demo.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/fork_demo.elf: $(BUILD)/fork_demo.o userland/user.ld
	$(LD) -m elf_i386 -T userland/user.ld -nostdlib -o $@ $(BUILD)/fork_demo.o

$(BUILD)/cow_test.o: userland/cow_test.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/cow_test.elf: $(BUILD)/cow_test.o userland/user.ld
	$(LD) -m elf_i386 -T userland/user.ld -nostdlib -o $@ $(BUILD)/cow_test.o

$(BUILD)/desktop_app.o: userland/desktop_app.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -Ikernel -c $< -o $@

$(BUILD)/desktop_font8x16.o: kernel/font8x16.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -Ikernel -c $< -o $@

$(BUILD)/desktop_cursor_bitmap.o: kernel/cursor_bitmap.c | $(BUILD)
	$(CC) $(USER_CFLAGS) -Ikernel -c $< -o $@

$(BUILD)/desktop_app.elf: $(BUILD)/desktop_app.o $(BUILD)/desktop_font8x16.o $(BUILD)/desktop_cursor_bitmap.o userland/user.ld
	$(LD) -m elf_i386 -T userland/user.ld -nostdlib -o $@ $(BUILD)/desktop_app.o $(BUILD)/desktop_font8x16.o $(BUILD)/desktop_cursor_bitmap.o

$(BUILD):
	mkdir -p $(BUILD)

$(TARGET): $(ASM_OBJS) $(KERNEL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(ASM_OBJS) $(KERNEL_OBJS)

diskimg/disk.img: build/user_program.elf build/fork_demo.elf build/desktop_app.elf build/cow_test.elf
	mkdir -p diskimg
	dd if=/dev/zero of=diskimg/disk.img bs=1M count=16
	mformat -i diskimg/disk.img -v KRONKEDOS ::
	echo "Hello from a real file on a real FAT filesystem, read by our own FAT16 driver!" > diskimg/hello.txt
	printf '10 LET A = 42\n20 LET B = 7\n30 PRINT "BEFORE:", A, B\n40 KRONK\n50 PRINT "AFTER:", A, B\n60 LET I = 1\n70 IF I > 5 THEN 110\n80 PRINT "COUNT IS", I\n90 LET I = I + 1\n100 GOTO 70\n110 PRINT "ALL DONE"\n120 END\n' > diskimg/demo.kro
	mcopy -i diskimg/disk.img diskimg/hello.txt ::/HELLO.TXT
	mcopy -i diskimg/disk.img diskimg/demo.kro ::/DEMO.KRO
	mcopy -i diskimg/disk.img build/user_program.elf ::/PROGRAM.ELF
	mcopy -i diskimg/disk.img build/fork_demo.elf ::/FORKDEMO.ELF
	mcopy -i diskimg/disk.img build/desktop_app.elf ::/DESKTOP.ELF
	mcopy -i diskimg/disk.img build/cow_test.elf ::/COWTEST.ELF

FAT16_LBA_OFFSET := 2048
KERNEL_SECTORS   := 200

$(BUILD)/stage1.bin: boot/stage1.s | $(BUILD)
	$(AS) -f bin $< -o $@

$(BUILD)/stage2.bin: boot/stage2.s | $(BUILD)
	$(AS) -f bin $< -o $@

$(BUILD)/myos_flat.bin: $(TARGET)
	objcopy -O binary $< $@

$(BUILD)/myos_flat_padded.bin: $(BUILD)/myos_flat.bin
	cp $< $@
	truncate -s $$(( $(KERNEL_SECTORS) * 512 )) $@

# Bootloader + kernel occupy the front of the disk, padded out to a
# clean 1MiB boundary; the FAT16 filesystem (built exactly as before)
# starts right after it. ata.c's FAT16_PARTITION_LBA_OFFSET must match.
$(BUILD)/bootdisk.img: $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(BUILD)/myos_flat_padded.bin
	cat $^ > $@
	truncate -s $$(( $(FAT16_LBA_OFFSET) * 512 )) $@

$(BUILD)/final_disk.img: $(BUILD)/bootdisk.img diskimg/disk.img
	cat $^ > $@

run-vga: $(BUILD)/final_disk.img
	qemu-system-x86_64 -drive file=$(BUILD)/final_disk.img,format=raw,if=ide,index=0 -serial stdio -no-reboot -no-shutdown -m 64


clean:
	rm -rf $(BUILD) diskimg
