#!/usr/bin/env python3
"""把系统 CA 根证书打包成 C 头文件（内置一份，保证设备端开箱能用）。

知乎证书链：*.zhihu.com <- GeoTrust G2 TLS CN RSA4096 SHA256 2022 CA1
           <- DigiCert Global Root G2
但用户可能遇到 CDN 切换，所以内置完整 Mozilla 根集合更稳妥；
也可以放 /mnt/us/zhihu/ca.pem 覆盖。
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = sys.argv[1] if len(sys.argv) > 1 else "/etc/ssl/certs/ca-certificates.crt"
OUT = os.path.join(ROOT, "src", "ca_bundle.h")


def main():
    pem = open(SRC, "r", encoding="utf-8", errors="ignore").read()
    if "BEGIN CERTIFICATE" not in pem:
        sys.exit("CA 包无效")
    n = pem.count("BEGIN CERTIFICATE")
    esc = pem.replace("\\", "\\\\").replace('"', '\\"')
    lines = ['/* 自动生成：内置 CA 根证书（%d 张）。可用 /mnt/us/zhihu/ca.pem 覆盖。 */' % n,
             "#ifndef ZH_CA_BUNDLE_H", "#define ZH_CA_BUNDLE_H", "",
             "static const char ZH_CA_BUNDLE[] ="]
    chunk = 200
    for i in range(0, len(esc), chunk):
        seg = esc[i:i + chunk].replace("\n", "\\n")
        lines.append('    "%s"' % seg)
    lines += [";", "", "#endif", ""]
    open(OUT, "w", encoding="utf-8").write("\n".join(lines))
    print("生成 %s：%d 张证书，%d KB" % (OUT, n, os.path.getsize(OUT) // 1024))


if __name__ == "__main__":
    main()
