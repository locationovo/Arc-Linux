#!/bin/bash
set -e
ARCH=${1:-x86_64}
DIR="$(dirname "$0")"
echo "$DIR"

echo "=== Arc Linux 构建 ==="
echo "架构: $ARCH"
bash $DIR/scripts/download-sources.sh
bash $DIR/scripts/build-toolchain.sh "$ARCH"
bash $DIR/scripts/build-rootfs.sh "$ARCH"
bash $DIR/packages/build-order.sh "$ARCH" "$PWD/rootfs-build/$ARCH"
bash $DIR/scripts/build-image.sh "$ARCH"

echo "=== 构建完成 ==="
if [ "$ARCH" = "x86_64" ]; then
    echo "x86_64"    
else
    echo "arm64"    
fi
