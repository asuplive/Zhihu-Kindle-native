#!/bin/sh
# 交叉编译依赖（ARM，静态库）。开发机上跑一次即可，产物放在 deps/prebuilt/<arch>/。
set -e
DEP="${1:-/root/.codebuddy/artifact/2f8a7d30-9c21-4f0b-1d6e-60583dd50000/deps}"
ARCH="${2:-arm}"
OUT="$DEP/prebuilt/$ARCH"
CC=arm-linux-gnueabi-gcc
AR=arm-linux-gnueabi-ar
RANLIB=arm-linux-gnueabi-ranlib
CFLAGS="-Os -fPIC -ffunction-sections -fdata-sections"

mkdir -p "$OUT"

echo "=== mbedTLS ($ARCH) ==="
make -C "$DEP/mbedtls/library" clean >/dev/null 2>&1 || true
make -C "$DEP/mbedtls/library" -j4 CC="$CC" AR="$AR" RANLIB="$RANLIB" CFLAGS="$CFLAGS" libmbedtls.a libmbedx509.a libmbedcrypto.a >"$DEP/mbedtls_build_$ARCH.log" 2>&1
cp "$DEP/mbedtls/library/libmbed"*.a "$OUT/"

echo "=== FreeType ($ARCH) ==="
cd "$DEP/freetype"
if [ ! -f config.mk ]; then
  ./configure --host=arm-linux-gnueabi --build=x86_64-pc-linux-gnu \
      --disable-shared --enable-static --without-zlib --without-bzip2 \
      --without-png --without-harfbuzz --without-brotli --without-librt \
      CFLAGS="$CFLAGS" >"$DEP/freetype_configure_$ARCH.log" 2>&1
fi
make -j4 >"$DEP/freetype_build_$ARCH.log" 2>&1
cp objs/.libs/libfreetype.a "$OUT/"

echo "=== 产物 ==="
ls -lh "$OUT"
