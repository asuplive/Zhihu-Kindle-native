#!/bin/sh
# 在屏幕上显示最近日志（不需要 SSH、不需要电脑）
#
# 注意：程序真正的运行日志是 log.txt，run.log 只记"启动/退出/安装"。
# 以前这里读的是 run.log，所以出问题时只能看到一行 installed，白折腾。
#
# 1.2.3 起多显示三样东西，都是定位"整机重启"的关键：
#   · 挂起统计（/sys/power/suspend_stats）：睡没睡、睡失败几次
#   · 内核日志尾巴（kmsg.log）：panic / 看门狗会在里面留最后一句话
#   · 刷新路径（log.txt 里的 eink=）：原生 mxcfb 还是 eips 后路

DATA=/mnt/us/zhihu
eips -c 2>/dev/null

show() {   # show <文件> <起始行> <最多行数>
    [ -f "$1" ] || return 0
    i=$2
    n=0
    tail -n 16 "$1" 2>/dev/null | while IFS= read -r line; do
        n=$((n + 1))
        [ $n -gt "$3" ] && break
        eips 1 $i "$line" 2>/dev/null
        i=$((i + 1))
    done
}

eips 1 1 "== log.txt (program) ==" 2>/dev/null
show "$DATA/log.txt" 2 12

eips 1 15 "== run.log (launcher) ==" 2>/dev/null
show "$DATA/run.log" 16 4

# 挂起统计：success 涨了说明机器真的睡过；fail 非 0 就是"睡了没醒过来"
S=""
for f in success fail last_failed_dev; do
    [ -r "/sys/power/suspend_stats/$f" ] || continue
    S="$S $f=$(cat /sys/power/suspend_stats/$f 2>/dev/null)"
done
[ -n "$S" ] && eips 1 21 "suspend:$S" 2>/dev/null

# 内核最后一行：硬重启前的那句话通常就在这里
if [ -s "$DATA/kmsg.log" ]; then
    tail -n 3 "$DATA/kmsg.log" 2>/dev/null | while IFS= read -r line; do
        eips 1 22 "$line" 2>/dev/null
        break
    done
else
    eips 1 22 "kmsg.log: (empty - dmesg blocked?)" 2>/dev/null
fi

eips 1 24 "files: log.txt run.log kmsg.log eips_spy.log" 2>/dev/null
