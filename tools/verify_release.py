#!/usr/bin/env python3
"""发布包自检：结构、权限、配置合法性、二进制属性、文档引用。"""
import json
import os
import struct
import sys
import xml.etree.ElementTree as ET
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VER = "1.3.8"
ZIP = os.path.join(ROOT, "release", "知乎阅读-Kindle-%s.zip" % VER)

ok = True
problems = []


def names_base(z, base):
    return z.namelist()


def check(cond, msg):
    global ok
    print(("[ok]  " if cond else "[!!]  ") + msg)
    if not cond:
        ok = False
        problems.append(msg)


def main():
    check(os.path.exists(ZIP), "发布包存在：%s" % ZIP)
    if not os.path.exists(ZIP):
        return

    z = zipfile.ZipFile(ZIP)
    names = z.namelist()
    base = "知乎阅读-Kindle-%s/" % VER

    need = [
        "extensions/zhihureader/config.xml",
        "extensions/zhihureader/menu.json",
        "extensions/zhihureader/app/zhihu",
        "extensions/zhihureader/bin/start.sh",
        "extensions/zhihureader/bin/install.sh",
        "extensions/zhihureader/bin/log.sh",
        "README.md",
        "安装说明.txt",
        "LICENSE-AGPL-3.0.txt",
    ]
    for n in need:
        check(base + n in names, "包含 %s" % n)

    # 可执行权限
    for n in ("extensions/zhihureader/app/zhihu", "extensions/zhihureader/bin/start.sh"):
        info = z.getinfo(base + n)
        mode = info.external_attr >> 16
        check(bool(mode & 0o111), "%s 可执行（mode %o）" % (n, mode & 0o777))

    # 二进制属性
    data = z.read(base + "extensions/zhihureader/app/zhihu")
    check(data[:4] == b"\x7fELF", "app/zhihu 是 ELF")
    ei_class = data[4]
    e_machine = struct.unpack_from("<H", data, 18)[0]
    check(ei_class == 1, "32 位 ELF")
    check(e_machine == 40, "ARM 架构（e_machine=40）")
    # 静态链接：无 PT_INTERP 段
    e_phoff = struct.unpack_from("<I", data, 28)[0]
    e_phnum = struct.unpack_from("<H", data, 44)[0]
    has_interp = False
    for i in range(e_phnum):
        p_type = struct.unpack_from("<I", data, e_phoff + i * 32)[0]
        if p_type == 3:
            has_interp = True
    check(not has_interp, "静态链接（无 PT_INTERP）")
    check(len(data) > 1_000_000, "二进制大小 %d 字节" % len(data))

    # 这一次修复的几个要点，必须在包里，防止"改了源码但打包用了旧产物"
    check(VER.encode() in data, "二进制内含版本号 %s" % VER)
    sh = z.read(base + "extensions/zhihureader/bin/start.sh").decode("utf-8", "replace")
    check("cvm_pid()" in sh, "start.sh 含 cvm_pid 清理")
    check("kill -9" in sh, "start.sh 含 kill -9")
    check("/sys/power/wake_lock" in sh, "start.sh 含内核 wakelock")
    check("--grabtest" in sh, "start.sh 用 --grabtest 决定要不要接管系统界面")
    check("framework=auto" in sh or "framework auto" in sh, "start.sh 默认 framework=auto")
    check("grab=busy" in sh, "start.sh 认得 grab=busy（触摸被独占）")
    check(".framework_stopped" in sh, "start.sh 用标记文件避免重复 start framework")
    check("suspend_stats" in sh, "start.sh 记录挂起统计")
    check("kmsg.log" in sh, "start.sh 把 dmesg 尾巴落盘")
    check("preventScreenSaver" in sh, "start.sh 含防休眠心跳")
    fbc = z.read(base + "source/src/fb.c").decode("utf-8", "replace")
    check("MXCFB_SEND_UPDATE _IOWR" in fbc, "fb.c 的 SEND_UPDATE 用 _IOWR")
    check("eips_shot" in fbc, "fb.c 有 eips 刷新后路")
    check("png_write_gray8" in fbc, "fb.c 用 PNG 把整页交给 eips")
    check("unknown_errno" in fbc, "fb.c 先量\"未知命令\"基线再解释 EINVAL")
    check(base + "source/src/png.c" in names, "包含 source/src/png.c")
    check("ioc_dir(ZH_IOC_RW" in fbc, "fb.c 探测用 _IOWR 方向位")
    check("fb_trace" in fbc, "fb.c 含首帧 trace")
    check("fb_guard" in fbc, "fb.c 含屏幕归属守护")
    check("fb_sample_sig" in fbc, "fb.c 含 framebuffer 采样哈希")
    check("SEND_UPDATE 耗时" in fbc, "fb.c 记录刷新 ioctl 耗时")
    inc = z.read(base + "source/src/input.c").decode("utf-8", "replace")
    check("EVIOCGRAB" in inc, "input.c 抢触摸屏独占（EVIOCGRAB）")
    check("grab=ok" in inc, "input.c 记录 grab 结果")
    mnc = z.read(base + "source/src/main.c").decode("utf-8", "replace")
    check("atexit(restore_atexit)" in mnc, "main.c 退出时自己还原系统界面")
    check("setsid()" in mnc, "main.c 自成会话（躲开进程组 kill）")
    check("--grabtest" in mnc, "main.c 提供 --grabtest")
    uic = z.read(base + "source/src/ui.c").decode("utf-8", "replace")
    check("present_ui" in uic, "ui.c 菜单用快刷（present_ui）")
    check("主循环卡顿" in uic, "ui.c 主循环看门狗埋点")
    check("redraw_current" in uic, "ui.c 能重画当前屏（被抢屏后自愈）")

    # 1.2.5：触摸随旋转、影子回灌、详情回退、接口自检
    check("sync_touch" in uic or "input_set_rotate(g_cfg.rotate" in uic,
          "ui.c 改旋转时同步触摸（1.2.5 核心）")
    check("fb_shadow_enable" in uic, "ui.c 启用影子副本（抢屏后整页回灌）")
    check("fb_restore" in fbc, "fb.c 有整页回灌")
    check("shadow_sync" in fbc, "fb.c 每次刷屏后同步影子副本")
    api = z.read(base + "source/src/api.c").decode("utf-8", "replace")
    check("api_answer_detail2" in api, "api.c 详情带上下文 Referer + 404 回退")
    check("UA_PC" in api, "api.c 网页端接口用桌面 UA")
    check("api_ids_selftest" in api, "api.c 有 URL->id 自检")
    check("api_last_head" in api, "api.c 记录响应体前 200 字节（404 证据）")
    check("--apitest" in mnc, "main.c 提供 --apitest 接口体检")
    check("json_get_idstr" in mnc, "main.c 自检 19 位 id（[id64]）")
    jsc = z.read(base + "source/src/json.c").decode("utf-8", "replace")
    check("v->raw = xstrdup" in jsc, "json.c 保留数字原始字面量（19 位 id 不丢精度）")
    check("json_get_idstr" in api, "api.c 用 idstr 取 id（不用 double/long）")
    check("input_set_rotate" in inc, "input.c 触摸随旋转（pix_off 的逆变换）")
    check("input_rotate_selftest" in inc, "input.c 四方向映射自检")
    check("--apitest" in sh, "start.sh 有 apitest 菜单入口")
    check(".want_stop" in sh, "start.sh 支持程序请求停掉系统界面")


    # 1.2.6：翻页键独占、回答摘要标题、详情元信息、推荐多页、私信端点、
    #         收藏接线、真图（JPEG 解码）
    check("input_key_grab_rule" in inc, "input.c 有翻页键独占规则（含翻页键且不含电源键）")
    check("grabkeys" in inc, "input.c 支持 grabkeys=0 关掉按键独占")
    check("has_power" in inc, "input.c 探测按键设备是否含电源键")
    check("html_plain" in api, "api.c 回答无标题时用摘要（html_plain）")
    check("item_count" in api, "api.c 收藏夹条目数取 item_count")
    check("created_time" in api, "api.c 详情解析 created_time")
    check("api_format_date" in api, "api.c 有 Unix 时间格式化")
    check("api_all_collections" in api, "api.c 有「全部收藏」合并")
    check("api_fetch_bin" in api, "api.c 能取图片二进制（带 Cookie/Referer）")
    check("api_list_save" in api, "api.c ApiList 落盘（父子进程传列表）")
    check("job_recommend" in uic, "ui.c 推荐多页合并（job_recommend）")
    check("REC_PAGES" in uic, "ui.c 推荐页数上限（REC_PAGES）")
    check("act_collections" in uic, "ui.c 收藏真正接线（act_collections）")
    check("act_allfav" in uic, "ui.c 有「全部收藏」入口")
    check("doc_images_prepare" in uic, "ui.c 打开文章时只建占位（不再同步等图）")
    check("doc_insert_front" in uic, "ui.c 正文顶部插元信息（BLK_META）")
    check("draw_reader_image" in uic, "ui.c 阅读页绘制图片")
    check("显示图片" in uic, "ui.c 设置项改名为「显示图片」")
    check(base + "source/src/img.c" in names, "包含 source/src/img.c（JPEG 解码）")
    check(base + "source/src/img.h" in names, "包含 source/src/img.h")
    imgc = z.read(base + "source/src/img.c").decode("utf-8", "replace")
    check("idct_block" in imgc, "img.c 有定点 IDCT（armv5te 无浮点）")
    check("box_down" in imgc, "img.c 解码后盒式缩小")
    check("渐进式 JPEG 暂不支持" in imgc, "img.c 明确拒绝渐进式 JPEG")
    check("img_cache_save" in imgc, "img.c 图片落盘缓存")
    ctc = z.read(base + "source/src/content.c").decode("utf-8", "replace")
    check("data-actualsrc" in ctc, "content.c 取知乎真图地址（data-actualsrc）")
    check("BLK_IMAGE" in ctc, "content.c 插入图片块")
    lc = z.read(base + "source/src/layout.c").decode("utf-8", "replace")
    check("block_image_h" in lc, "layout.c 图片按高度占位")
    soc = z.read(base + "source/src/social.c").decode("utf-8", "replace")
    check("/api/v4/inbox" in soc, "social.c 私信端点改为 /api/v4/inbox")
    check("participant" in soc, "social.c 解析 participant 昵称")
    check("social_parse_threads" in soc, "social.c 会话解析可离线自检")
    check("social_parse_threads" in mnc, "main.c 自检会话解析（[dm2]）")
    check("img_selftest" in mnc, "main.c 自检 JPEG 解码（[img]）")
    check("input_key_grab_rule" in mnc, "main.c 自检翻页键规则（[grab]）")

    # 1.2.7：右下角统一返回、阅读页大标题、图片异步、列表翻页/刷新、
    #         私信与收藏端点修正、KUAL 二级菜单
    check("footer_btn" in uic, "ui.c 有统一页脚按钮（footer_btn）")
    check("BTN_BACK" in uic, "ui.c 每个次级页面都加「返回」按钮")
    check("footer_hit" in uic, "ui.c 按钮可点击（footer_hit）")
    check("reader_header_h" in uic, "ui.c 阅读页页眉加高以容纳大标题")
    check("draw_reader_header" in uic, "ui.c 阅读页标题单独绘制（更大更粗）")
    check("reader_relayout_keep" in uic, "ui.c 图片到货后重排仍锚定阅读位置")
    check("doc_images_prepare" in uic, "ui.c 未下到的图先排成占位块")
    check("draw_img_placeholder" in uic, "ui.c 占位框画出「图片加载中」与进度")
    check("img_start" in uic, "ui.c 图片后台进程（img_start）")
    check("img_tick" in uic, "ui.c 主循环轮询图片进度（img_tick）")
    check("doc_images_give_up" in uic, "ui.c 全部失败后回退文字占位")
    check("list_page" in uic, "ui.c 列表整页翻页（list_page）")
    check("BTN_REFRESH" in uic, "ui.c 推荐列表有刷新按钮")
    check("sim_seed_image(g.doc_refs.v[0])" in uic, "ui.c 模拟器可延迟图片验证异步链路")
    check("members/self/favlists" in api, "api.c 收藏夹改用 members/self/favlists")
    check("api_last_next" in api, "api.c 保留 paging.next")
    check("api_recommend_next" in api, "api.c 按 next 续页拉推荐")
    check("fetch_bin_once" in api, "api.c 图片下载分级重试")
    check("_1440w.jpg" in api, "api.c 图片失败时改后缀转码重试")
    check("messages_at" in soc, "social.c 优先用会话自带 url 取消息")
    check("pick_url" in soc, "social.c 保存会话自带消息地址")
    check("--dmprobe" in mnc, "main.c 提供 --dmprobe（私信端点诊断）")
    check("[imgp ]" in mnc, "main.c 自检占位块排版（[imgp]）")
    check("[dm3  ]" in mnc, "main.c 自检会话 url 保留（[dm3]）")
    check("[fav2 ]" in mnc, "main.c 自检收藏夹列表解析（[fav2]）")
    check("[next ]" in mnc, "main.c 自检 paging.next（[next]）")
    check("dmprobe" in sh, "start.sh 有「私信端点诊断」入口")

    # 1.2.8：残影/发虚（刷新方式）与图片三档、缓存键
    check('"-f", "-g"' in fbc, "fb.c eips 换屏走 -f -g 全刷（不再只发局部更新）")
    check("use_du" in fbc, "fb.c DU 波形改为显式开关（默认不用）")
    check("want_full" in fbc, "fb.c 局部刷新每 3 次补一次全刷")
    check("present_screen" in uic, "ui.c 换屏统一走全刷（present_screen）")
    check("fast_next" in uic, "ui.c 只有翻页允许局部刷新（fast_next）")
    check("eipsclean" in fbc, "fb.c 支持 eipsclean=0（退回全局部刷新）")
    check("点按加载此图" in uic, "ui.c 点按档占位写「点按加载此图」")
    check("reader_img_hit" in uic, "ui.c 阅读页图片占位可命中（点按加载）")
    check("img_load_one" in uic, "ui.c 单张图片下载任务（img_load_one）")
    check("\\x02" in uic, "ui.c 点按档用 \\x02 前缀与自动档区分")
    check("img_cache_fill(refs->v[i], IMG_CACHE_W" in uic, "ui.c 存盘后按缓存键回读校验")
    check("cache_path(path, sizeof(path), url, IMG_CACHE_W)" in imgc,
          "img.c 缓存文件名固定用 IMG_CACHE_W（1.2.7 的图片全灭根因）")
    check("[imod ]" in mnc, "main.c 自检三档图片模式（[imod]）")

    # 1.2.9：状态栏开关、推荐条数、PNG/WebP、文章分目录缓存、Regal、
    #         旋转跟随、私信端点修正、KUAL 精简、标题只在第一页
    check("showbar" in uic, "ui.c 设置里有「系统状态栏」开关（showbar）")
    check("showbar" in sh, "start.sh 读 showbar（1.2.14 起正面隐藏：pillow+awesome，仍不杀 cvm）")
    check("awesome_reassert" in sh and "awesome_restore" in sh and ".awesome_stopped" in sh,
          "start.sh 每 5 秒重压 + cleanup 兜底恢复（1.2.14 正面方案，无看门狗风险）")
    check(".want_stop" in sh and "自动升级 stop 已移除" in sh, "start.sh 已移除 .want_stop 自动升级 stop 机制")
    check(".want_stop" not in uic or "已废弃" in uic, "ui.c 不再写 .want_stop（1.2.10 不主动请求杀 cvm）")
    check("g_cfg.rec" in uic and "rec=%d" in uic, "ui.c 推荐条数 6/12/18 可配置并写回 config")
    check("limit=%d" in api.replace("%", "%") or "limit=%d" in api, "api.c 推荐接口支持自定义 limit")
    check("stbi_load_from_memory" in imgc, "img.c 支持 PNG/GIF/BMP（stb_image）")
    check("WebPDecodeYUV" in imgc, "img.c 支持 WebP（libwebp）")
    check("IMG_MAX_PIXELS" in imgc, "img.c 超大图设上限（防卡死）")
    check("img_cache_set_article" in imgc and "img_cache_set_article" in uic,
          "img.c/ui.c 图片缓存按文章分目录")
    check("img_cache_count" in imgc, "img.c 能统计图片缓存文件数")
    check("cache_rm_rec" in api, "api.c「清空缓存」递归删除子目录")
    check("WAVEFORM_MODE_REAGL" in fbc, "fb.c 有 Regal（REAGL=0x8）波形")
    check('"-w", "8"' in fbc, "fb.c eips 路径尝试 -w 8（Regal）")
    check("fb_poll_rotate" in fbc and "fb_poll_rotate" in uic, "fb.c/ui.c 硬件旋转跟随（触摸同步）")
    check("autorot" in fbc, "fb.c 支持 autorot=0 关闭旋转跟随")
    check('/api/v4/messages?thread_id={id}' in soc, "social.c 私信消息首选实测存在的端点")
    check("social_msg_candidate" in soc, "social.c 端点候选可被自检钉死")
    check("[dme  ]" in mnc, "main.c 自检私信端点候选（[dme]）")
    check("[lay2 ]" in mnc, "main.c 自检首页/后续页两种页高（[lay2]）")
    check("layout_build2" in uic, "ui.c 阅读页首页与后续页分开排（layout_build2）")
    check("layout_page_top" in uic, "ui.c 用 layout_page_top 换算页内位置")
    check(any("img_test_webp.h" in n for n in names_base(z, base)), "源码含 WebP 测试图头文件")

    # 1.2.10：看门狗重启修复——showbar/framework 解耦、stop 兜底、
    #          崩溃恢复 framework、SEND_UPDATE breadcrumb
    check("stop_expire_loop" in sh, "start.sh stop 兜底：framework=stop 超时自动恢复共存")
    check("fwback" in sh, "start.sh 兜底时长可配（fwback=，默认 600s）")
    check(".stop_released" in sh, "start.sh 兜底恢复后停手（不再掐 cvm）")
    check("framework 是我们停的" in mnc, "main.c 崩溃处理器 _exit 前恢复 framework")
    check("即将 SEND_UPDATE" in fbc, "fb.c SEND_UPDATE 前写 breadcrumb（panic 定位）")
    check("FBIOPAN_DISPLAY branch" in fbc and "#warning" in fbc, "fb.c FBIOPAN 分支编译期警示")
    check("fw=%s" in uic, "ui.c 心跳带 framework 状态（fw=coexist/stop Ns）")
    check('g_cfg.showbar = 0;' in uic and "默认隐藏" in uic,
          "ui.c showbar 默认 0（1.2.14 正面方案已无看门狗风险）")

    # 1.2.11：真机三修——排版空页、标题折行、回答列表三行预览
    layc = z.read(base + "source/src/layout.c").decode("utf-8", "replace")
    apic = z.read(base + "source/src/api.c").decode("utf-8", "replace")
    check("L->npages = pn;" in layc and "L->pages[pn] = L->n;" in layc,
          "layout.c 修复 pages[pn] 未赋值导致的尾部空白页（1.2.11 主修）")
    check("npages = pn + 1;" not in layc, "layout.c 已移除 npages 多算一页")
    check("reader_title_layout" in uic and "READER_TITLE_MAX_LINES 8" in uic,
          "ui.c 阅读页标题自动折行（1.2.12 上限 8 行，完整显示）")
    check("g.base / 2 + g_title_lines * lh" in uic, "ui.c 页眉高度随标题行数自适应")
    check("draw_wrapped_n" in uic and 'strcmp(g.list_kind, "answers") == 0' in uic,
          "ui.c 回答列表每条 3 行摘要预览（行高自适应）")
    check("max_chars), 140" in apic or ", 140)" in apic,
          "api.c 回答摘要加长到 140 字（够三行预览）")
    check("空页=" in mnc and "越页=" in mnc,
          "main.c 自检 [lay2] 钉死空页/越页回归")

    # 1.2.12：推荐列表样式开关（简洁/详细）+ 问题标题优先 + 标题 8 行
    check("liststyle" in uic and "list_is_feed" in uic,
          "ui.c 列表样式开关（简洁/详细，作用于推荐与回答列表）")
    check("draw_wrapped_n(tx, sy" in uic and "2, y + rh" in uic,
          "ui.c 详细版摘要固定 2 行截断（行高恒定不跳页）")
    check("g_cfg.liststyle ? 0 : 1" in uic, "ui.c 设置页可切换列表样式")
    check("liststyle=%d" in uic, "config 写出 liststyle 键")
    check("char  summary[512]" in z.read(base + "source/src/api.h").decode("utf-8", "replace"),
          "api.h ApiItem 摘要独立字段 summary")
    apicc = z.read(base + "source/src/api.c").decode("utf-8", "replace")
    check('json_get_str(q, "title", NULL)' in apicc and "copy_str(it->summary" in apicc,
          "api.c 标题优先级：条目标题 > 问题标题 > 摘要（1.2.12 问题优先）")
    check("READER_TITLE_MAX_LINES 8" in uic,
          "ui.c 阅读页标题折行上限放开到 8 行（完整显示）")
    check("标题=问题标题、摘要进 summary" in mnc,
          "main.c 自检 [ans] 钉死 1.2.12 条目语义")

    # 1.2.13：真机六修——层级返回、图片后冻结、压屏节流、私信 App 端通道、
    #          关于页 GitHub、回答列表去重标题
    check("list_push_parent" in uic and "list_pop_parent" in uic and
          "list_clear_parent" in uic,
          "ui.c 列表快照 push/pop/clear（返回键层级化）")
    check("list_push_parent();               /* 1.2.13" in uic,
          "ui.c 进入问题列表/收藏内容前先存上一层")
    check("if (code == 158 || code == 102) nav_back();" in uic,
          "ui.c 物理返回键在列表页也走层级返回")
    check("static void loading_scr_restore(void);" in uic and
          "loading_scr_restore();\n    return 0;" in uic,
          "ui.c run_job 成功路径恢复 scr（1.3.1：LOADING 态未恢复导致 push_parent 被拒、回答列表返回直回主页的根因）")
    check("loading_scr_enter();" in uic and "g.load_scr_prev = g.scr;" in uic and
          "g.scr = g.load_scr_prev;" in uic,
          "ui.c 1.3.2：进入加载页记录真实来源界面（scr_prev 是过期值，1.3.1 恢复到主页导致压栈仍被拒）")
    # 1.3.3 定稿第2条：压栈彻底去掉 g.scr 守卫（三版连栽的根），只看 g.list.v
    check("if (g.list.v == NULL) {" in uic and "1.3.3 定稿（第2条）" in uic and
          "g.scr != SCR_LIST" not in uic.split("static void list_push_parent")[1].split("static int list_pop_parent")[0],
          "ui.c 1.3.3：list_push_parent 去掉 g.scr 守卫（1.2.13~1.3.2 层级返回三版失效的根因）")
    check("g.scr = SCR_LOADING;" in uic and "g.scr = SCR_MESSAGE;" in uic and
          "MESSAGE 态也必须成功" in uic,
          "ui.c [nav] 自检钉死 LOADING/MESSAGE 态压栈照样成功（去 scr 依赖）")
    # KO3 模拟器新增 hotans 离线注入：直接读工作区文件（打包内不一定含模拟器）
    try:
        ksim = open(os.path.join(os.path.dirname(__file__), '..', '..', 'KO3-模拟器', 'kindle_sim.py'), encoding='utf-8').read()
        check("sim.inject('hotans')" in ksim,
              "KO3 模拟器新增 hotans 离线注入：热榜→压栈→回答列表→返回回热榜端到端")
    except OSError:
        pass
    # 1.3.3 定稿第1条：刷屏两分法路由
    check("static void mark_inplace_refresh(void)" in uic and
          "g.scr == SCR_LIST || g.scr == SCR_READER" in uic and
          "int ui_refresh_route_test(void)" in uic,
          "ui.c 1.3.3：刷屏两分法——列表/答案详情→翻页刷屏，其余→主界面刷屏（[ref] 自检）")
    check("[ref  ]" in mnc and "屏内刷新分类" in mnc,
          "main.c [ref] 自检覆盖屏内刷新分类全部界面")
    # 1.3.4：默认 Regal + 修主界面闪（Regal 覆盖 full=1）
    check("static Config g_cfg = { 1, 2, 0, 0, 7" in uic and
          "g_cfg.uifast = 2" in uic and "strcmp(k, \"rglmig\")" in uic,
          "ui.c 1.3.4：翻页/主界面刷屏默认 Regal(2)，老设备 rglmig 一次性迁移")
    check("unsigned int fb_wave_for(int fast_mode, int regal, int full" in fbc and
          "if (fast_mode == 2 || regal) {" in fbc and
          "UPDATE_MODE_FULL;\n        return WAVEFORM_MODE_REAGL;" in fbc,
          "fb.c 1.3.4：Regal 提到波形决策最高优先级，覆盖 full=1 换屏不黑闪")
    check("int fb_refresh_wave_test(void)" in fbc and "[wav  ]" in mnc,
          "fb.c/main.c 1.3.4：[wav] 自检钉死 Regal 覆盖翻页+换屏波形规则")
    # 1.3.5：答案阅读页末页「下一个回答」
    check("static int reader_can_go_next_answer(void)" in uic and
          "static void reader_next_answer(void)" in uic and
          "footer_btn_center(BTN_NEXTANSWER" in uic,
          "ui.c 1.3.5：末页「下一个回答」判据+切换+正中按钮")
    check("reader_can_go_next_answer()) reader_next_answer();" in uic and
          "id == BTN_NEXTANSWER && g.scr == SCR_READER" in uic,
          "ui.c 1.3.5：右侧/下页键/左滑/正中按钮三条路径都切下一个")
    check("int ui_next_answer_test(void)" in uic and "[nxt  ]" in mnc and
          '"nextans"' in inc,
          "ui.c/main.c/input.c 1.3.5：[nxt] 自检+nextans 注入，私信页不误切")
    # 1.3.6：提交去重——屏幕不变不刷新
    check("static int commit_if_changed(int full)" in uic and
          "static int present_needed(uint32_t last, int valid, uint32_t now, int force)" in uic and
          "fb_present(&g.fb, full);" in uic,
          "ui.c 1.3.6：commit_if_changed 提交前比对签名，没变跳过 ioctl")
    check("g.present_force = 1;" in uic and "g.screen_sig = g.present_sig;" in uic and
          "去重=%ld/%ld" in uic,
          "ui.c 1.3.6：唤醒/守护/旋转强制补刷，守护基准同步，心跳可观测")
    check("int ui_present_diff_test(void)" in uic and "[diff ]" in mnc and
          '"redraw"' in inc,
          "ui.c/main.c/input.c 1.3.6：[diff] 自检+redraw 去重注入")
    # 1.3.7：翻页刷屏+主界面刷屏合并为单一「刷新模式」
    check('"正文字号", "刷新模式", "界面旋转", "显示图片", "页边距",' in uic and
          "char vals[11][64]" in uic and
          "int nrow = (int)(sizeof(names) / sizeof(names[0]))" in uic,
          "ui.c 1.3.7：设置页删「主界面刷屏」合并为「刷新模式」，11 项自动计数")
    check("g_cfg.uifast = g_cfg.fast;   /* 两项同源" in uic and
          "g_cfg.fast = (g_cfg.fast + 1) % 4;" in uic and
          "if (g_cfg.fast == 0) {   /* 1.3.7：与主界面共用同一档 */" in uic,
          "ui.c 1.3.7：切换 fast/uifast 同源，present_screen 两路同档")
    # 1.3.8：静止不动零刷新——挂起恢复不再整屏刷，图片线程结束不再无条件重画
    check("void fb_mark_resume(Fb *fb)" in fbc and
          "1.3.8：**不再 fb_present(1)**" in fbc and
          "int fb_resume_lazy_test(void)" in fbc,
          "fb.c 1.3.8：挂起恢复只打标记（不 unblank/不刷屏），静默回灌内存")
    check("fb_mark_resume(&g.fb);" in uic and
          "g.present_force = 1;\n                    redraw_current();" not in uic,
          "ui.c 1.3.8：心跳挂起分支不再 present_force+redraw（取消每轮唤醒全刷）")
    check("if ((got > 0 || left > 0) && (g.scr == SCR_READER" in uic,
          "ui.c 1.3.8：图片线程结束仅在真有图就绪/占位降级时重画")
    check("[idle ]" in mnc and "自动刷新=%ld" in uic and
          "fb_resume_lazy_test" in mnc,
          "main.c/ui.c 1.3.8：[idle] 自检 + 心跳「自动刷新=」静止期审计")
    check("clock_gettime(CLOCK_MONOTONIC, &ts);" in uic and
          "gettimeofday(&tv, NULL);" not in uic,
          "ui.c 1.3.8：全部定时改用单调钟（校时跳秒不再被误判成挂起→整屏刷新）")
    check("g.scr = SCR_READER;" in uic and
          "成功路径如果不换回来" in uic,
          "ui.c img_load_one 成功路径恢复 scr（1.2.12 图片后冻结根因）")
    check("guard_hold = now_ms_ui() + 500;" in uic and
          "guard_hold = now_ms_ui() + 3000" not in uic,
          "ui.c 压屏改 500ms 节流（顶栏时间不再停留 3 秒）")
    apic2 = z.read(base + "source/src/api.c").decode("utf-8", "replace")
    check('strncmp(path, "https://", 8) == 0' in apic2,
          "api.c 支持绝对 URL（api.zhihu.com App 端通道）")
    soc = z.read(base + "source/src/social.c").decode("utf-8", "replace")
    check("https://api.zhihu.com/messages/inbox?thread_id=" in soc,
          "social.c 新增 App 端私信端点（游客探针 401=路由存在）")
    check("200 但解析失败" in soc and "%d* " in soc,
          "social.c 200-解析失败也计入诊断状态（不再误报全 404）")
    check("github.com/asuplive/Zhihu-Kindle-native" in uic,
          "ui.c 关于页含 GitHub 仓库地址")
    check('strcmp(g.list_kind, "answers") == 0) {' in uic and
          "页眉上已经是这个问题了" in uic,
          "ui.c 回答列表去掉重复问题标题（摘要为主，推荐列表不变）")

    # 1.2.14：两修——选中反色（黑底白字）、状态栏正面隐藏（pillow+awesome）
    check("sel ? 255 : 30" in uic and "黑底配黑字" in uic,
          "ui.c 回答列表选中态摘要=白字（1.2.13 曾黑底黑字不可见）")
    check("int fg3 = sel ? 200 : 130;" in uic,
          "ui.c 选中态作者行灰度提高（170→200）")
    sbar = z.read(base + "source/src/statusbar.c").decode("utf-8", "replace")
    sbarh = z.read(base + "source/src/statusbar.h").decode("utf-8", "replace")
    check("disableEnablePillow" in sbar and '"disable"' in sbar and '"enable"' in sbar,
          "statusbar.c 禁用/恢复 chrome pillow（KOReader 同款正面方案）")
    check("SIGSTOP" in sbar or ("kill(pid" in sbar and "SIGSTOP" in sbarh) or "SIGSTOP" in sbarh,
          "statusbar.c/h SIGSTOP awesome（FW>=5.7.2 时钟仍会刷新，必须停 WM）")
    check("SIGCONT" in sbar and ".awesome_stopped" in sbar,
          "statusbar.c 退出恢复=SIGCONT+标记文件（崩溃/脚本两条兜底路都认它）")
    check('"awesome" == ' not in sbar and 'strcmp(buf, "awesome")' in sbar,
          "statusbar.c 扫 /proc 找 awesome（不依赖 pidof）")
    check("statusbar_hide();" in uic and "statusbar_restore();" in uic and
          "statusbar_restore_safe();" in mnc and "statusbar_restore();" in mnc,
          "接线完整：ui.c 启动隐藏/退出恢复，main.c 退出与崩溃兜底")
    check("awesome_reassert" in z.read(base + "extensions/zhihureader/bin/start.sh").decode("utf-8", "replace") and
          "awesome_restore" in z.read(base + "extensions/zhihureader/bin/start.sh").decode("utf-8", "replace"),
          "start.sh 每 5 秒重压（唤醒后 framework 恢复顶栏时）+ cleanup 兜底恢复")
    check('"anslist"' in z.read(base + "source/src/input.c").decode("utf-8", "replace") and
          "sim_answers_demo" in uic,
          "模拟器离线注入回答列表（知乎游客 403，回归需要离线数据）")

    # 1.2.15：私信消息 SSR 收割 + 翻页刷屏第 4 档（局部 N 次 + 定期全刷）
    soc = z.read(base + "source/src/social.c").decode("utf-8", "replace")
    check("js-initialData" in soc and "social_parse_messages_html" in soc and
          "harvest_walk" in soc and "looks_like_message" in soc,
          "social.c SSR 收割：提取 js-initialData + 通用递归找消息数组（真机实锤会话 url 是网页）")
    check("msg_page_dump.html" in soc,
          "social.c 解析失败时把 SSR 页面落盘（下一轮反馈不再猜结构）")
    check('"https://www.zhihu.com/messages?thread_id={id}"' in soc,
          "social.c 消息候选加 web SSR 页面兜底")
    check("is_sender" in soc and "from_self" in soc,
          "social.c is_self 判定覆盖 is_sender/from_self（App/SSR 字段）")
    check("api.zhihu.com" in z.read(base + "source/src/api.c").decode("utf-8", "replace") and
          "x-api-version" in z.read(base + "source/src/api.c").decode("utf-8", "replace") and
          "UA_APP" in z.read(base + "source/src/api.c").decode("utf-8", "replace"),
          "api.c App 通道：api.zhihu.com 用 App UA + x-api-version，不发 web 签名（真机登录态 500 的修复）")
    fbc = z.read(base + "source/src/fb.c").decode("utf-8", "replace")
    # 1.3.4：第4档周期逻辑移入 fb_wave_for 纯函数（去掉 fb-> 前缀）
    check(('fast_mode == 3 && full_every > 1' in fbc or
           'fast_mode == 3 && fb->full_every > 1' in fbc) and "fullevery=" in fbc,
          "fb.c 第 4 档：每 full_every 次翻页补一次 GC16 全刷，周期可配（2..20）")
    check("was_4" in fbc,
          "fb.c 第 4 档被驱动拒绝时模式保留（不永久关闭）")
    check("fullevery" in uic and "(g_cfg.fast + 1) % 4" in uic and "局部+定期全刷" in uic,
          "ui.c 设置页四档循环 + fullevery 配置持久化")
    check("[dmssr]" in mnc and "social_parse_messages_html" in mnc,
          "main.c [dmssr] 离线自检：合成 SSR 页面钉死提取→收割链路")

    # 1.3.0 专项：主界面刷屏四档 + 拼音输入法
    # 1.3.7：两项刷屏合并为单一「刷新模式」，切换写 fast+uifast 同源；
    # 切换语句从 (uifast+1)%4 改为 (fast+1)%4 + uifast=fast。
    check("g_cfg.uifast = g_cfg.fast" in uic and '"刷新模式"' in uic and
          "ui_fast_next = 1" in uic,
          "ui.c 刷新模式四档（1.3.7 合并：fast/uifast 同源，屏内更新标记保留）")
    check('if (g.fast_next && g_cfg.fast)' in uic and
          'if (g.ui_fast_next)' in uic and "present_full" in uic,
          "present_screen 三路分流：阅读页翻页/主界面屏内更新/换屏全刷")
    check('strcmp(kt, "uifast") == 0' in uic and "uifast=%d" in uic,
          "config.txt uifast 读写且不被手写项冲掉")
    check("ime_table_n" in z.read(base + "source/src/ime_table.c").decode("utf-8", "replace"),
          "ime_table.c 码表在包内（KOReader zh_pinyin_data 同源，Apache-2.0）")
    check("ime_find" in z.read(base + "source/src/ime.c").decode("utf-8", "replace") and
          "ime_segment" in z.read(base + "source/src/ime.c").decode("utf-8", "replace"),
          "ime.c 码表二分查找 + 音节切分")
    check("#include \"ime.h\"" in uic and "ime_refresh" in uic and "ime_commit" in uic and
          "ime_bar_geom" in uic and "kb_cn" in uic,
          "ui.c 候选栏绘制/热区/中英切换/数字选字接线")
    check("[ime  ]" in mnc and "ime_segment(\"nihao\"" in mnc,
          "main.c [ime] 离线自检：查找/切分/词组回退")
    check('"ime"' in inc and "e->code = 990" in inc,
          "模拟器注入命令 ime（离线回归输入法）")
    check("fb_fill(&g.fb, cx + 2, ry + 2" in uic,
          "确定键黑底用 fb_fill 填充（fb_rect 只画边框，旧版白底白字修复）")

    # 配置合法
    menu = json.loads(z.read(base + "extensions/zhihureader/menu.json").decode("utf-8"))

    def walk(items, depth=1):
        acts = 0
        for it in items:
            if "items" in it:
                check(depth == 1, "二级菜单「%s」只出现在第一层" % it.get("name"))
                acts += walk(it["items"], depth + 1)
            else:
                act = it.get("action", "")
                okk = bool(act) and (base + "extensions/zhihureader/" + act in names)
                check(okk, "菜单「%s」指向的脚本存在" % it.get("name"))
                acts += 1
        return acts

    check("items" in menu and len(menu["items"]) == 2, "menu.json 是一级菜单：%d 个分类" % len(menu.get("items", [])))
    total = walk(menu["items"])
    check(total >= 8, "二级菜单共 %d 项（安装/启动/自检/日志/诊断/排障）" % total)

    ET.fromstring(z.read(base + "extensions/zhihureader/config.xml"))
    check(True, "config.xml 是合法 XML")

    # 脚本 shebang
    for n in ("start.sh", "install.sh", "log.sh"):
        body = z.read(base + "extensions/zhihureader/bin/" + n)
        check(body.startswith(b"#!/bin/sh"), "%s 有 shebang" % n)

    # README 里的图片引用
    readme = z.read(base + "README.md").decode("utf-8")
    import re
    for m in re.finditer(r"\]\((docs/[^)]+)\)", readme):
        check(base + m.group(1) in names, "README 图片存在：%s" % m.group(1))

    print()
    if ok:
        print("全部检查通过（%d 个文件）" % len(names))
    else:
        print("发现问题：")
        for p in problems:
            print("  - " + p)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
