#!/bin/bash
set -e

ARCH=$1
SYSROOT="$PWD/rootfs-build/$ARCH"

if [ "$ARCH" = "x86_64" ]; then
    KERNEL_IMAGE="$PWD/kernel/arch/x86/boot/bzImage"
    ISO="$PWD/arc-linux-x86_64.iso"

    echo ">>> 清理旧分卷文件..."
    rm -f arc-linux-x86_64.iso.part_*

    echo ">>> 创建ISO根目录..."
    ISO_ROOT="$PWD/iso-root"
    mkdir -p "$ISO_ROOT/boot/grub"

    echo ">>> 复制文件系统..."
    sudo rsync -a "$SYSROOT"/ "$ISO_ROOT"/
    sudo cp "$KERNEL_IMAGE" "$ISO_ROOT/boot/vmlinuz"
    sudo cp boot/grub.cfg "$ISO_ROOT/boot/grub/custom.cfg"

    echo ">>> 生成可启动ISO..."
    sudo grub-mkrescue -o "$ISO" "$ISO_ROOT" -- -volid "ARC_LINUX"

    sudo rm -rf "$ISO_ROOT"

    echo ">>> 分卷切割（2GB/卷）..."
    split -b 2G -d "$ISO" arc-linux-x86_64.iso.part_
    rm -f "$ISO"

    echo ">>> 分卷文件已生成:"
    ls -lh arc-linux-x86_64.iso.part_*

elif [ "$ARCH" = "aarch64" ]; then
    KERNEL_IMAGE="$PWD/kernel/arch/arm64/boot/Image"
    IMAGE="$PWD/arc-linux-aarch64.img"

    echo ">>> 清理旧分卷文件..."
    rm -f arc-linux-aarch64.img.part_*

    echo ">>> 创建4G磁盘镜像..."
    dd if=/dev/zero of="$IMAGE" bs=1M count=4096 status=progress

    echo ">>> 分区（GPT）..."
    parted -s "$IMAGE" mklabel gpt
    parted -s "$IMAGE" mkpart primary fat32 1MiB 513MiB
    parted -s "$IMAGE" mkpart primary ext4 513MiB 100%
    parted -s "$IMAGE" set 1 esp on

    echo ">>> 挂载镜像..."
    sudo losetup -fP "$IMAGE"
    LOOP=$(sudo losetup -j "$IMAGE" | head -1 | cut -d: -f1)
    EFI_PART="${LOOP}p1"
    ROOT_PART="${LOOP}p2"

    sudo mkfs.vfat -F32 "$EFI_PART"
    sudo mkfs.ext4 -F "$ROOT_PART"

    sudo mount "$ROOT_PART" /mnt
    sudo mkdir -p /mnt/boot/efi
    sudo mount "$EFI_PART" /mnt/boot/efi

    echo ">>> 复制文件系统到根分区..."
    sudo rsync -a "$SYSROOT"/ /mnt/

    echo ">>> 安装内核到 EFI 分区（EFI Stub，无需 GRUB）..."
    sudo mkdir -p /mnt/boot/efi/EFI/BOOT
    sudo cp "$KERNEL_IMAGE" /mnt/boot/efi/EFI/BOOT/BOOTAA64.EFI

    echo ">>> 内核命令行已内置在 CONFIG_CMDLINE 中，无需外部配置"

    sudo umount /mnt/boot/efi
    sudo umount /mnt
    sudo losetup -d "$LOOP"

    echo ">>> 分卷切割（2GB/卷）..."
    split -b 2G -d "$IMAGE" arc-linux-aarch64.img.part_
    rm -f "$IMAGE"

    echo ">>> 分卷文件已生成:"
    ls -lh arc-linux-aarch64.img.part_*
fi
