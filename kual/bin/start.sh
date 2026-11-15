#!/bin/sh
# 启动器：接管屏幕 → 跑知乎阅读 → 还原系统界面
#
# 这一版（1.2.4）要解决的是「能用但不好用」：
#   1) 静置几分钟弹「应用程序错误」、进程被 SIGTERM 杀掉（rc=143）；
#   2) 卡在主页、点哪儿都没反应；
#   3) 点一下菜单整屏黑闪一下，感觉像卡死。
#
# 真机日志给出的结论是：**冻结 cvm（SIGSTOP）这个模型本身有问题**。
#   · cvm 被冻住时仍然攥着触摸屏的 EVIOCGRAB 独占，独占是排他的，
#     于是知乎虽然 open 得到设备，却一个触摸事件也收不到 → 点不动；
#   · framework 发现自家 UI 被冻住，过几分钟按进程组 kill -TERM，
#     把 KUAL 拉起来的脚本和知乎一起带走 → 弹「应用程序错误」。
# 所以默认不再冻结，改成先探测：能不能拿到触摸屏独占。
#   · 拿得到 → 和 framework 共存（framework=none），最安全，零副作用；
#   · 拿不到 → 说明 cvm 占着，只能接管系统界面（framework=stop），
#     用 /etc/init.d/framework stop 正常停（不是 kill -9），退出时干净启动回来。
# 程序自己也会在退出时还原（atexit + SIGTERM 优雅退出），两条路互为备份。
#
# 排障开关（写进 /mnt/us/zhihu/config.txt，不用重新编译）：
#   framework=auto|none|stop|pause   默认 auto（按触摸独占探测决定）
#   wake=auto|off                    默认 auto（wakelock + lipc 双保险）
#   showbar=0|1                      默认 0（1.2.14：隐藏顶栏=禁 pillow+暂停 awesome）
#   refresh=auto|mxcfb|eips|none|blank|pan   见 README
#   eips=auto|image|flash|off
#   blankpulse=0|1

BASE=$(cd "$(dirname "$0")/.." && pwd)
DATA=/mnt/us/zhihu
APP="$DATA/zhihu"
[ -x "$APP" ] || APP="$BASE/app/zhihu"
LOG="$DATA/run.log"
APPLOG="$DATA/log.txt"
CFG="$DATA/config.txt"
STOPPED="$DATA/.framework_stopped"
mkdir -p "$DATA/cache"
export ZHIHU_HOME="$DATA"

log() { echo "--- $(date '+%m-%d %H:%M:%S') $*" >> "$LOG"; }

# config.txt 里取一个键，没有就用默认值
cfg_get() {   # cfg_get <key> <default>
    [ -f "$CFG" ] || { echo "$2"; return; }
    V=$(sed -n "s/^[ 	]*$1[ 	]*=[ 	]*//p" "$CFG" 2>/dev/null | tail -n 1 | tr -d ' \t\r')
    if [ -n "$V" ]; then echo "$V"; else echo "$2"; fi
}

# eips 只认内置西文字体，中文会变方块，所以屏幕上只写英文
show_file() {   # show_file <文件> <最多到第几行>
    [ -f "$1" ] || return 0
    i=2
    while IFS= read -r line; do
        eips 1 $i "$line" 2>/dev/null
        i=$((i + 1))
        [ $i -gt "$2" ] && break
    done < "$1"
}

# 取 cvm 的 pid。
# Kindle 上 pidof 不一定在 PATH 里，拿不到时会误判成"cvm 已经退了"，
# 于是程序照常启动、cvm 继续往同一块 framebuffer 上画，画面被整个盖掉。
# 所以这里再退一步直接扫 /proc/*/comm（只看进程名，不会误杀别的进程）。
cvm_pid() {
    P=$(pidof cvm 2>/dev/null)
    if [ -n "$P" ]; then echo "$P"; return 0; fi
    [ -x /sbin/pidof ] && P=$(/sbin/pidof cvm 2>/dev/null)
    if [ -n "$P" ]; then echo "$P"; return 0; fi
    for d in /proc/[0-9]*; do
        [ -r "$d/comm" ] || continue
        nm=$(cat "$d/comm" 2>/dev/null)
        if [ "$nm" = "cvm" ]; then echo "${d#/proc/}"; return 0; fi
    done
    return 0
}

# ---------------- 保持唤醒 ----------------
# 停掉 framework 之后没人再告诉 powerd"别睡"，它到点就把整机挂起，
# 而 5.x 在无 system UI 的环境下唤醒会失败 → 硬件复位（1.2.2 的整机重启）。
# 所以这里两手都上：内核 wakelock（最硬）+ lipc 三个开关（老机型也有）。

WAKE_FILE=""
acquire_wake() {
    for f in /sys/power/wake_lock /sys/class/power/wake_lock; do
        [ -e "$f" ] || continue
        if echo zhihu_reading > "$f" 2>/dev/null; then
            WAKE_FILE="$f"
            log "wakelock 已持有：$f"
            return 0
        fi
    done
    log "WARN: 没有 /sys/power/wake_lock（内核未开 wakelock），只靠 lipc 防休眠"
    return 1
}

release_wake() {
    [ -n "$WAKE_FILE" ] || return 0
    U=$(echo "$WAKE_FILE" | sed 's/wake_lock$/wake_unlock/')
    echo zhihu_reading > "$U" 2>/dev/null
    log "wakelock 已释放：$U"
    WAKE_FILE=""
}

powerd_nudge() {
    lipc-set-prop -i com.lab126.powerd preventScreenSaver 1 2>/dev/null
    lipc-set-prop -i com.lab126.powerd autoSuspend 0 2>/dev/null
    lipc-set-prop -i com.lab126.powerd deferSuspend 1 2>/dev/null
}

# ---------------- 1.2.14：状态栏隐藏的脚本侧兜底 ----------------
# showbar=0 时程序自己会：禁 chrome pillow + SIGSTOP awesome（KOReader 同款，
# 不碰 cvm，无看门狗风险），并把 awesome pid 写进 .awesome_stopped。
# 脚本在这里做两件事：
#   reassert —— 每 5 秒一次（挂在 keepawake_loop 里）：唤醒后 framework 可能
#               把顶栏/awesome 恢复回去，发现 awesome 在跑就重新压住；
#   cleanup  —— 程序没来得及自己恢复（崩溃/被杀）时兜底还原。
AWESOME_MARK="$DATA/.awesome_stopped"

awesome_reassert() {   # 仅 showbar=0 时调用
    [ -f "$AWESOME_MARK" ] || return 0
    lipc-set-prop com.lab126.pillow disableEnablePillow disable 2>/dev/null
    AP=$(cat "$AWESOME_MARK" 2>/dev/null)
    [ -n "$AP" ] || return 0
    ST=$(sed -n 's/.*) \([A-Z]\) .*/\1/p' "/proc/$AP/stat" 2>/dev/null)
    if [ "$ST" != "T" ]; then
        kill -STOP "$AP" 2>/dev/null && log "awesome(pid=$AP) 被系统恢复过，重新暂停（状态=$ST）"
    fi
}

awesome_restore() {
    if [ -f "$AWESOME_MARK" ]; then
        AP=$(cat "$AWESOME_MARK" 2>/dev/null)
        [ -n "$AP" ] && kill -CONT "$AP" 2>/dev/null
        lipc-set-prop com.lab126.pillow disableEnablePillow enable 2>/dev/null
        rm -f "$AWESOME_MARK"
        log "恢复：awesome(pid=$AP) SIGCONT + pillow enable（脚本兜底）"
    fi
}

# 每 5 秒补一次。lipc 全失败说明 powerd 不理我们（或者根本没起来），
# 这时把结论写进日志——下次看日志就知道该指望 wakelock 还是 lipc。
keepawake_loop() {
    fails=0
    while :; do
        if lipc-set-prop -i com.lab126.powerd preventScreenSaver 1 2>/dev/null; then
            fails=0
            lipc-set-prop -i com.lab126.powerd autoSuspend 0 2>/dev/null
            lipc-set-prop -i com.lab126.powerd deferSuspend 1 2>/dev/null
        else
            fails=$((fails + 1))
            if [ $fails -eq 3 ]; then
                log "WARN: powerd 不响应 lipc（$fails 次），防休眠只能靠 wakelock=[$WAKE_FILE]"
            fi
        fi
        [ "$SB_HIDE" = "1" ] && awesome_reassert
        guard_framework
        sleep 5
    done
}

# ---------------- 诊断：内核现场 ----------------

log_boot_state() {
    UP=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)
    BOOTID=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)
    LAST=$(cat "$DATA/.boot_id" 2>/dev/null)
    log "开机时长 ${UP}s boot_id=${BOOTID}"
    if [ -n "$BOOTID" ] && [ -n "$LAST" ] && [ "$BOOTID" != "$LAST" ]; then
        log "诊断：boot_id 变了 —— 上次会话没走到退出，机器是重启过的（开机仅 ${UP}s）"
    fi
    [ -n "$BOOTID" ] && echo "$BOOTID" > "$DATA/.boot_id"
    log "powerd pid=[$(pidof powerd 2>/dev/null)] framework pid=[$(pidof cvm 2>/dev/null)]"
    [ -r /sys/power/wake_lock ] && log "wake_lock=[$(cat /sys/power/wake_lock 2>/dev/null)]"
    [ -r /sys/power/state ] && log "power/state=[$(cat /sys/power/state 2>/dev/null)]"
    log_suspend_stats "启动前"
}

log_suspend_stats() {
    S=""
    for f in success fail last_failed_dev last_failed_errno; do
        [ -r "/sys/power/suspend_stats/$f" ] || continue
        S="$S $f=$(cat /sys/power/suspend_stats/$f 2>/dev/null)"
    done
    [ -n "$S" ] && log "suspend_stats($1):$S"
}

# dmesg 尾巴落到闪存上：整机重启后内核环形缓冲就没了，
# 只有提前抄下来的这些能告诉我们"是 panic、看门狗还是挂起失败"。
kmsg_loop() {
    : > /tmp/zh_kmsg.prev
    while :; do
        dmesg 2>/dev/null | tail -n 12 > /tmp/zh_kmsg.txt
        if [ -s /tmp/zh_kmsg.txt ] && ! cmp -s /tmp/zh_kmsg.txt /tmp/zh_kmsg.prev 2>/dev/null; then
            cp -f /tmp/zh_kmsg.txt /tmp/zh_kmsg.prev 2>/dev/null
            echo "=== $(date '+%m-%d %H:%M:%S') ===" >> "$DATA/kmsg.log"
            cat /tmp/zh_kmsg.txt >> "$DATA/kmsg.log"
            SZ=$(wc -c < "$DATA/kmsg.log" 2>/dev/null)
            if [ -n "$SZ" ] && [ "$SZ" -gt 524288 ]; then
                tail -c 262144 "$DATA/kmsg.log" > /tmp/zh_kmsg.tail 2>/dev/null
                mv -f /tmp/zh_kmsg.tail "$DATA/kmsg.log" 2>/dev/null
            fi
        fi
        sleep 20
    done
}

# ---------------- framework 处理 ----------------

CF_MODE=$(cfg_get framework auto)
FROZEN_FILE="$DATA/.cvm_frozen"
# 1.2.10：stop 兜底（防止 framework 看门狗因 cvm 长期死亡而整机重启）
FWBACK=$(cfg_get fwback 600)
RELEASED="$DATA/.stop_released"

# framework=stop 持续超过 fwback 秒就主动恢复，退回共存模式。
# 以 $STOPPED 标记的 mtime 为 stop 生效时刻。
stop_expire_loop() {
    while :; do
        sleep 10
        [ -f "$RELEASED" ] && return 0
        ST=$(stat -c %Y "$STOPPED" 2>/dev/null) || return 0
        NOW=$(date +%s)
        if [ -n "$ST" ] && [ $((NOW - ST)) -ge "$FWBACK" ]; then
            touch "$RELEASED" 2>/dev/null
            rm -f "$STOPPED"
            log "stop 兜底：framework 已停 ${FWBACK}s，为避免看门狗整机重启，恢复 framework（顶栏由屏幕守护压回）"
            [ -x /etc/init.d/framework ] && /etc/init.d/framework start
            return 0
        fi
    done
}

cvm_state() {   # cvm_state <pid> -> R/S/T/Z/?
    # 直接读 /proc/<pid>/stat 的第三个字段（进程状态），不用 fork sed——
    # 这个函数在 5 秒一次的心跳里跑，能省一个进程就省一个。
    [ -r "/proc/$1/stat" ] || { echo "?"; return; }
    read -r _cpid _ccomm _cstate _crest < "/proc/$1/stat" 2>/dev/null
    echo "$_cstate"
}

# auto：用程序自己的探测结果决定要不要接管系统界面。
# 这一步是整个 1.2.4 的关键——不再靠猜，也不再"默认就把系统界面冻上"。
resolve_mode() {
    [ "$CF_MODE" = "auto" ] || return 0
    if [ ! -x "$APP" ]; then CF_MODE=none; return 0; fi
    GT=$("$APP" --grabtest 2>/dev/null | head -n 3 | tr '\n' ' ')
    log "触摸独占探测: $GT"
    case "$GT" in
    *grab=ok*)
        CF_MODE=none
        log "framework=auto -> none（触摸屏没被独占，和系统界面共存，不碰 cvm）"
        ;;
    *grab=busy*)
        CF_MODE=stop
        log "framework=auto -> stop（触摸屏被 cvm 独占，必须接管；用 framework stop 正常停）"
        ;;
    *)
        CF_MODE=none
        log "framework=auto -> none（没有触摸设备：K4/K5 或模拟器，不必接管）"
        ;;
    esac
}

# 冻结：仅保留给 config.txt 里显式写 framework=pause 的对比实验。
# 注意：冻住的 cvm 仍然握着触摸屏独占，所以这个模式下点击一定无效。
freeze_pid() {
    kill -STOP "$1" 2>/dev/null || return 1
    echo "$1" > "$FROZEN_FILE" 2>/dev/null
    log "冻结 cvm pid=[$1]（实验模式：触摸会失效）"
    sleep 1
    log "cvm state=$(cvm_state "$1")"
}

stop_framework() {
    # 先落标记再动手：标记的意思是"framework 是我们停的，退出时要启动回来"。
    # 程序退出时也会看这个标记（start.sh 被强杀时由它兜底启动）。
    # 谁先处理谁就删掉它，保证不会两边各 start 一次（起两个 cvm 更糟）。
    echo 1 > "$STOPPED" 2>/dev/null
    # 优先走官方脚本：它会给 cvm 发 SIGTERM 让它自己收尾，
    # 直接 kill -9 会被 appmgr 当成崩溃，弹出「应用程序错误」。
    if [ -x /etc/init.d/framework ]; then
        log "framework stop（会先正常收尾，再兜底 kill）"
        /etc/init.d/framework stop
    else
        log "没有 /etc/init.d/framework，直接杀 cvm"
    fi
    tries=0
    while [ $tries -lt 8 ]; do
        CP=$(cvm_pid)
        [ -z "$CP" ] && break
        log "kill cvm pid=[$CP] try $((tries + 1))"
        kill -9 $CP 2>/dev/null
        sleep 1
        tries=$((tries + 1))
    done
    CP=$(cvm_pid)
    if [ -n "$CP" ]; then
        log "WARN: cvm 还在 [$CP]，触摸可能仍被它占着"
    else
        log "framework stopped, cvm gone（触摸独占已释放）"
    fi
    rm -f "$FROZEN_FILE"
}

# 每 5 秒调一次。两种模式下要做的事不一样：
#   stop  —— 系统可能把 cvm 重新拉起来（appmgrd 会），一冒头就掐掉，
#            否则它会顶掉我们的画面、并把触摸屏重新独占走；
#   pause —— 只把被 SIGCONT 的那个再冻回去（实验模式）。
WANT_STOP="$DATA/.want_stop"

guard_framework() {
    case "$CF_MODE" in
    none)
        # 1.2.10：不再因"程序报告抢屏"自动切 stop——杀 cvm 会触发 framework
        # 看门狗整机重启。抢屏由程序内的屏幕守护（影子回灌）自己解决。
        # 1.2.9 及以前写的 .want_stop 残留在这里清掉即可，不再生效。
        [ -f "$WANT_STOP" ] || return 0
        rm -f "$WANT_STOP"
        log "发现 .want_stop 残留（1.2.9 及以前的自动升级 stop 已移除），已清除；保持共存"
        ;;
    stop)
        # 1.2.10：stop 兜底已恢复过 framework（.stop_released）就退回共存，
        # 不再继续掐 cvm——init 会把它拉起来，让系统界面自然回来。
        if [ -f "$RELEASED" ]; then
            CF_MODE=none
            return 0
        fi
        P=$(cvm_pid)
        [ -n "$P" ] || return 0
        if kill -9 "$P" 2>/dev/null; then
            log "系统又拉起了 cvm pid=[$P]，已掐掉（否则画面/触摸会被抢回去）"
        fi
        ;;
    pause)
        P=$(cat "$FROZEN_FILE" 2>/dev/null)
        if [ -n "$P" ] && [ -d "/proc/$P" ]; then
            [ "$(cvm_state "$P")" = "T" ] && return 0
            sleep 1
            [ "$(cvm_state "$P")" = "T" ] && return 0
            kill -STOP "$P" 2>/dev/null && log "重新冻结 cvm pid=[$P]（被 SIGCONT 了）"
            return 0
        fi
        P=$(cvm_pid)
        [ -n "$P" ] || return 0
        if kill -STOP "$P" 2>/dev/null; then
            echo "$P" > "$FROZEN_FILE" 2>/dev/null
            log "冻结新拉起的 cvm pid=[$P]"
        fi
        ;;
    esac
}

restore_framework() {
    # 1.2.10：stop 兜底已经恢复过 framework，就不在这里重复 start
    # （重复 start 可能起两个 cvm，比不启动更糟）。
    if [ -f "$RELEASED" ]; then
        rm -f "$RELEASED"
        log "恢复：stop 兜底已 framework start，这里不再重复"
        return 0
    fi
    # 被动接管（none -> stop）时 CF_MODE 的改动发生在后台心跳的 subshell 里，
    # 主 shell 这边还是 none，但不代表没停过——以标记文件为准，先处理它。
    if [ "$CF_MODE" != "stop" ] && [ -f "$STOPPED" ]; then
        rm -f "$STOPPED"
        log "恢复：检测到 .framework_stopped（运行中被动接管过），framework start"
        [ -x /etc/init.d/framework ] && /etc/init.d/framework start
        return
    fi
    case "$CF_MODE" in
    stop)
        # 标记还在 = 启动脚本负责收尾；已经被程序删掉 = 程序已经启动过 framework
        if [ -f "$STOPPED" ]; then
            rm -f "$STOPPED"
            log "恢复：framework start"
            [ -x /etc/init.d/framework ] && /etc/init.d/framework start
        else
            log "恢复：framework 已由程序自己启动，这里不重复 start"
        fi
        ;;
    pause)
        P=$(cat "$FROZEN_FILE" 2>/dev/null)
        rm -f "$FROZEN_FILE"
        if [ -n "$P" ] && [ -d "/proc/$P" ]; then
            kill -CONT "$P" 2>/dev/null
            log "恢复 cvm pid=[$P]（SIGCONT）"
        else
            log "没有可恢复的 cvm（已被系统清掉），framework 保持现状"
        fi
        ;;
    esac
}

cleanup() {
    rm -f "$WANT_STOP"
    release_wake
    awesome_restore
    [ -n "$KEEP_PID" ] && kill $KEEP_PID 2>/dev/null
    [ -n "$EXPIRE_PID" ] && kill $EXPIRE_PID 2>/dev/null
    [ -n "$KMSG_PID" ] && kill $KMSG_PID 2>/dev/null
    lipc-set-prop -i com.lab126.powerd preventScreenSaver 0 2>/dev/null
    restore_framework
}

if [ ! -x "$APP" ]; then
    eips -c 2>/dev/null
    eips 2 2 "zhihu-reader: app not found" 2>/dev/null
    eips 2 3 "run 'Install / Update' in KUAL first" 2>/dev/null
    exit 1
fi

case "$1" in
selftest)
    "$APP" --selftest > "$DATA/selftest.txt" 2>&1
    eips -c 2>/dev/null
    eips 1 1 "zhihu-reader selftest:" 2>/dev/null
    show_file "$DATA/selftest.txt" 20
    ;;
diag)
    "$APP" --diag > "$DATA/diag.txt" 2>&1
    eips -c 2>/dev/null
    eips 1 1 "zhihu-reader diag:" 2>/dev/null
    show_file "$DATA/diag.txt" 20
    ;;
apitest)
    # 接口体检：把"真实 URL / 状态码 / 响应体前 200 字节"打到屏幕上（ASCII）
    # 和 log.txt（中文）。点不进去 404 时，这一页就是证据。
    "$APP" --apitest > "$DATA/apitest.txt" 2>&1
    eips -c 2>/dev/null
    eips 1 1 "zhihu-reader api test:" 2>/dev/null
    show_file "$DATA/apitest.txt" 20
    ;;
dmprobe)
    # 私信端点诊断：私信打不开时，先看这一页的状态码
    "$APP" --dmprobe > "$DATA/dmprobe.out" 2>&1
    eips -c 2>/dev/null
    eips 1 1 "zhihu-reader dm probe:" 2>/dev/null
    show_file "$DATA/dmprobe.out" 20
    ;;
spy)
    # 排障用：让系统 eips 在 LD_PRELOAD 下跑一遍，把它调的 mxcfb ioctl 全抓下来。
    SPY="$BASE/bin/spy.so"
    [ -f "$SPY" ] || SPY="$DATA/bin/spy.so"
    if [ ! -f "$SPY" ]; then
        eips -c 2>/dev/null
        eips 1 2 "spy.so not found in package" 2>/dev/null
        exit 1
    fi
    rm -f "$DATA/eips_spy.log"
    log "spy: 开始抓 eips 的 ioctl"
    ZH_SPY_LOG="$DATA/eips_spy.log" LD_PRELOAD="$SPY" eips -f 2>/dev/null
    R=$?
    ZH_SPY_LOG="$DATA/eips_spy.log" LD_PRELOAD="$SPY" eips 1 2 "spy probe" 2>/dev/null
    N=$(wc -l < "$DATA/eips_spy.log" 2>/dev/null)
    if [ -z "$N" ] || [ "$N" -eq 0 ]; then
        log "spy: 没抓到记录（eips rc=$R），确认 eips 单独能跑"
        eips 1 2 "spy.so load failed, rc=$R" 2>/dev/null
    fi
    log "spy: 抓到 ${N} 条 ioctl 记录"
    eips -c 2>/dev/null
    eips 1 1 "eips ioctl spy: ${N} calls" 2>/dev/null
    eips 1 2 "see K:\\zhihu\\eips_spy.log" 2>/dev/null
    head -n 14 "$DATA/eips_spy.log" 2>/dev/null > "$DATA/.spy_tail"
    show_file "$DATA/.spy_tail" 18
    rm -f "$DATA/.spy_tail"
    ;;
*)
    # 包里的程序和已装的大小不一致就自动同步一次。
    # 免得出现"明明换了新包，跑的还是旧程序"——这种坑排查起来极费劲。
    if [ -f "$BASE/app/zhihu" ]; then
        WANT=$(wc -c < "$BASE/app/zhihu" | tr -d ' ')
        GOT=$(wc -c < "$DATA/zhihu" 2>/dev/null | tr -d ' ')
        if [ "$WANT" != "$GOT" ]; then
            cp -f "$BASE/app/zhihu" "$DATA/zhihu" 2>/dev/null && chmod 755 "$DATA/zhihu"
            sync
            echo "--- auto-update $GOT -> $WANT bytes ---" >> "$LOG"
        fi
    fi

    rm -f "$DATA/.want_stop"
    log "================ start $(date) ================"
    log_boot_state

    # 探测必须在动 framework 之前：探测结果决定要不要动它
    # 1.2.10：showbar 与 framework 彻底解耦（1.2.9 的教训：showbar=0 时杀 cvm，
    # framework 看门狗在宽限期后整机重启）。
    # 1.2.14：状态栏隐藏换成正面方案——程序内禁 chrome pillow + SIGSTOP
    # awesome（KOReader 同款，不碰 cvm，无看门狗风险），默认 showbar=0；
    # 压屏守护保留作兜底，脚本侧每 5 秒重压一次（唤醒后 framework 恢复顶栏时）。
    SB=$(cfg_get showbar 0)
    SB_HIDE=0
    [ "$SB" = "0" ] && SB_HIDE=1
    if [ "$SB" = "0" ]; then
        log "showbar=0：隐藏顶栏=禁 pillow + 暂停 awesome（1.2.14 正面方案，不杀 cvm）"
    fi
    resolve_mode
    log "config: framework=$CF_MODE（原始值 $(cfg_get framework auto)）refresh=$(cfg_get refresh auto) eips=$(cfg_get eips auto) showbar=$SB rec=$(cfg_get rec 6)"

    # 从这一刻起，任何退出路径都要把 wakelock / cvm / 防休眠还原，
    # 否则用户强杀脚本时系统界面会一直冻着
    trap 'cleanup' EXIT INT TERM HUP

    # 1) 先拿 wakelock —— 必须在动 framework 之前，中间不能有"能睡"的空窗
    WAKE_MODE=$(cfg_get wake auto)
    if [ "$WAKE_MODE" != "off" ]; then
        acquire_wake
        if powerd_nudge && lipc-set-prop -i com.lab126.powerd preventScreenSaver 1 2>/dev/null; then
            log "lipc 防休眠已生效（powerd 应答正常）"
        else
            log "WARN: lipc 防休眠没生效，只能靠 wakelock"
        fi
    fi

    # 2) dmesg 尾巴持续落盘（重启后内核缓冲就没了，只能靠这个）
    kmsg_loop >/dev/null 2>&1 &
    KMSG_PID=$!

    # 3) 接管屏幕（先接管，再起看门狗——否则看门狗会和这里抢着处理）
    case "$CF_MODE" in
    none)  log "framework=none：与系统界面共存，cvm 保持运行（退出时也不需要恢复）" ;;
    pause)
        P=$(cvm_pid)
        if [ -n "$P" ]; then freeze_pid "$P" "实验模式"
        else log "WARN: framework=pause 但没找到 cvm（系统界面本来就没起来？）"; fi
        ;;
    *)     stop_framework ;;
    esac

    # 4) 每 5 秒补一次防休眠，并守住"cvm 别被拉回来抢屏"
    keepawake_loop >/dev/null 2>&1 &
    KEEP_PID=$!

    # 5) 1.2.10：stop 模式兜底——cvm 死太久会触发 framework 看门狗整机重启
    #    （1.2.9 真机实测，宽限期几十秒到几分钟不等）。显式 framework=stop
    #    （或 auto 探测到 busy）时，持续超过 fwback 秒就主动恢复 framework
    #    回到共存，顶栏由程序内屏幕守护压回。fwback=0 关闭兜底。
    if [ "$CF_MODE" = "stop" ] && [ "$FWBACK" -gt 0 ] 2>/dev/null; then
        rm -f "$RELEASED"
        stop_expire_loop >/dev/null 2>&1 &
        EXPIRE_PID=$!
        log "stop 兜底已启动：framework=stop 持续超过 ${FWBACK}s 自动恢复共存（config.txt 写 fwback=0 关闭）"
    fi

    "$APP" >> "$LOG" 2>&1
    rc=$?
    log "================ exit rc=$rc $(date) ================"
    log_suspend_stats "退出前"

    if [ $rc -ne 0 ]; then
        # 崩溃/异常退出：把最后的现场留在屏幕上，方便拍照反馈
        eips -c 2>/dev/null
        eips 1 1 "zhihu-reader stopped, code $rc" 2>/dev/null
        if [ $rc -ge 128 ]; then
            eips 1 2 "signal $((rc - 128)) - program crashed" 2>/dev/null
        fi
        eips 1 3 "---- last lines of log.txt ----" 2>/dev/null
        tail -n 14 "$APPLOG" 2>/dev/null > "$DATA/.tail"
        show_file "$DATA/.tail" 18
        eips 1 20 "screen will return in 25s" 2>/dev/null
        sleep 25
        rm -f "$DATA/.tail"
    fi

    cleanup
    trap - EXIT INT TERM HUP
    ;;
esac
