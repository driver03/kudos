.SUFFIXES:
ARCH ?= x86_64
QEMUFLAGS := -m 2G
override IMAGE_NAME := 0.0-Kudos
HOST_CC := gcc
HOST_CFLAGS := -g -O2 -pipe
HOST_CPPFLAGS :=
HOST_LDFLAGS :=
HOST_LIBS :=
FS_SIZE_MB ?= 5120
FS_IMAGE := kfs.img
FS_OFFSET_SECTORS := 10485760
# --- RISC-V Toolchain ---
ifeq ($(ARCH),riscv64)
    TOOLCHAIN_PREFIX := riscv64-unknown-elf-
    CC := $(TOOLCHAIN_PREFIX)gcc
    LD := $(TOOLCHAIN_PREFIX)ld
    CFLAGS := -g -O2 -pipe -march=rv64imac_zicsr_zifencei -mabi=lp64 -mno-relax
    LDFLAGS := -m elf64lriscv --no-relax
endif

.PHONY: all
all: $(IMAGE_NAME).iso

.PHONY: all-hdd
all-hdd: $(IMAGE_NAME).hdd

.PHONY: run
run: run-$(ARCH)

.PHONY: run-hdd
run-hdd: run-hdd-$(ARCH)

# --- QEMU run targets ---
.PHONY: run-x86_64
run-x86_64: ovmf/ovmf-code-$(ARCH).fd $(IMAGE_NAME).iso
	qemu-system-$(ARCH) \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		$(QEMUFLAGS)

.PHONY: run-riscv64
run-riscv64: $(IMAGE_NAME).iso
	qemu-system-riscv64 \
	    -machine virt \
	    -bios none \
	    -kernel kernel/bin-riscv64/kernel \
	    -drive file=0.0-Kudos.iso,format=raw,id=hd,if=none \
	    -device virtio-blk-device,drive=hd,bus=virtio-mmio-bus.0 \
	    -m 5G \
	    -nographic

.PHONY: run-hdd-x86_64
run-hdd-x86_64: ovmf/ovmf-code-$(ARCH).fd $(IMAGE_NAME).hdd
	qemu-system-$(ARCH) \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS)

.PHONY: run-hdd-riscv64
run-hdd-riscv64: $(IMAGE_NAME).hdd
	qemu-system-riscv64 \
		-machine virt \
		-bios none \
		-kernel kernel/bin-$(ARCH)/kernel \
		-drive file=$(IMAGE_NAME).hdd,format=raw,id=hd \
		-device virtio-blk-device,drive=hd \
		$(QEMUFLAGS)

# --- OVMF firmware ---
ovmf/ovmf-code-$(ARCH).fd:
	mkdir -p ovmf
	curl -Lo $@ https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/ovmf-code-$(ARCH).fd
	case "$(ARCH)" in \
		aarch64) dd if=/dev/zero of=$@ bs=1 count=0 seek=67108864 2>/dev/null;; \
		riscv64) dd if=/dev/zero of=$@ bs=1 count=0 seek=33554432 2>/dev/null;; \
	esac

# --- Limine ---
.PHONY: fetch-limine
fetch-limine:
	rm -rf limine
	git clone https://codeberg.org/Limine/Limine.git limine --branch=v10.x-binary --depth=1

limine/limine: fetch-limine
	$(MAKE) -C limine \
		CC="$(HOST_CC)" \
		CFLAGS="$(HOST_CFLAGS)" \
		CPPFLAGS="$(HOST_CPPFLAGS)" \
		LDFLAGS="$(HOST_LDFLAGS)" \
		LIBS="$(HOST_LIBS)"

# --- Kernel ---
kernel/.deps-obtained:
	./kernel/get-deps

.PHONY: kernel
kernel: kernel/.deps-obtained
	$(MAKE) -C kernel ARCH=$(ARCH) CC=$(CC) LD=$(LD) CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)"

# --- Extra exFAT filesystem image ---
$(FS_IMAGE):
	dd if=/dev/zero of=$@ bs=1M count=$(FS_SIZE_MB)
	mkfs.exfat -n KUDOS_FS $@

# --- ISO with exFAT filesystem ---
$(IMAGE_NAME).iso: limine/limine kernel $(FS_IMAGE)
	rm -rf iso_root
	mkdir -p iso_root/boot
	cp -v kernel/bin-$(ARCH)/kernel iso_root/boot/
	mkdir -p iso_root/boot/limine
	cp -v limine.conf iso_root/boot/limine/
	mkdir -p iso_root/EFI/BOOT
ifeq ($(ARCH),x86_64)
	cp -v limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/boot/limine/
	cp -v limine/BOOTX64.EFI iso_root/EFI/BOOT/
	cp -v limine/BOOTIA32.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(IMAGE_NAME).iso
	./limine/limine bios-install $(IMAGE_NAME).iso
endif
ifeq ($(ARCH),riscv64)
	genisoimage -R -o $(IMAGE_NAME).iso iso_root
endif
	dd if=$(FS_IMAGE) of=$(IMAGE_NAME).iso bs=512 seek=$(FS_OFFSET_SECTORS) conv=notrunc
	rm -rf iso_root make $(FS_IMAGE)

# --- HDD ---
$(IMAGE_NAME).hdd: limine/limine kernel
	rm -f $(IMAGE_NAME).hdd
	dd if=/dev/zero bs=1M count=0 seek=64 of=$(IMAGE_NAME).hdd
ifeq ($(ARCH),x86_64)
	PATH=$$PATH:/usr/sbin:/sbin sgdisk $(IMAGE_NAME).hdd -n 1:2048 -t 1:ef00 -m 1
	./limine/limine bios-install $(IMAGE_NAME).hdd
else
	PATH=$$PATH:/usr/sbin:/sbin sgdisk $(IMAGE_NAME).hdd -n 1:2048 -t 1:ef00
endif
	mformat -i $(IMAGE_NAME).hdd@@1M
	mmd -i $(IMAGE_NAME).hdd@@1M ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
	mcopy -i $(IMAGE_NAME).hdd@@1M kernel/bin-$(ARCH)/kernel ::/boot
	mcopy -i $(IMAGE_NAME).hdd@@1M limine.conf ::/boot/limine
ifeq ($(ARCH),x86_64)
	mcopy -i $(IMAGE_NAME).hdd@@1M limine/limine-bios.sys ::/boot/limine
	mcopy -i $(IMAGE_NAME).hdd@@1M limine/BOOTX64.EFI ::/EFI/BOOT
	mcopy -i $(IMAGE_NAME).hdd@@1M limine/BOOTIA32.EFI ::/EFI/BOOT
endif

# --- Clean ---
.PHONY: clean
clean:
	$(MAKE) -C kernel clean
	rm -rf iso_root $(IMAGE_NAME).iso $(IMAGE_NAME).hdd $(FS_IMAGE) limine

.PHONY: distclean
distclean:
	$(MAKE) -C kernel distclean
	rm -rf iso_root *.iso *.hdd $(FS_IMAGE) kernel-deps limine ovmf
