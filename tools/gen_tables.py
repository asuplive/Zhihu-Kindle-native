#!/usr/bin/env python3
"""从已验证的 Lua 实现里抽出 ZK / ZB 常量表，生成 C 头文件。

为什么要生成而不是手抄：256 个 0-255 的常量手抄必错，且错了以后签名
只在某些字节上出错，很难定位。生成 + 断言长度可以彻底避免这一类问题。
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LUA = os.environ.get("ZH_LUA_SRC") or os.path.join(
    os.path.dirname(ROOT), "zhihu-reader-kindle", "zhihureader.koplugin", "zhihu", "zse96.lua")
OUT = os.path.join(ROOT, "src", "zse_tables.h")


def grab(name, src):
    m = re.search(r"local %s = \{(.*?)\n\}" % name, src, re.S)
    if not m:
        sys.exit("找不到 %s 表" % name)
    body = m.group(1)
    body = re.sub(r"--.*", "", body)
    return [int(x.strip()) for x in body.split(",") if x.strip()]


def main():
    if not os.path.exists(LUA):
        sys.exit("缺少 Lua 参考实现: %s" % LUA)
    src = open(LUA, encoding="utf-8").read()
    zk = grab("ZK", src)
    zb = grab("ZB", src)
    assert len(zk) == 32, "ZK 长度应为 32，实际 %d" % len(zk)
    assert len(zb) == 256, "ZB 长度应为 256，实际 %d" % len(zb)
    assert len(set(zb)) == 256, "ZB 应是 0..255 的一个排列"

    lines = ["/* 自动生成，请勿手改。来源：zse96.lua（与 Zhihu++ ZseSigner.kt 逐项一致） */",
             "#ifndef ZH_ZSE_TABLES_H", "#define ZH_ZSE_TABLES_H", "",
             "#include <stdint.h>", "",
             "static const uint32_t ZK_TABLE[32] = {"]
    for i in range(0, 32, 4):
        lines.append("    " + ", ".join("0x%08Xu" % v for v in zk[i:i + 4]) + ",")
    lines += ["};", "", "static const uint8_t ZB_TABLE[256] = {"]
    for i in range(0, 256, 16):
        lines.append("    " + ", ".join("0x%02X" % v for v in zb[i:i + 16]) + ",")
    lines += ["};", "", "#endif", ""]
    open(OUT, "w", encoding="utf-8").write("\n".join(lines))
    print("生成 %s：ZK=%d ZB=%d" % (OUT, len(zk), len(zb)))


if __name__ == "__main__":
    main()
