#!/bin/bash
set -e

ARCH=$1
SYSROOT=$2
JOBS=$(nproc)
SOURCES_DIR="$PWD/packages/sources"
BUILD_DIR="$PWD/packages/build/$ARCH"

if [ "$ARCH" = "x86_64" ]; then
    CROSS_COMPILE=""; HOST="x86_64-linux-gnu"; OPENSSL_TARGET="linux-x86_64"
elif [ "$ARCH" = "aarch64" ]; then
    CROSS_COMPILE="aarch64-linux-gnu-"; HOST="aarch64-linux-gnu"; OPENSSL_TARGET="linux-aarch64"
else
    echo "未知架构: $ARCH"; exit 1
fi

export CC="${CROSS_COMPILE}gcc" CXX="${CROSS_COMPILE}g++" AR="${CROSS_COMPILE}ar" RANLIB="${CROSS_COMPILE}ranlib" CFLAGS="-O2 -pipe"
mkdir -p "$BUILD_DIR" "$SYSROOT"

unpack() {
    local pkg=$1 dir=$2 ext=$3
    echo ">>> 解压 $pkg..."
    rm -rf "$BUILD_DIR/$dir"
    case $ext in
        tar.xz) tar -xf "$SOURCES_DIR/$pkg" -C "$BUILD_DIR" ;;
        tar.gz) tar -xzf "$SOURCES_DIR/$pkg" -C "$BUILD_DIR" ;;
        tar.bz2) tar -xjf "$SOURCES_DIR/$pkg" -C "$BUILD_DIR" ;;
    esac
}

# glibc
unpack "glibc-2.39.tar.xz" "glibc-2.39" "tar.xz"
cd "$BUILD_DIR/glibc-2.39" && mkdir -p build && cd build
../configure --prefix=/usr --host=$HOST --disable-werror && make -j$JOBS && make install DESTDIR="$SYSROOT"
cd "$OLDPWD"

# bash
unpack "bash-5.2.21.tar.gz" "bash-5.2.21" "tar.gz"
cd "$BUILD_DIR/bash-5.2.21"
./configure --prefix=/usr --host=$HOST --enable-readline --without-bash-malloc && make -j$JOBS && make install DESTDIR="$SYSROOT"
cd "$OLDPWD"

# coreutils
unpack "coreutils-9.4.tar.xz" "coreutils-9.4" "tar.xz"
cd "$BUILD_DIR/coreutils-9.4"
./configure --prefix=/usr --host=$HOST && make -j$JOBS && make install DESTDIR="$SYSROOT"
cd "$OLDPWD"

# openssl
unpack "openssl-3.2.1.tar.gz" "openssl-3.2.1" "tar.gz"
cd "$BUILD_DIR/openssl-3.2.1"
./Configure "$OPENSSL_TARGET" --prefix=/usr --cross-compile-prefix="$CROSS_COMPILE" no-shared && make -j$JOBS && make install DESTDIR="$SYSROOT"
cd "$OLDPWD"

# libarchive
unpack "libarchive-3.7.2.tar.xz" "libarchive-3.7.2" "tar.xz"
cd "$BUILD_DIR/libarchive-3.7.2"
./configure --prefix=/usr --host=$HOST --disable-shared && make -j$JOBS && make install DESTDIR="$SYSROOT"
cd "$OLDPWD"

# curl
unpack "curl-8.6.0.tar.xz" "curl-8.6.0" "tar.xz"
cd "$BUILD_DIR/curl-8.6.0"
./configure --prefix=/usr --host=$HOST --with-openssl --disable-shared && make -j$JOBS && make install DESTDIR="$SYSROOT"
cd "$OLDPWD"

# gpgme
unpack "gpgme-1.23.2.tar.bz2" "gpgme-1.23.2" "tar.bz2"
cd "$BUILD_DIR/gpgme-1.23.2"
./configure --prefix=/usr --host=$HOST --disable-shared && make -j$JOBS && make install DESTDIR="$SYSROOT"
cd "$OLDPWD"

# pacman
unpack "pacman-6.0.2.tar.xz" "pacman-6.0.2" "tar.xz"
cd "$BUILD_DIR/pacman-6.0.2"
meson setup build --prefix=/usr -Dpkg-ext=.pkg.tar.zst && meson compile -C build && DESTDIR="$SYSROOT" meson install -C build
cd "$OLDPWD"

echo ">>> 所有包编译完成"
