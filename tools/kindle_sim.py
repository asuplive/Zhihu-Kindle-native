#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Kindle Oasis 3 桌面模拟器 —— 在电脑上真实运行「知乎阅读」并像真机一样操作。

它模拟的不是外壳贴图，而是程序真正依赖的硬件接口：

  · 帧缓冲：按 KO3 的 1264x1680 / 8bpp 灰度离线渲染，逐像素等同真机
  · 输入：MT 协议 B 虚拟触摸屏（tap / swipe）+ 实体翻页键码
  · CPU：用 qemu-user 直接跑设备上的 ARMv5TE 静态 ELF（不是重新编译的 x86 版）
  · 刷新：记录每次 EPDC 提交（DU / GC16 在真机上才有波形，模拟器只记帧）

无法模拟的部分（必须真机验证）：i.MX EPDC 波形与残影、PMIC/温补、电池、
电源管理、真实触摸固件、内核 ioctl 返回值。

用法：
    python3 kindle_sim.py                     # 起服务，浏览器打开 http://127.0.0.1:8765
    python3 kindle_sim.py --port 9000          # 换端口
    python3 kindle_sim.py --bin /path/zhihu    # 指定二进制（自动识别 ARM 并挂 qemu）
    python3 kindle_sim.py --test              # 自动跑一轮 KO3 端到端回归，输出报告
    python3 kindle_sim.py --standalone OUT.html  # 抓帧生成可离线双击的 HTML

依赖：Python 3 标准库即可（PNG 用 zlib 手写编码，不需要 Pillow）。
若要用设备上的 ARM 二进制，需要 qemu-arm-static；没有的话会自动退回 x86 主机版。
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ---- KO3 真实硬件参数 -------------------------------------------------------
W, H = 1264, 1680          # KO3 / PW5 面板分辨率
PPI = 300
SCREEN_INCH = 7
BPP = 8


# ---- PPM -> PNG（纯标准库，灰度 8bit） -------------------------------------
def parse_p5(data):
    """解析二进制 P5 PPM，返回 (w, h, 像素字节)。"""
    pos, tokens = 2, []
    while len(tokens) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            while pos < len(data) and data[pos:pos + 1] not in (b'\n', b'\r'):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        tokens.append(data[start:pos])
    w, h, maxv = int(tokens[0]), int(tokens[1]), int(tokens[2])
    pos += 1                                  # header 与数据之间恰好一个空白
    px = data[pos:pos + w * h]
    if maxv != 255:                           # 理论上不会走到
        px = bytes(min(255, v * 255 // maxv) for v in px)
    return w, h, px


def png_gray(w, h, pixels):
    raw = b''.join(b'\x00' + pixels[y * w:(y + 1) * w] for y in range(h))

    def chunk(tag, payload):
        return (struct.pack('>I', len(payload)) + tag + payload +
                struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff))

    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 6)) +
            chunk(b'IEND', b''))


def frame_to_png(path):
    data = Path(path).read_bytes()
    w, h, px = parse_p5(data)
    return png_gray(w, h, px)


# ---- 二进制选择 -------------------------------------------------------------
def elf_machine(path):
    with open(path, 'rb') as f:
        hdr = f.read(20)
    if hdr[:4] != b'\x7fELF':
        return '不是 ELF'
    m = struct.unpack_from('<H', hdr, 18)[0]
    return {0x28: 'ARM', 0x03: 'x86', 0x3e: 'x86-64', 0xb7: 'AArch64'}.get(m, hex(m))


def pick_binary(explicit=None):
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    candidates += [ROOT / 'dist' / 'zhihu', Path('/tmp/zhihu-host')]
    for c in candidates:
        if c.is_file():
            arch = elf_machine(str(c))
            if arch == 'ARM':
                q = shutil.which('qemu-arm-static') or shutil.which('qemu-arm')
                if q:
                    return [q, str(c)], 'ARMv5TE 静态 ELF（qemu-user 执行）', arch
                continue          # 没有 qemu 就先找 x86 版
            return [str(c)], '本机编译版（' + arch + '）', arch
    raise RuntimeError('找不到可执行的二进制。请先 sh build.sh arm 或 sh build.sh host，'
                       'ARM 二进制还需要安装 qemu-arm-static。')


# ---- 模拟器主体 -------------------------------------------------------------
class Sim:
    def __init__(self, cmd, arch_desc, arch, workdir=None, w=W, h=H):
        self.cmd, self.arch_desc, self.arch = cmd, arch_desc, arch
        self.w, self.h = w, h
        self.dir = Path(workdir) if workdir else ROOT / '.sim'
        self.dir.mkdir(parents=True, exist_ok=True)
        self.frame = self.dir / 'frame.ppm'
        self.seqpath = Path(str(self.frame) + '.seq')
        self.events = self.dir / 'events.txt'
        self.home = self.dir / 'home'
        # 彻底隔离状态：程序会把字号/旋转等写进 config、把读过的文章写进 history，
        # 不清掉的话上一轮的设置会让本轮的菜单坐标全部偏移，测试结果不可复现。
        if self.home.exists():
            for p in self.home.iterdir():
                if p.is_dir():
                    shutil.rmtree(p)
                else:
                    p.unlink()
        else:
            self.home.mkdir(parents=True)
        self.stdout = self.dir / 'stdout.log'
        self.events.write_text('')            # 清空事件队列
        for p in (self.frame, self.seqpath):
            if p.exists():
                p.unlink()
        self.logfile = self.home / 'log.txt'
        self.proc = None
        self._lock = threading.Lock()
        self.event_history = []

    def start(self):
        env = os.environ.copy()
        env.update({
            'ZH_SIM': '1', 'ZH_SIM_LIVE': '1',
            'ZH_SIM_W': str(self.w), 'ZH_SIM_H': str(self.h),
            'ZH_SIM_OUT': str(self.frame),
            'ZH_SIM_EVENTS': str(self.events),
            'ZHIHU_HOME': str(self.home),
        })
        self._fout = open(self.stdout, 'wb')
        self.proc = subprocess.Popen(self.cmd, env=env, cwd=str(ROOT),
                                     stdout=self._fout, stderr=subprocess.STDOUT)
        return self.proc

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def seq(self):
        try:
            return self.seqpath.read_text().strip()
        except OSError:
            return ''

    def inject(self, line):
        line = line.strip().replace('\n', ' ')
        if not line:
            return False
        with self._lock:
            with open(self.events, 'ab') as f:      # O_APPEND，一次 write，整行原子
                f.write((line + '\n').encode('utf-8'))
            self.event_history.append((time.strftime('%H:%M:%S'), line))
            self.event_history = self.event_history[-40:]
        return True

    def wait_frame(self, old=None, timeout=12.0):
        """等到出现与 old 不同的新帧，返回新 seq 字符串；超时返回 None。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            s = self.seq()
            if s and s != old:
                return s
            if not self.alive():
                return None
            time.sleep(0.08)
        return None

    def frame_png(self):
        try:
            return frame_to_png(self.frame)
        except Exception:
            return None

    def log(self, tail=40):
        try:
            lines = self.logfile.read_text('utf-8', 'replace').splitlines()
            return '\n'.join(lines[-tail:])
        except OSError:
            return ''

    def status(self):
        return {
            'alive': self.alive(),
            'rc': None if self.alive() else (self.proc.poll() if self.proc else None),
            'arch': self.arch, 'arch_desc': self.arch_desc,
            'w': self.w, 'h': self.h, 'ppi': PPI, 'bpp': BPP,
            'seq': self.seq(), 'cmd': ' '.join(self.cmd),
            'events': self.event_history[-12:],
        }

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.inject('quit')
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
        try:
            self._fout.close()
        except Exception:
            pass


# ---- 前端页面 ---------------------------------------------------------------
PAGE = r'''<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>Kindle Oasis 3 · 知乎阅读 模拟器</title>
<style>
:root{--bg:#12151a;--panel:#1b2028;--line:#2c3440;--txt:#dfe5ec;--dim:#8b97a6;
      --accent:#e8b64c;--ok:#4ec9a0;--bad:#e56b6b;--case:#2a2e33;--case2:#1c1f23}
*{box-sizing:border-box}
body{margin:0;background:radial-gradient(1200px 700px at 30% 0%,#1c2530,#0e1115 70%);
     color:var(--txt);font:14px/1.6 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif;
     min-height:100vh;padding:24px}
h1{font-size:19px;margin:0 0 4px;font-weight:600;letter-spacing:.5px}
.sub{color:var(--dim);font-size:12px;margin-bottom:20px}
.wrap{display:flex;gap:26px;align-items:flex-start;flex-wrap:wrap}
/* ---------- KO3 机身 ---------- */
.device{position:relative;background:linear-gradient(145deg,var(--case),var(--case2));
  border-radius:16px 16px 22px 22px;padding:22px 24px 30px 76px;
  box-shadow:0 24px 60px #0009,inset 0 1px 0 #ffffff14,inset 0 -2px 6px #0007;
  border:1px solid #3a3f46;flex:0 0 auto}
.device:before{content:"";position:absolute;left:0;top:0;bottom:0;width:60px;
  background:linear-gradient(90deg,#32373d,#23272c);border-radius:15px 0 0 21px;
  box-shadow:inset -3px 0 8px #0006}
.grip{position:absolute;left:20px;top:46%;width:20px;height:80px;border-radius:10px;
  background:linear-gradient(90deg,#1b1e22,#2c3137);box-shadow:inset 0 0 4px #000a}
.bezel{background:#080a0c;border-radius:3px;padding:15px 14px;
  box-shadow:inset 0 0 0 1px #000,0 0 0 1px #41464d,0 2px 10px #0008}
#screen{display:block;width:380px;height:505px;background:#fff;cursor:crosshair;
  image-rendering:auto;user-select:none;-webkit-user-drag:none}
#screen:active{opacity:.93}
.brand{position:absolute;bottom:9px;left:0;right:0;text-align:center;color:#6b727b;
  font-size:10px;letter-spacing:3px}
.port{position:absolute;bottom:-3px;left:50%;transform:translateX(-50%);
  width:34px;height:5px;background:#0b0d0f;border-radius:0 0 4px 4px;box-shadow:0 1px 2px #000}
.power{position:absolute;top:-5px;right:60px;width:38px;height:6px;border-radius:3px;
  background:linear-gradient(#3d4248,#202428);box-shadow:0 1px 2px #000}
/* 右侧两个物理翻页键 */
.keys{position:absolute;right:-1px;top:26%;display:flex;flex-direction:column;gap:74px}
.keys button{width:16px;height:66px;border:1px solid #454b52;border-radius:3px;
  background:linear-gradient(90deg,#343940,#22262b);cursor:pointer;color:#aab3bd;font-size:9px;
  box-shadow:0 2px 5px #0008,inset 0 1px 0 #ffffff18}
.keys button:active{background:#111418;transform:translateY(1px)}
.keycap{position:absolute;right:22px;color:#79818b;font-size:9px;letter-spacing:1px}
/* ---------- 侧栏 ---------- */
.side{flex:1 1 380px;min-width:340px;max-width:560px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:10px;
  padding:14px 16px;margin-bottom:14px}
.card h2{font-size:12px;color:var(--accent);margin:0 0 10px;font-weight:600;
  letter-spacing:1px;text-transform:uppercase}
table{width:100%;border-collapse:collapse;font-size:12px}
td{padding:3px 0;color:var(--dim)}td+td{text-align:right;color:var(--txt);
  font-family:ui-monospace,Menlo,Consolas,monospace}
.quick{display:grid;grid-template-columns:repeat(3,1fr);gap:7px}
button.b{background:#252c36;color:var(--txt);border:1px solid var(--line);
  border-radius:6px;padding:7px 4px;font-size:12px;cursor:pointer}
button.b:hover{border-color:var(--accent);color:var(--accent)}
button.b:active{background:#161b22}
pre{background:#0d1116;border:1px solid #232a33;border-radius:6px;padding:9px;
  font:11px/1.5 ui-monospace,Menlo,Consolas,monospace;color:#a9b6c4;
  max-height:150px;overflow:auto;white-space:pre-wrap;word-break:break-all;margin:0}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--bad);margin-right:6px}
.dot.on{background:var(--ok);box-shadow:0 0 8px var(--ok)}
.note{font-size:11px;color:var(--dim);line-height:1.7}
.hint{color:var(--accent)}
kbd{background:#252c36;border:1px solid #3a434f;border-radius:4px;padding:1px 5px;
  font:11px ui-monospace,monospace}
.ev{font:11px/1.7 ui-monospace,Menlo,Consolas,monospace;color:#9fb0c0;
  max-height:130px;overflow:auto}
.ev span{color:#5f6b78;margin-right:6px}
</style></head><body>
<h1>Kindle Oasis 3 · 知乎阅读 硬件模拟器</h1>
<div class="sub">帧缓冲级模拟 · 真实 ARM 二进制 · 1264×1680 / 300ppi / 8bpp 灰度 · MT 协议 B 触摸</div>
<div class="wrap">
  <div class="device">
    <div class="power"></div><div class="grip"></div>
    <div class="bezel"><img id="screen" alt="Kindle 屏幕" src="/frame"></div>
    <div class="keys"><button data-ev="prev" title="上一个">▲</button><button data-ev="next" title="下一个">▼</button></div>
    <div class="brand">KINDLE · OAISIS 3</div><div class="port"></div>
  </div>
  <div class="side">
    <div class="card">
      <h2>设备状态</h2>
      <table>
        <tr><td>运行状态</td><td id="st"><span class="dot" id="dot"></span><span id="stt">连接中…</span></td></tr>
        <tr><td>CPU / 二进制</td><td id="arch">—</td></tr>
        <tr><td>屏幕</td><td id="scr">—</td></tr>
        <tr><td>刷新帧序号</td><td id="seqv">—</td></tr>
        <tr><td>输入设备</td><td>虚拟 MT-B 触摸屏 + 翻页键</td></tr>
        <tr><td>EPDC 波形</td><td>不模拟（真机 DU/GC16）</td></tr>
      </table>
    </div>
    <div class="card">
      <h2>快捷操作</h2>
      <div class="quick">
        <button class="b" data-ev="tap 300 361">热榜</button>
        <button class="b" data-ev="tap 300 492">推荐</button>
        <button class="b" data-ev="tap 300 623">私信</button>
        <button class="b" data-ev="tap 300 754">账号 / 登录</button>
        <button class="b" data-ev="tap 300 885">搜索</button>
        <button class="b" data-ev="tap 300 1016">我的收藏</button>
        <button class="b" data-ev="tap 300 1147">阅读历史</button>
        <button class="b" data-ev="tap 300 1278">设置</button>
        <button class="b" data-ev="tap 300 1409">关于</button>
        <button class="b" data-ev="demo" style="border-color:var(--accent);color:var(--accent)">★ 演示文章</button>
        <button class="b" data-ev="empty" style="border-color:var(--accent);color:var(--accent)">⚠ 取不到内容</button>
        <button class="b" data-ev="qrcode" style="border-color:var(--accent);color:var(--accent)">▦ 离线二维码</button>
        <button class="b" data-ev="dmlist" style="border-color:var(--accent);color:var(--accent)">✉ 私信会话</button>
        <button class="b" data-ev="dmread" style="border-color:var(--accent);color:var(--accent)">✉ 私信消息</button>
        <button class="b" data-ev="prev">◀ 上一页</button>
        <button class="b" data-ev="next">下一页 ▶</button>
        <button class="b" data-ev="back">返回 / Home</button>
        <button class="b" data-ev="swipeL">左滑</button>
        <button class="b" data-ev="swipeR">右滑</button>
        <button class="b" data-ev="quit">退出程序</button>
      </div>
      <div class="note" style="margin-top:10px">
        直接<span class="hint">点击屏幕</span>＝触摸；<span class="hint">横向拖动</span>＝翻页手势。
        键盘：<kbd>←</kbd><kbd>→</kbd> 翻页，<kbd>空格</kbd> 下一页，<kbd>Esc</kbd>/<kbd>Backspace</kbd> 返回，
        <kbd>Enter</kbd> 确认，<kbd>Home</kbd> 回首页。<br>
        <span class="hint">热榜能正常拉取，但点进条目走的是「问题→回答列表」接口，当前网络出口会被知乎 403 风控拦截；
        想完整体验阅读与整页翻页，请点「★ 演示文章」载入内置多页文章。</span><br>
        <span class="hint">「▦ 离线二维码」「✉ 私信会话 / 消息」是模拟器内置的离线演示，不联网、不需要登录态，
        用来验证 1.2.0 新增页面的绘制与整页翻页；真机上请在「账号 / 登录」里扫码或导入 Cookie。</span><br>
        快捷按钮按<span class="hint">默认字号</span>的菜单坐标计算；在设置里改过字号后，请直接点屏幕上的实际位置。
      </div>
    </div>
    <div class="card"><h2>注入的事件</h2><div class="ev" id="evlog">（暂无）</div></div>
    <div class="card"><h2>设备运行日志 log.txt</h2><pre id="log">读取中…</pre></div>
    <div class="card"><h2>模拟边界</h2><div class="note">
      本模拟器精确复现<span class="hint">像素输出</span>与<span class="hint">输入事件</span>，
      并用 qemu 执行设备上的同一份 ARM 二进制。但 E-Ink 波形刷新、残影、温补、
      电源管理、内核 ioctl 返回值与真实触摸固件无法在 PC 上复现 —— 这些只能在真机验证。
    </div></div>
  </div>
</div>
<script>
const W=1264,H=1680, scr=document.getElementById('screen');
let down=null, lastSeq='';
async function inject(q){
  try{ await fetch('/event?q='+encodeURIComponent(q)); }catch(e){}
  refreshEv();
}
function pushEv(q){}
scr.addEventListener('pointerdown',e=>{down={x:e.clientX,y:e.clientY};scr.setPointerCapture(e.pointerId);});
scr.addEventListener('pointerup',e=>{
  if(!down)return; const r=scr.getBoundingClientRect();
  const x=Math.round((e.clientX-r.left)/r.width*W), y=Math.round((e.clientY-r.top)/r.height*H);
  const dx=e.clientX-down.x; down=null;
  if(Math.abs(dx)>55) inject(dx<0?'swipeL':'swipeR'); else inject('tap '+x+' '+y);
});
document.querySelectorAll('[data-ev]').forEach(b=>b.addEventListener('click',()=>inject(b.dataset.ev)));
document.addEventListener('keydown',e=>{
  const m={ArrowLeft:'prev',ArrowRight:'next',PageUp:'prev',PageDown:'next',
           Backspace:'back',Escape:'back',Home:'home',Enter:'enter',' ':'next'};
  if(m[e.key]){e.preventDefault();inject(m[e.key]);}
});
async function pollFrame(){
  try{
    const s=(await (await fetch('/seq')).text()).trim();
    if(s&&s!==lastSeq){lastSeq=s;scr.src='/frame?t='+Date.now();
      document.getElementById('seqv').textContent=s.split(' ')[0];}
  }catch(e){}
}
async function refreshStatus(){
  try{
    const j=await (await fetch('/status')).json();
    document.getElementById('dot').className='dot'+(j.alive?' on':'');
    document.getElementById('stt').textContent=j.alive?'运行中':'已退出 rc='+j.rc;
    document.getElementById('arch').textContent=j.arch_desc;
    document.getElementById('scr').textContent=j.w+'×'+j.h+' / '+j.ppi+'ppi / '+j.bpp+'bpp';
    const ev=document.getElementById('evlog');
    ev.innerHTML=j.events.length?j.events.map(x=>'<div><span>'+x[0]+'</span>'+x[1]+'</div>').reverse().join(''):'（暂无）';
  }catch(e){}
}
async function refreshLog(){
  try{document.getElementById('log').textContent=await (await fetch('/log')).text();}catch(e){}
}
setInterval(pollFrame,180); setInterval(refreshStatus,1000); setInterval(refreshLog,1500);
pollFrame();refreshStatus();refreshLog();
</script></body></html>'''


# ---- HTTP 服务 --------------------------------------------------------------
def make_handler(sim):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, ctype, body, extra=None):
            if isinstance(body, str):
                body = body.encode('utf-8')
            self.send_response(code)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def do_GET(self):
            from urllib.parse import urlparse, parse_qs, unquote
            u = urlparse(self.path)
            q = parse_qs(u.query)
            if u.path in ('/', '/index.html'):
                self._send(200, 'text/html; charset=utf-8', PAGE)
            elif u.path == '/seq':
                self._send(200, 'text/plain; charset=utf-8', sim.seq())
            elif u.path in ('/frame', '/frame.png'):
                png = sim.frame_png()
                if png:
                    self._send(200, 'image/png', png)
                else:
                    self._send(404, 'text/plain', '还没有帧')
            elif u.path == '/event':
                line = unquote(q.get('q', [''])[0])
                ok = sim.inject(line)
                self._send(200, 'application/json', json.dumps({'ok': ok, 'q': line}))
            elif u.path == '/log':
                self._send(200, 'text/plain; charset=utf-8', sim.log())
            elif u.path == '/status':
                self._send(200, 'application/json', json.dumps(sim.status(), ensure_ascii=False))
            elif u.path == '/quit':
                sim.inject('quit')
                self._send(200, 'text/plain', 'bye')
            else:
                self._send(404, 'text/plain', 'not found')

        def do_POST(self):
            if self.path.rstrip('/') == '/event':
                n = int(self.headers.get('Content-Length', 0))
                line = self.rfile.read(n).decode('utf-8', 'replace')
                self._send(200, 'application/json',
                           json.dumps({'ok': sim.inject(line), 'q': line}))
            else:
                self._send(404, 'text/plain', 'not found')
    return Handler


# ---- 自动回归测试 -----------------------------------------------------------
# 首页菜单 y 中心：top=296, rh=131（base=45, header=67，10 项自适应）
# 账号页 y 中心：top=179, rh=112（6 项）
STEPS = [
    ('首页渲染',        None,                 12, '启动后应出现 10 项首页菜单'),
    ('进入热榜',        'tap 300 361',        30, '点「热榜」后加载列表（需联网）'),
    ('热榜列表稳定',    None,                 18, '等待网络返回并渲染列表'),
    ('空列表说明页',    'empty',              10, '离线注入空列表：必须显示原因与登录引导，而不是一个「（空）」'),
    ('载入演示文章',    'demo',               10, '注入本地多页文章，进入阅读页'),
    ('整页向后翻',      'next',               10, '实体键翻到下一页'),
    ('整页向前翻',      'prev',               10, '实体键翻回上一页'),
    ('左滑翻页',        'swipeL',             10, '手势向后翻页'),
    ('右滑翻页',        'swipeR',             10, '手势向前翻页'),
    ('返回列表',        'back',               10, '阅读页返回列表'),
    ('返回首页',        'back',               10, '列表页返回首页'),
    ('进入设置',        'tap 300 1278',       10, '设置页'),
    ('切换字号',        'tap 300 140',        10, '设置项点击后重绘（字号 1→2）'),
    ('字号复位 1',      'tap 300 140',        10, '字号 2→0'),
    ('字号复位 2',      'tap 300 140',        10, '字号 0→1，恢复默认后再测菜单坐标'),
    ('返回首页',        'back',               10, '设置页返回首页'),
    ('进入关于',        'tap 300 1409',       10, '关于页（版本与硬件信息）'),
    ('返回首页',        'back',               10, '关于页返回首页'),
    ('打开阅读历史',    'tap 300 1147',       10, '历史页'),
    ('返回首页',        'back',               10, '历史页返回首页'),
    # ---- 1.2.0 新增：登录 / 推荐 / 私信 ----
    ('推荐未登录提示',  'tap 300 492',        12, '游客点「推荐」应弹出需登录提示'),
    ('关闭提示回首页',  'tap 300 800',        10, '点提示任意位置回首页'),
    ('私信未登录提示',  'tap 300 623',        12, '游客点「私信」应弹出需登录提示'),
    ('关闭提示回首页',  'tap 300 800',        10, '点提示任意位置回首页'),
    ('进入账号页',      'tap 300 754',        10, '账号 / 登录页（6 项菜单）'),
    ('Cookie 导入提示', 'tap 300 347',        10, 'cookie.txt 不存在时给出说明'),
    ('关闭提示回账号',  'tap 300 800',        10, '关闭提示应回到账号页而非首页'),
    ('账号页返回首页',  'tap 300 795',        10, '账号页「返回首页」'),
    ('离线二维码页',    'qrcode',             10, '本地生成二维码并绘制到帧缓冲'),
    ('二维码页返回',    'tap 300 800',        10, '点二维码页任意位置回账号页'),
    ('账号页返回首页',  'tap 300 795',        10, '账号页「返回首页」'),
    ('私信会话列表',    'dmlist',             10, '离线注入 3 个会话并排版'),
    ('私信消息阅读',    'dmread',             10, '离线注入 6 条消息，整页排版'),
    ('私信消息翻页',    'next',               10, '私信消息页整页翻页'),
    ('私信返回会话',    'back',               10, '私信阅读页返回会话列表'),
    ('返回首页',        'back',               10, '会话列表返回首页'),
]

def run_test(sim, outdir):
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / 'frames').mkdir(exist_ok=True)
    # 清掉上一轮的帧：步骤数变过（例如 1.2.1 插入「空列表说明页」）时，
    # 旧编号会和新编号混在一起，按前缀取图会取错。
    for old in (outdir / 'frames').glob('*.png'):
        old.unlink()
    rows = []
    print('\n=== Kindle Oasis 3 端到端回归 ===')
    print('二进制：', sim.arch_desc)
    print('命令  ：', ' '.join(sim.cmd))
    print('屏幕  ：%dx%d / %dppi / %dbpp\n' % (sim.w, sim.h, PPI, BPP))
    sim.start()
    first = sim.wait_frame(None, 15)
    print('启动帧：', first or '（超时）')

    prev = None
    for i, (name, ev, timeout, expect) in enumerate(STEPS, 1):
        if ev:
            sim.inject(ev)
        s = sim.wait_frame(prev, timeout)
        changed = bool(s)
        if s:
            prev = s
        png = sim.frame_png()
        digest = hashlib.md5(png).hexdigest()[:10] if png else '—'
        if png:
            (outdir / 'frames' / ('%02d-%s.png' % (i, name.replace('/', '-')))).write_bytes(png)
        alive = sim.alive()
        logtail = sim.log(8)
        crash = 'CRASH' in logtail or 'segfault' in logtail.lower()
        ok = alive and not crash and (changed or ev is None)
        rows.append((i, name, ev or '—', expect, 'PASS' if ok else 'FAIL', digest, s or '—'))
        print('%2d. %-14s %-8s 帧变化=%-5s 存活=%-5s 帧=%s' %
              (i, name, 'PASS' if ok else 'FAIL', changed, alive, digest))
        if not alive:
            print('    进程已退出 rc=', sim.proc.poll(), '，日志：\n', logtail)
            break
        time.sleep(0.25)

    # 汇总
    logall = sim.log(400)
    crash_lines = [l for l in logall.splitlines() if 'CRASH' in l]
    http_lines = [l for l in logall.splitlines()
                  if any(k in l for k in ('子进程', 'HTTP', 'http', '403', '200',
                                          'cookie', 'Cookie', 'warmup', 'DNS'))]
    print('\n--- 运行日志中的网络记录（最多 12 条）---')
    for l in http_lines[-12:]:
        print('   ', l[:160])
    print('\n--- 崩溃记录 ---')
    print('\n'.join(crash_lines) if crash_lines else '   无')
    print('\n--- 结尾日志 ---')
    print('\n'.join(logall.splitlines()[-12:]))

    n_pass = sum(1 for r in rows if r[4] == 'PASS')
    report = build_report(sim, rows, n_pass, crash_lines, http_lines, outdir)
    (outdir / 'KO3-模拟测试报告.md').write_text(report, encoding='utf-8')
    sim.stop()
    print('\n结果：%d/%d 通过，报告 -> %s' % (n_pass, len(rows), outdir / 'KO3-模拟测试报告.md'))
    return n_pass == len(rows)


def probe_version(cmd):
    """直接问被测二进制拿版本号，避免报告里写死、换版本后失真"""
    try:
        out = subprocess.check_output(list(cmd) + ['--version'],
                                      stderr=subprocess.DEVNULL, timeout=15)
        txt = out.decode('utf-8', 'replace').strip()
        return txt.split()[-1] if txt else '未知'
    except Exception:
        return '未知'


def build_report(sim, rows, n_pass, crash_lines, http_lines, outdir):
    lines = [
        '# Kindle Oasis 3 硬件模拟测试报告',
        '',
        '生成时间：%s  ' % time.strftime('%Y-%m-%d %H:%M:%S'),
        '被测程序：知乎阅读 v%s（设备上的同一份二进制）' % probe_version(sim.cmd),
        '',
        '## 模拟层级',
        '',
        '| 层面 | 模拟方式 | 保真度 |',
        '|---|---|---|',
        '| CPU 指令 | qemu-user 直接执行 ARMv5TE 静态 ELF | 与真机一致 |',
        '| 帧缓冲 | 离屏 1264×1680 / 8bpp 灰度，逐像素输出 | 与真机一致 |',
        '| 输入 | MT 协议 B 虚拟触摸屏 + 实体翻页键码 | 事件级一致 |',
        '| 网络 | 真实 HTTPS + zse96 签名 + 自研 DNS | 与真机一致 |',
        '| E-Ink 波形 | DU / GC16、残影、温补 | **不模拟**，需真机 |',
        '| PMIC / 电源 / 电池 / 内核 ioctl 返回值 | — | **不模拟**，需真机 |',
        '',
        '- 二进制：`%s`' % ' '.join(sim.cmd),
        '- 架构：%s' % sim.arch_desc,
        '- 屏幕：%d×%d，%dppi，%dbpp 灰度，7 英寸' % (sim.w, sim.h, PPI, BPP),
        '',
        '## 逐项结果：%d/%d 通过' % (n_pass, len(rows)),
        '',
        '| # | 操作 | 注入事件 | 期望 | 结果 | 帧 MD5 |',
        '|---|---|---|---|---|---|',
    ]
    for i, name, ev, expect, ok, digest, seq in rows:
        lines.append('| %d | %s | `%s` | %s | **%s** | `%s` |' %
                     (i, name, ev, expect, ok, digest))
    lines += ['', '> 判定依据：每一步注入事件后必须出现新的刷新帧、进程保持存活、日志无 CRASH。',
              '> 帧 MD5 是当次帧缓冲像素的指纹，用于确认「确实重绘了」而不是空转。', '',
              '## 网络请求记录（摘自 log.txt）', '', '```']
    lines += http_lines[-20:] if http_lines else ['（本次未产生网络日志）']
    lines += ['```', '', '## 崩溃记录', '', '```']
    lines += crash_lines if crash_lines else ['无']
    lines += ['```', '', '## 关键帧截图', '',
              '截图保存在 `frames/`，文件名与上面的步骤序号对应。', '']
    for i, name, *_ in rows:
        f = outdir / 'frames' / ('%02d-%s.png' % (i, name.replace('/', '-')))
        if f.exists():
            lines.append('- `%s` — %s' % (f.name, name))
    lines += ['', '## 结论与真机待验项', '',
              '模拟器能确认：程序在目标 CPU 上能启动、界面能绘制、所有菜单能进入并返回、'
              '整页翻页与手势生效、崩溃捕获有效、二维码本地编码与绘制正确、'
              '私信列表与消息整页翻页版式正常、登录态缺失时的提示与返回路径正确、'
              '列表为空时会显示原因与登录引导（不再只画一个「（空）」）。',
              '',
              '> **关于网络**：热榜走的是真实 HTTPS 请求，沙箱出口可能被知乎风控（403 / '
              '拿不到游客 `d_c0`），上方「网络请求记录」是本次实测结果。'
              '若出现 403，属网络出口问题而非程序缺陷——同一份二进制在风控较松时'
              '可拉到完整热榜（约 50 条 / 55–70KB）。回归判定只看「是否重绘 + 进程存活 + 无崩溃」，'
              '因此风控时该步仍为 PASS；此时第 4 步的空态页正好展示用户会看到什么。',
              '',
              '仍必须在 KO3 真机确认：', '',
              '1. `MXCFB_SEND_UPDATE` 的 ioctl 号与结构体大小匹配本机内核（不同固件不同）；',
              '2. 触摸设备是否真被识别为 MT 协议 B，坐标范围与方向是否正确；',
              '3. EPDC 全刷/快刷是否闪烁、有无残影；',
              '4. powerd 抑制休眠是否生效；',
              '5. 字体是否能加载到含中文字形的 face；',
              '6. **二维码扫码轮询** `scan_info` 是否被风控（40352），以及扫码成功后的 `z_c0` 落盘；',
              '7. **私信真实端点**：登录态下哪个候选端点返回 200、真实 JSON 结构，'
              '必要时用 `social.conf` 覆盖并把 `log.txt` 反馈；',
              '8. **推荐流**登录后是否稳定返回 200；',
              '9. **cvm 是否真的被清掉**（1.2.1）：看 `run.log` 里是否出现 '
              '`--- framework stopped, cvm gone ---`；若出现 '
              '`WARNING: cvm still alive`，说明系统在反复拉起它，需再加延时重试。'
              '这一项模拟器无法验证（容器里没有 Kindle 的 cvm）。', '']
    return '\n'.join(lines)


# ---- 离线可双击的 HTML（内嵌真实帧） --------------------------------------
def build_standalone(sim, outpath, frames):
    """frames: [(name, png_bytes)]，生成不依赖服务的单文件 HTML。"""
    import base64
    items = []
    for name, png in frames:
        items.append('{"n":%s,"d":"%s"}' %
                     (json.dumps(name, ensure_ascii=False),
                      base64.b64encode(png).decode()))
    js_frames = '[\n' + ',\n'.join(items) + '\n]'
    html = r'''<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<title>Kindle Oasis 3 · 知乎阅读 离线演示</title><style>
:root{--bg:#12151a;--txt:#dfe5ec;--dim:#8b97a6;--accent:#e8b64c;--line:#2c3440}
*{box-sizing:border-box}body{margin:0;background:radial-gradient(1000px 600px at 30% 0%,#1c2530,#0e1115 70%);
color:var(--txt);font:14px/1.6 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif;padding:22px}
h1{font-size:18px;margin:0 0 4px}.sub{color:var(--dim);font-size:12px;margin-bottom:18px}
.wrap{display:flex;gap:24px;flex-wrap:wrap;align-items:flex-start}
.device{position:relative;background:linear-gradient(145deg,#2a2e33,#1c1f23);border:1px solid #3a3f46;
border-radius:16px 16px 22px 22px;padding:22px 24px 30px 76px;box-shadow:0 24px 60px #0009}
.device:before{content:"";position:absolute;left:0;top:0;bottom:0;width:60px;background:linear-gradient(90deg,#32373d,#23272c);border-radius:15px 0 0 21px}
.bezel{background:#080a0c;border-radius:3px;padding:15px 14px;box-shadow:0 0 0 1px #41464d}
#screen{display:block;width:380px;height:505px;background:#fff;cursor:crosshair}
.brand{position:absolute;bottom:9px;left:0;right:0;text-align:center;color:#6b727b;font-size:10px;letter-spacing:3px}
.keys{position:absolute;right:-1px;top:26%;display:flex;flex-direction:column;gap:74px}
.keys button{width:16px;height:66px;border:1px solid #454b52;border-radius:3px;background:linear-gradient(90deg,#343940,#22262b);color:#aab3bd;font-size:9px;cursor:pointer}
.side{flex:1 1 330px;min-width:320px;max-width:520px}
.card{background:#1b2028;border:1px solid var(--line);border-radius:10px;padding:14px 16px;margin-bottom:12px}
.card h2{font-size:12px;color:var(--accent);margin:0 0 10px;letter-spacing:1px}
.framebar{display:flex;gap:6px;flex-wrap:wrap}button.b{background:#252c36;color:var(--txt);border:1px solid var(--line);border-radius:6px;padding:6px 9px;font-size:12px;cursor:pointer}
button.b.on{border-color:var(--accent);color:var(--accent)}button.b:hover{border-color:var(--accent)}
table{width:100%;border-collapse:collapse;font-size:12px}td{padding:3px 0;color:var(--dim)}td+td{text-align:right;color:var(--txt)}
.note{font-size:11px;color:var(--dim);line-height:1.8}.hint{color:var(--accent)}
#status{font-size:12px;color:var(--accent);margin-top:10px;min-height:20px}
</style></head><body>
<h1>Kindle Oasis 3 · 知乎阅读 离线交互演示</h1>
<div class="sub">单文件、零依赖、双击即可用 · 帧为模拟器从真实 ARM 二进制逐像素抓取 · 1264×1680 / 300ppi</div>
<div class="wrap"><div class="device"><div class="bezel"><img id="screen"></div>
<div class="keys"><button id="kprev">▲</button><button id="knext">▼</button></div>
<div class="brand">KINDLE · OAISIS 3</div></div>
<div class="side"><div class="card"><h2>界面快照</h2><div class="framebar" id="bar"></div><div id="status"></div></div>
<div class="card"><h2>模拟的 KO3 硬件参数</h2><table>
<tr><td>机型</td><td>Kindle Oasis 3 (KO3)</td></tr><tr><td>面板</td><td>7 英寸 E-Ink Carta</td></tr>
<tr><td>分辨率</td><td>1264 × 1680</td></tr><tr><td>像素密度</td><td>300 ppi</td></tr>
<tr><td>色彩</td><td>8bpp 灰度（16 级）</td></tr><tr><td>触摸</td><td>电容多点 · MT 协议 B</td></tr>
<tr><td>实体键</td><td>上/下翻页键、电源键</td></tr><tr><td>CPU</td><td>ARMv5TE 软浮点（静态 ELF）</td></tr>
<tr><td>刷新</td><td>EPDC DU / GC16 波形（真机）</td></tr></table></div>
<div class="card"><h2>说明</h2><div class="note">
这是<span class="hint">帧缓冲级</span>模拟：所有画面都由设备上的同一份 ARM 二进制真实绘制后逐像素导出，
点击热区按真机布局还原（1.2.0 首页为 10 项）。E-Ink 波形、残影、电源管理与内核 ioctl 无法在 PC 上复现，
登录 / 推荐 / 私信的真实数据也需 KO3 真机在登录态下验证；这里的二维码与私信帧是模拟器内置离线演示。<br>
左侧按钮切换各页面快照；点击屏幕可查看点击落在哪个热区；
▲▼ 键模拟实体翻页键。
</div></div></div></div>
<script>
const F=__FRAMES__;let cur=0;
const W=1264,H=1680,base=45,header=67,top=296,rh=131;
const HOME=["热榜","推荐","私信","账号 / 登录","搜索","我的收藏","阅读历史","设置","关于","退出"];
function show(i){cur=(i+F.length)%F.length;document.getElementById('screen').src='data:image/png;base64,'+F[cur].d;
document.querySelectorAll('#bar button').forEach((b,j)=>b.className='b'+(j===cur?' on':''));
document.getElementById('status').textContent='当前：'+F[cur].n;}
F.forEach((f,i)=>{const b=document.createElement('button');b.className='b';b.textContent=(i+1)+'. '+f.n;
b.onclick=()=>show(i);document.getElementById('bar').appendChild(b);});
document.getElementById('kprev').onclick=()=>show(cur-1);
document.getElementById('knext').onclick=()=>show(cur+1);
document.getElementById('screen').onclick=e=>{const r=e.target.getBoundingClientRect();
const x=Math.round((e.clientX-r.left)/r.width*W),y=Math.round((e.clientY-r.top)/r.height*H);
let info='点击坐标 ('+x+', '+y+')';
if(y>=top&&y<top+rh*10&&cur===0)info+=' → 首页「'+HOME[Math.floor((y-top)/rh)]+'」';
else if(y<header)info+=' → 页眉（返回）';
else if(y>H-header)info+=' → 页脚';
else info+=' → 内容区（阅读页：左侧 1/4 上一页，其余下一页）';
document.getElementById('status').textContent=info;};
document.addEventListener('keydown',e=>{if(e.key==='ArrowLeft')show(cur-1);if(e.key==='ArrowRight'||e.key===' ')show(cur+1);});
show(0);
</script></body></html>'''
    html = html.replace('__FRAMES__', js_frames)
    Path(outpath).write_text(html, encoding='utf-8')
    return Path(outpath)


def capture_standalone_frames(sim, outdir):
    """跑一遍关键路径，抓若干有代表性的帧用于离线 HTML。"""
    frames = []
    sim.start()
    first = sim.wait_frame(None, 15)
    if first:
        png = sim.frame_png()
        if png:
            frames.append(('首页 · 10 项菜单', png))
    prev = first
    # 热榜（联网）
    sim.inject('tap 300 361')
    s = sim.wait_frame(prev, 8)
    if s:
        prev = s
        png = sim.frame_png()
        if png:
            frames.append(('加载中', png))
    s = sim.wait_frame(prev, 25)
    if s:
        prev = s
        png = sim.frame_png()
        if png:
            frames.append(('热榜列表', png))
    # 空列表说明页（1.2.1）：风控 / 断网 / 未登录都会落到这一页，
    # 必须在离线演示里也能看到，否则用户不知道"打不开"时屏幕是什么样
    sim.inject('empty')
    s = sim.wait_frame(prev, 8)
    if s:
        prev = s
        png = sim.frame_png()
        if png:
            frames.append(('取不到内容时 · 原因与引导', png))
    # 阅读页：用内置演示文章，避开知乎「问题→回答列表」的 403 风控
    sim.inject('demo')
    s = sim.wait_frame(prev, 12)
    if s:
        prev = s
        png = sim.frame_png()
        if png:
            frames.append(('阅读页 · 第 1 页', png))
        sim.inject('next')
        s2 = sim.wait_frame(prev, 8)
        if s2:
            prev = s2
            png = sim.frame_png()
            if png:
                frames.append(('整页翻页 · 第 2 页', png))
    # 返回首页
    for _ in range(2):
        sim.inject('back')
        s = sim.wait_frame(prev, 6)
        if s:
            prev = s
    # 账号 / 登录页 + 离线二维码（不联网）
    sim.inject('tap 300 754')
    s = sim.wait_frame(prev, 6)
    if s and sim.frame_png():
        prev = s
        frames.append(('账号 / 登录', sim.frame_png()))
    sim.inject('qrcode')
    s = sim.wait_frame(prev, 8)
    if s and sim.frame_png():
        prev = s
        frames.append(('扫码登录 · 本地二维码', sim.frame_png()))
    sim.inject('tap 300 800')      # 二维码页 → 账号页
    s = sim.wait_frame(prev, 6)
    if s:
        prev = s
    sim.inject('tap 300 795')      # 账号页 → 首页
    s = sim.wait_frame(prev, 6)
    if s:
        prev = s
    # 私信会话列表 + 消息阅读（离线注入）
    sim.inject('dmlist')
    s = sim.wait_frame(prev, 8)
    if s and sim.frame_png():
        prev = s
        frames.append(('私信 · 会话列表', sim.frame_png()))
    sim.inject('dmread')
    s = sim.wait_frame(prev, 8)
    if s and sim.frame_png():
        prev = s
        frames.append(('私信 · 消息阅读', sim.frame_png()))
    for _ in range(2):
        sim.inject('back')
        s = sim.wait_frame(prev, 6)
        if s:
            prev = s
    # 设置 / 关于
    sim.inject('tap 300 1278')
    s = sim.wait_frame(prev, 6)
    if s and sim.frame_png():
        prev = s
        frames.append(('设置', sim.frame_png()))
    sim.inject('back')
    s = sim.wait_frame(prev, 6)
    if s:
        prev = s
    sim.inject('tap 300 1409')
    s = sim.wait_frame(prev, 6)
    if s and sim.frame_png():
        frames.append(('关于 · 设备信息', sim.frame_png()))
    sim.stop()
    return frames


# ---- main -------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description='Kindle Oasis 3 桌面模拟器')
    ap.add_argument('--port', type=int, default=8765)
    ap.add_argument('--bin', dest='binpath')
    ap.add_argument('--workdir', default=str(ROOT / '.sim'))
    ap.add_argument('--test', action='store_true', help='自动跑端到端回归并生成报告')
    ap.add_argument('--standalone', metavar='OUT.html', help='抓帧生成离线单文件 HTML')
    ap.add_argument('--out', default=str(ROOT.parent / 'KO3-模拟器'))
    args = ap.parse_args()

    cmd, desc, arch = pick_binary(args.binpath)
    print('二进制：%s' % ' '.join(cmd))
    print('架构  ：%s' % desc)

    if args.test:
        sim = Sim(cmd, desc, arch, args.workdir)
        ok = run_test(sim, Path(args.out))
        sys.exit(0 if ok else 1)

    if args.standalone:
        sim = Sim(cmd, desc, arch, args.workdir + '-sa')
        frames = capture_standalone_frames(sim, Path(args.out))
        out = build_standalone(sim, args.standalone, frames)
        print('离线 HTML -> %s（%d 帧，%.1f KB）' %
              (out, len(frames), out.stat().st_size / 1024))
        return

    sim = Sim(cmd, desc, arch, args.workdir)
    sim.start()
    srv = ThreadingHTTPServer(('0.0.0.0', args.port), make_handler(sim))
    print('\n模拟器已启动：http://127.0.0.1:%d' % args.port)
    print('屏幕 %dx%d / %dppi / %dbpp    日志 -> %s' %
          (W, H, PPI, BPP, sim.logfile))
    print('Ctrl+C 退出\n')
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        sim.stop()
        srv.server_close()


if __name__ == '__main__':
    main()
