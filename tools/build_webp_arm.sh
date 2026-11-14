#!/bin/sh
# 构建静态 libwebpdecoder.a（解码器子集，约 360KB）。
# 用法：sh tools/build_webp_arm.sh [arm|host]
#   arm  —— arm-linux-gnueabi 交叉编译（-march=armv5te -mfloat-abi=softfp，
#           与主程序一致），产出放 $DEPS/prebuilt/arm/libwebpdecoder.a
#   host —— 用系统自带的 libwebpdecoder.a（apt install libwebp-dev 得到的
#           /usr/lib/x86_64-linux-gnu/libwebpdecoder.a），拷到
#           $DEPS/prebuilt/host/libwebpdecoder.a 供 build.sh host 链接。
# 源码：libwebp 1.4.0（与 tools/webp-include 里的公开头文件同一版本）。
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEPS="$ROOT/../deps"
PRE="$DEPS/prebuilt"
SRC_TGZ="/tmp/webp.tar.gz"
SRC_URL="https://storage.githubusercontent.com/downloads.webmproject.org/releases/webp/libwebp-1.4.0.tar.gz"
SRC_URL2="https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.4.0.tar.gz"
VER_DIR="/tmp/libwebp-1.4.0"

mkdir -p "$PRE/arm" "$PRE/host"

case "${1:-arm}" in
host)
    if [ -f /usr/lib/x86_64-linux-gnu/libwebpdecoder.a ]; then
        cp /usr/lib/x86_64-linux-gnu/libwebpdecoder.a "$PRE/host/"
        echo "host 库：拷贝系统 libwebpdecoder.a -> $PRE/host/"
    else
        echo "错误：系统没有 libwebpdecoder.a（apt install libwebp-dev）" >&2
        exit 1
    fi
    ;;
arm)
    OUT="$PRE/arm/libwebpdecoder.a"
    if [ -f "$OUT" ]; then
        echo "已有 $OUT，跳过（删除可强制重建）"
        exit 0
    fi
    if [ ! -d "$VER_DIR" ]; then
        curl -sL --max-time 120 -o "$SRC_TGZ" "$SRC_URL" ||
            curl -sL --max-time 120 -o "$SRC_TGZ" "$SRC_URL2"
        tar xzf "$SRC_TGZ" -C /tmp
    fi
    OBJ=$(mktemp -d)
    ( cd "$VER_DIR/src" &&
      arm-linux-gnueabi-gcc -static -Os -march=armv5te -mfloat-abi=softfp \
          -ffunction-sections -fdata-sections \
          -I.. -I../src -c dec/*.c dsp/*.c utils/*.c ) &&
    ( cd "$VER_DIR/src" && arm-linux-gnueabi-ar rcs "$OUT" *.o )
    arm-linux-gnueabi-strip --strip-debug "$OUT"
    rm -rf "$OBJ"
    echo "ARM 库：$OUT（$(ls -la "$OUT" | awk '{print $5}') 字节）"
    ;;
*)
    echo "用法：sh tools/build_webp_arm.sh [arm|host]" >&2
    exit 1
    ;;
esac
