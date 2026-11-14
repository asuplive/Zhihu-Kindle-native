#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""1.2.9 文档截图：驱动 KO3 模拟器抓最新界面帧。

用法：python3 tools/make_shots_129.py
输出：docs/01-home.png 02-hotlist.png 04-reader.png 05-settings.png
      11-reader-image.png 12-img-placeholder.png 13-img-tap.png
      14-reader-page2.png（新增：后续页无标题）
"""
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kindle_sim import Sim, pick_binary, ROOT          # noqa: E402

DOCS = ROOT / 'docs'


def shoot(sim, name, wait=6.0):
    if not name:
        return
    png = sim.frame_png()
    if png:
        DOCS.mkdir(exist_ok=True)
        (DOCS / name).write_bytes(png)
        print('  ', name, len(png), 'bytes')
    else:
        print('  ', name, 'FAIL（无帧）')
    time.sleep(0.3)


def wait(sim, old, t=10.0):
    return sim.wait_frame(old, t)


def run(env_extra, steps):
    """steps: [(event|None, filename)]，每个事件后等到新帧再抓图。"""
    cmd, desc, arch = pick_binary()
    sim = Sim(cmd, desc, arch)
    for k, v in env_extra.items():
        os.environ[k] = v
    sim.start()
    seq = wait(sim, None, 15)
    print('启动帧', seq, desc)
    prev = seq
    for ev, name in steps:
        if ev:
            sim.inject(ev)
        s = wait(sim, prev, 12)
        if s:
            prev = s
        else:
            # 事件帧没等到也照抓（页内容可能没变，比如同一页重复刷新）
            print('   wait 超时:', ev, '->', name)
        import time as _t
        _t.sleep(0.6)          # ppm 写盘在 seq 之后，稍等再抓更稳
        shoot(sim, name)
        if not sim.alive():
            print('进程退出，中断')
            break
    sim.stop()
    for k in env_extra:
        os.environ.pop(k, None)


# ---- 第一轮：首页/热榜/设置/阅读页（图片自动档，直接种图） ----
run({}, [
    (None,            '01-home.png'),
    ('tap 300 361',   None),                 # 先跳过 loading 帧
    (None,            '02-hotlist.png'),     # 等列表真正渲染出来再抓
    ('back',          None),
    ('tap 300 1278',  '05-settings.png'),
    ('back',          None),
    ('demo',          '04-reader.png'),      # 第一页：大标题
    ('next',          '14-reader-page2.png'),    # 后续页：无标题（含图片）
    ('next',          None),
    ('prev',          '11-reader-image.png'),    # 回到含图页
    ('back',          None),
])

# ---- 第二轮：点按档占位（延迟到货才不会"有缓存直接显示"，占位框在第二页） ----
run({'ZH_SIM_IMGMODE': '1', 'ZH_SIM_IMG_DELAY': '8000'}, [
    ('demo',          None),
    ('next',          '13-img-tap.png'),
])

# ---- 第三轮：自动档"加载中"占位（延迟 8s 到货，第二页还是占位框） ----
run({'ZH_SIM_IMG_DELAY': '8000'}, [
    ('demo',          None),
    ('next',          '12-img-placeholder.png'),
])

print('完成')
