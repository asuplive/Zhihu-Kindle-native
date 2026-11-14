#!/bin/sh
# 安装/更新：把扩展目录里的程序复制到 /mnt/us/zhihu/
#
# 注意屏幕提示一律用英文：Kindle 的 eips 只有西文字体，中文会显示成方块。
BASE=$(cd "$(dirname "$0")/.." && pwd)
DATA=/mnt/us/zhihu

mkdir -p "$DATA/cache"

SRC="$BASE/app/zhihu"
if [ ! -f "$SRC" ]; then
    eips -c 2>/dev/null
    eips 2 2 "zhihu-reader: package incomplete" 2>/dev/null
    eips 2 3 "missing app/zhihu" 2>/dev/null
    exit 1
fi

WANT=$(wc -c < "$SRC" | tr -d ' ')

# FAT32 上复制大文件有可能被截断（尤其是空间不足时），复制完必须核对大小，
# 否则会出现"能装但一打开就退"的怪现象
cp -f "$SRC" "$DATA/zhihu" || {
    eips -c 2>/dev/null
    eips 2 2 "copy failed - disk full?" 2>/dev/null
    exit 1
}
chmod 755 "$DATA/zhihu"
sync

GOT=$(wc -c < "$DATA/zhihu" | tr -d ' ')
eips -c 2>/dev/null
if [ "$WANT" != "$GOT" ]; then
    eips 2 2 "install INCOMPLETE" 2>/dev/null
    eips 2 3 "expect $WANT got $GOT bytes" 2>/dev/null
    eips 2 4 "free some space and retry" 2>/dev/null
    echo "install FAILED $(date) want=$WANT got=$GOT" >> "$DATA/run.log"
    exit 1
fi

eips 2 2 "zhihu-reader installed" 2>/dev/null
eips 2 3 "/mnt/us/zhihu/zhihu  $GOT bytes" 2>/dev/null
eips 2 4 "now open it from the KUAL menu" 2>/dev/null
echo "installed $(date) size=$GOT" >> "$DATA/run.log"
