#!/bin/bash
set -e

ARCH=$1
SYSROOT="$PWD/rootfs-build/$ARCH"

mkdir -p "$SYSROOT"/{bin,sbin,lib,usr/{bin,lib,sbin},dev,proc,sys,tmp,etc,var/{lib/pacman,cache/pacman/pkg},home,mnt,root,boot}

cp rootfs/passwd "$SYSROOT/etc/"
cp rootfs/group "$SYSROOT/etc/"
cp rootfs/fstab "$SYSROOT/etc/"
cp rootfs/os-release "$SYSROOT/etc/"

sudo mknod "$SYSROOT/dev/console" c 5 1 2>/dev/null || true
sudo mknod "$SYSROOT/dev/null" c 1 3 2>/dev/null || true
sudo mknod "$SYSROOT/dev/tty" c 5 0 2>/dev/null || true

sudo chown -R root:root "$SYSROOT"

echo "根文件系统骨架已创建: $SYSROOT"
