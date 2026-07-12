#!/bin/bash
set -e

SOURCES_DIR="$PWD/packages/sources"
LINUX_KERNEL_VER="6.9"
LINUX_KERNEL_URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${LINUX_KERNEL_VER}.tar.xz"
KERNEL_TARGET_DIR="$PWD/kernel"

mkdir -p "$SOURCES_DIR"
cd "$SOURCES_DIR"

GLIBC_VER="2.43"
BASH_VER="5.3"
COREUTILS_VER="9.11"
OPENSSL_VER="4.0.1"
LIBARCHIVE_VER="3.8.8"
CURL_VER="8.21.0"
GPGME_VER="2.1.2"
PACMAN_VER="7.1.0"
UTIL_VER="2.42.2"

echo ">>> 下载源码包..."
echo ">>> $PWD"

[ -f "glibc-${GLIBC_VER}.tar.gz" ] || \
  sudo curl -O -L "https://ftp.gnu.org/gnu/glibc/glibc-${GLIBC_VER}.tar.gz"
[ -f "bash-${BASH_VER}.tar.gz" ] || \
  sudo curl -O -L "https://ftp.gnu.org/gnu/bash/bash-${BASH_VER}.tar.gz"
[ -f "coreutils-${COREUTILS_VER}.tar.xz" ] || \
  sudo curl -O -L "https://ftp.gnu.org/gnu/coreutils/coreutils-${COREUTILS_VER}.tar.xz"
[ -f "openssl-${OPENSSL_VER}.tar.gz" ] || \
  sudo curl -O -L "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VER}/openssl-${OPENSSL_VER}.tar.gz"
[ -f "libarchive-${LIBARCHIVE_VER}.tar.xz" ] || \
  sudo curl -O -L "https://github.com/libarchive/libarchive/releases/download/v${LIBARCHIVE_VER}/libarchive-${LIBARCHIVE_VER}.tar.xz"
[ -f "curl-${CURL_VER}.tar.xz" ] || \
  sudo curl -O -L "https://curl.se/download/curl-${CURL_VER}.tar.xz"
[ -f "gpgme-${GPGME_VER}.tar.bz2" ] || \
  sudo curl -O -L "https://www.gnupg.org/ftp/gcrypt/gpgme/gpgme-${GPGME_VER}.tar.bz2"
[ -f "pacman-${PACMAN_VER}.tar.xz" ] || \
  sudo curl -O -L "pacman-${PACMAN_VER}.tar.xz" -L \
  "https://gitlab.archlinux.org/pacman/pacman/-/releases/v${PACMAN_VER}/downloads/pacman-${PACMAN_VER}.tar.xz"
[ -f "util-linux-${UTIL_VER}.tar.gz" ] || \
  sudo curl -O -L "util-linux-${UTIL_VER}.tar.gz" -L \
  "https://github.com/util-linux/util-linux/archive/refs/tags/v${UTIL_VER}.tar.gz"

# ===== 内核下载与校验 =====
echo ">>> 下载 Linux 内核源码..."
if [ ! -f "linux-${LINUX_KERNEL_VER}.tar.xz" ]; then
    sudo wget -q "$LINUX_KERNEL_URL"
else
    echo "内核源码包已存在，跳过下载"
fi

echo ">>> 校验所有源码包完整性..."
for pkg in *.tar.*; do
    if [ ! -s "$pkg" ]; then
        echo "错误：发现空文件或损坏包 $pkg，已自动清理"
        rm -f "$pkg"
        exit 1
    fi
done

echo ">>> 解压内核源码到项目目录..."
mkdir -p "$KERNEL_TARGET_DIR"
tar -xf "linux-${LINUX_KERNEL_VER}.tar.xz" \
    --strip-components=1 \
    -C "$KERNEL_TARGET_DIR"

echo ">>> 所有源码准备完成"
