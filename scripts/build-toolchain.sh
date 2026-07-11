#!/bin/bash
set -e

ARCH=$1
TOOLCHAIN_DIR="$PWD/toolchain/$ARCH"

if [ "$ARCH" = "x86_64" ]; then
    echo "x86_64: 使用系统工具链"
    mkdir -p "$TOOLCHAIN_DIR"
    ln -sf /usr/bin/gcc "$TOOLCHAIN_DIR/gcc"
    ln -sf /usr/bin/g++ "$TOOLCHAIN_DIR/g++"
    exit 0
fi

if [ "$ARCH" = "aarch64" ]; then
    echo "aarch64: 安装交叉编译工具链"
    sudo apt-get install -y crossbuild-essential-arm64 2>/dev/null || true
    mkdir -p "$TOOLCHAIN_DIR"
    ln -sf /usr/bin/aarch64-linux-gnu-gcc "$TOOLCHAIN_DIR/gcc"
    ln -sf /usr/bin/aarch64-linux-gnu-g++ "$TOOLCHAIN_DIR/g++"
    exit 0
fi

echo "不支持的架构: $ARCH"
exit 1
