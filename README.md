# Kudos

Kudos - First Katools-integrated BSD-like operating system

## spamska's Kudos Notebook

17/11/25

Current commit is NOT final 0.0. This is a prototype release so I don't end up accidently losing the project like I did with original i386 COS. It's really barebones and barely works but I'll be iterating on each version and pushing origin to the main branch when I can.

So far I've integrated:

> Limine-based boot flow
> 8×8 bitmap font framebuffer console
> Full PC/AT scancode set with shift + caps
> Blinking cursor (it's annoying when it's slow though)
> Cooperative RedRobin scheduler (barely)
> x86_64 and RISC-V build targets (RISC-V crashes at the moment)
> ISO + HDD image generation
> QEMU run targets

## Dependencies

You need the following tools installed:
build tools
gcc or cross-compiler for riscv64-unknown-elf-gcc (when ARCH=riscv64)
make
ld
git
curl
dd
mkfs.exfat
xorriso (for x86_64 ISO)
genisoimage (for riscv64 ISO)
sgdisk
mtools (mcopy, mmd, mformat)
qemu-system-x86_64 or qemu-system-riscv64
bash
Limine (binary v10.x) — downloaded automatically by make fetch-limine
OVMF firmware — downloaded automatically by the Makefile
