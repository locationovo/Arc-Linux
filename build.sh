#!/bin/bash
set -e
ARCH=${1:-x86_64}

echo "=== Arc Linux 构建 ==="
echo "架构: $ARCH"

bash scripts/download-sources.sh
bash scripts/build-toolchain.sh "$ARCH"
bash scripts/build-rootfs.sh "$ARCH"
bash packages/build-order.sh "$ARCH" "$PWD/rootfs-build/$ARCH"
bash scripts/build-image.sh "$ARCH"

echo "=== 构建完成 ==="
if [ "$ARCH" = "x86_64" ]; then
    echo "x86_64"    
else
    echo "arm64"    
fi
