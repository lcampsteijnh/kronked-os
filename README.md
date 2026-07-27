# kronked-os

A 32-bit x86 operating system built from scratch in C and assembly — no
existing kernel, bootloader, or OS-dev framework as a starting point.
Built as a project to understand every layer between power-on and a
usable desktop, one subsystem at a time.

## Features

- **Custom two-stage bootloader** — real mode → protected mode, its own
  A20 enable and E820 memory detection, no GRUB/Multiboot dependency
- **Preemptive multitasking** — round-robin scheduler, timer-driven
  context switches, blocking mutexes
- **Real processes** — ring 3 execution, `int 0x80` syscalls, an ELF32
  loader, and `fork()`/`exec()`/`wait()` with copy-on-write
- **Memory management** — bitmap physical frame allocator, paging with
  per-frame refcounts, a heap allocator
- **FAT16 filesystem** over a polled ATA (PIO) driver
- **VBE framebuffer graphics** with a PCI-enumerated video BAR, a PS/2
  mouse driver, and a simple windowing/compositor GUI
- **KRONK** — a small BASIC-style interpreted language with no external
  compiler dependency
- An interactive shell, text editor, and a couple of demo programs
  (Snake, a fork/COW demo)

## Building and running

Requires `gcc`, `nasm`, `ld`, `qemu-system-x86_64`, and `mtools`.

```bash
make run-custom-boot        # headless, serial output only
make run-custom-boot-vga    # with a display, for keyboard/mouse/GUI
```

## Project layout

    boot/       stage1.s, stage2.s, boot.s   -> bootloader + kernel entry
    kernel/     everything else              -> memory, tasking, drivers, GUI, shell
    userland/   ELF binaries run in ring 3   -> fork demo, COW demo, desktop app
    tools/      font/cursor bitmap generators

## Status

A personal learning project, not intended for real hardware or
production use. No filesystem journaling, no SMP, no networking.
