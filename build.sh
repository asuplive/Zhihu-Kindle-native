#!/bin/sh
# build.sh —— 一键构建（在 Linux 开发机上运行）
#
#   sh build.sh deps     下载并编译依赖（mbedTLS + FreeType，ARM 静态库）
#   sh build.sh arm      交叉编译出 Kindle 用的 dist/zhihu（armv5te 软浮点，静态链接）
#   sh build.sh host     编译开发机版本（用于自测与离屏渲染）
#   sh build.sh test     跑一遍自测
#   sh build.sh package  组装发布目录 release/
set -e

ROOT=$(cd "$(dirname "$0")" && pwd)
DEPS=${DEPS:-$ROOT/../deps}
ARCH=arm
CC=arm-linux-gnueabi-gcc

mkdir -p "$DEPS" "$ROOT/dist"

fetch_deps() {
    # GitHub 直连不通时，codeload 一般可用
    cd "$DEPS"
    [ -d mbedtls ] || {
        curl -fL "https://codeload.github.com/Mbed-TLS/mbedtls/tar.gz/refs/tags/v2.28.8" -o mbedtls.tgz
        tar xzf mbedtls.tgz && mv mbedtls-2.28.8 mbedtls
    }
    [ -d freetype ] || {
        curl -fL "https://codeload.github.com/freetype/freetype/tar.gz/refs/tags/VER-2-13-2" -o ft.tgz
        tar xzf ft.tgz && mv freetype-VER-2-13-2 freetype
    }
}

build_deps() {
    OUT="$DEPS/prebuilt/$ARCH"
    mkdir -p "$OUT"
    if [ ! -f "$OUT/libfreetype.a" ] || [ ! -f "$OUT/libmbedtls.a" ]; then
        fetch_deps
        echo "== mbedTLS =="
        make -C "$DEPS/mbedtls/library" -j4 CC=$CC AR=arm-linux-gnueabi-ar \
             RANLIB=arm-linux-gnueabi-ranlib CFLAGS="-Os -fPIC" \
             libmbedtls.a libmbedx509.a libmbedcrypto.a
        cp "$DEPS"/mbedtls/library/libmbed*.a "$OUT/"

        echo "== FreeType =="
        cat > /tmp/zh-kindle-tc.cmake <<'EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER arm-linux-gnueabi-gcc)
set(CMAKE_C_FLAGS "-Os -ffunction-sections -fdata-sections")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
        rm -rf "$DEPS/ft-build"
        cmake -S "$DEPS/freetype" -B "$DEPS/ft-build" \
              -DCMAKE_TOOLCHAIN_FILE=/tmp/zh-kindle-tc.cmake \
              -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
              -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_BZIP2=ON \
              -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_ZLIB=ON >/dev/null
        cmake --build "$DEPS/ft-build" --target freetype -j4 >/dev/null
        cp "$DEPS/ft-build/libfreetype.a" "$OUT/"
        mkdir -p "$OUT/include"
        cp -r "$DEPS/freetype/include/." "$OUT/include/"
        cp -r "$DEPS/ft-build/include/." "$OUT/include/"
    fi
    # 1.2.9：图片解码需要 libwebp（解码器子集静态库），缺失则现场构建
    [ -f "$DEPS/prebuilt/$ARCH/libwebpdecoder.a" ] || sh "$ROOT/tools/build_webp_arm.sh" arm
    [ -f "$DEPS/prebuilt/host/libwebpdecoder.a" ] || sh "$ROOT/tools/build_webp_arm.sh" host
    echo "依赖就绪：$OUT"
}

SRC="src/main.c src/ui.c src/api.c src/net.c src/json.c src/common.c src/md5.c src/zse96.c \
     src/content.c src/fb.c src/png.c src/img.c src/font.c src/input.c src/layout.c \
     src/account.c src/social.c src/qr.c src/qrcodegen.c"

build_arm() {
    build_deps
    OUT="$DEPS/prebuilt/$ARCH"
    $CC -static -Os -march=armv5te -mfloat-abi=softfp \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        -Isrc -I"$OUT/include" -I"$DEPS/mbedtls/include" -I"$ROOT/tools/webp-include" \
        $SRC \
        "$OUT/libfreetype.a" "$OUT/libmbedtls.a" "$OUT/libmbedx509.a" "$OUT/libmbedcrypto.a" \
        "$OUT/libwebpdecoder.a" \
        -lm -lpthread -o "$ROOT/dist/zhihu"
    arm-linux-gnueabi-strip "$ROOT/dist/zhihu"
    echo "生成 $ROOT/dist/zhihu"
    file "$ROOT/dist/zhihu"
    build_spy
}

# eips 探针：LD_PRELOAD 小工具，用来抓系统 eips 的 mxcfb 调用参数。
# 只用于排障，失败不影响主程序（所以构建失败不报错）。
build_spy() {
    $CC -Os -march=armv5te -mfloat-abi=softfp -shared -fPIC -nostdlib \
        -o "$ROOT/dist/spy.so" "$ROOT/tools/ioctl_spy.c" 2>/dev/null && {
        arm-linux-gnueabi-strip "$ROOT/dist/spy.so" 2>/dev/null
        echo "生成 $ROOT/dist/spy.so（排障用，可选）"
    } || echo "（跳过 spy.so：交叉工具链不支持）"
}

build_host() {
    HINC=$(pkg-config --cflags freetype2 2>/dev/null || echo -I/usr/include/freetype2)
    HLIBS=$(pkg-config --libs freetype2 2>/dev/null || echo -lfreetype)
    WP="$DEPS/prebuilt/host/libwebpdecoder.a"
    [ -f "$WP" ] || WP=""
    gcc -O1 -g -Isrc -I"$ROOT/tools/webp-include" $HINC -o /tmp/zhihu-host $SRC \
        -lmbedtls -lmbedx509 -lmbedcrypto $HLIBS $WP -lm
    echo "生成 /tmp/zhihu-host"
}

case "$1" in
    deps) build_deps ;;
    arm) build_arm ;;
    host) build_host ;;
    test)
        build_host
        /tmp/zhihu-host --selftest
        ;;
    package)
        build_arm
        sh "$ROOT/tools/package.sh"
        ;;
    *)
        echo "用法: sh build.sh deps|arm|host|test|package"
        ;;
esac
