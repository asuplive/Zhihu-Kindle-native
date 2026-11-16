#!/bin/sh
# 组装发布目录并打 zip
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/release"
NAME="知乎阅读-Kindle-1.3.0"
PKG="$OUT/$NAME"

rm -rf "$PKG"
mkdir -p "$PKG/extensions/zhihureader/app" "$PKG/extensions/zhihureader/bin"

cp "$ROOT/dist/zhihu" "$PKG/extensions/zhihureader/app/zhihu"
cp "$ROOT/kual/config.xml" "$PKG/extensions/zhihureader/config.xml"
cp "$ROOT/kual/menu.json" "$PKG/extensions/zhihureader/menu.json"
cp "$ROOT/kual/bin/start.sh" "$PKG/extensions/zhihureader/bin/start.sh"
cp "$ROOT/kual/bin/install.sh" "$PKG/extensions/zhihureader/bin/install.sh"
cp "$ROOT/kual/bin/log.sh" "$PKG/extensions/zhihureader/bin/log.sh"
# 排障用 LD_PRELOAD 探针（抓系统 eips 的 mxcfb 调用），有就带上
[ -f "$ROOT/dist/spy.so" ] && cp "$ROOT/dist/spy.so" "$PKG/extensions/zhihureader/bin/spy.so"
chmod 755 "$PKG/extensions/zhihureader/bin/"*.sh "$PKG/extensions/zhihureader/app/zhihu"

cp "$ROOT/README.md" "$PKG/README.md"
mkdir -p "$PKG/docs"
cp "$ROOT/docs/"*.png "$PKG/docs/" 2>/dev/null || true
[ -f "$ROOT/安装说明.txt" ] && cp "$ROOT/安装说明.txt" "$PKG/安装说明.txt"
# 变更说明跟着版本走，用通配避免每次改脚本
cp "$ROOT"/变更说明-*.md "$PKG/" 2>/dev/null || true
[ -f "$ROOT/LICENSE-AGPL-3.0.txt" ] && cp "$ROOT/LICENSE-AGPL-3.0.txt" "$PKG/LICENSE-AGPL-3.0.txt"

# 源码一并给出（AGPL 要求）
mkdir -p "$PKG/source"
cp -r "$ROOT/src" "$PKG/source/src"
cp "$ROOT/build.sh" "$PKG/source/build.sh"
cp -r "$ROOT/tools" "$PKG/source/tools" 2>/dev/null || true

cd "$OUT"
rm -f "$NAME.zip"
python3 - "$NAME" <<'PY'
import os, sys, zipfile
name = sys.argv[1]
skip = ('__pycache__', '.pyc')
with zipfile.ZipFile(name + '.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk(name):
        for f in sorted(files):
            if any(s in f for s in skip):
                continue
            p = os.path.join(root, f)
            zi = zipfile.ZipInfo(p)
            zi.date_time = (2026, 11, 12, 12, 0, 0)
            zi.compress_type = zipfile.ZIP_DEFLATED
            perm = 0o755 if (f.endswith('.sh') or f == 'zhihu') else 0o644
            zi.external_attr = perm << 16
            z.writestr(zi, open(p, 'rb').read())
print("打包完成：%s.zip" % name)
PY
ls -lh "$OUT/$NAME.zip"
