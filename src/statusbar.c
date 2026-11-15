/* statusbar.c —— 见 statusbar.h 头注释 */
#include "statusbar.h"

#include "common.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* /proc/<pid>/stat 的进程状态字母。comm 里可能带空格，从 ')' 之后取。 */
static int proc_state(int pid) {
    char p[64];
    snprintf(p, sizeof(p), "/proc/%d/stat", pid);
    char buf[512];
    if (read_file_into(p, buf, sizeof(buf)) <= 0) return '?';
    char *rp = strrchr(buf, ')');
    if (!rp || rp[1] == 0 || rp[2] == 0) return '?';
    return rp[2];
}

/* 扫 /proc 找 awesome 的 pid。模拟器/无 WM 环境返回 -1，所有路径都要容忍。 */
static int awesome_pid(void) {
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *e;
    int pid = -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[96];
        snprintf(p, sizeof(p), "/proc/%s/comm", e->d_name);
        char buf[64];
        if (read_file_into(p, buf, sizeof(buf)) <= 0) continue;
        size_t l = strlen(buf);
        while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = 0;
        if (strcmp(buf, "awesome") == 0) {
            pid = atoi(e->d_name);
            break;
        }
    }
    closedir(d);
    return pid;
}

static const char *marker_path(void) {
    static char p[320];
    snprintf(p, sizeof(p), "%s/.awesome_stopped", zh_home());
    return p;
}

/* fork + execvp + 限时 wait，与 main.c 的 run_quiet 同款（那边是 static）。 */
static void run_quiet(const char *prog, const char *a1, const char *a2,
                      const char *a3, const char *a4) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); if (dn > 2) close(dn); }
        char *argv[6];
        int i = 0;
        argv[i++] = (char *)prog;
        if (a1) argv[i++] = (char *)a1;
        if (a2) argv[i++] = (char *)a2;
        if (a3) argv[i++] = (char *)a3;
        if (a4) argv[i++] = (char *)a4;
        argv[i] = NULL;
        execvp(prog, argv);
        _exit(127);
    }
    for (int k = 0; k < 40; k++) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) return;
        usleep(50 * 1000);
    }
    kill(pid, SIGKILL);
}

void statusbar_hide(void) {
    int pid = awesome_pid();
    int already = (pid > 0 && proc_state(pid) == 'T');
    if (!already) {
        run_quiet("lipc-set-prop", "com.lab126.pillow",
                  "disableEnablePillow", "disable", NULL);
        if (pid > 0 && kill(pid, SIGSTOP) == 0)
            logmsg("状态栏: pillow 已禁用 + awesome(pid=%d) 已暂停（showbar=0，"
                   "不碰 cvm，无看门狗风险；退出时恢复）", pid);
        else
            logmsg("状态栏: pillow 禁用指令已发（awesome %s）",
                   pid > 0 ? "暂停失败" : "未找到——模拟器或非标准环境，忽略");
    }
    /* 标记每次都刷新：pid 可能变（系统重启过 awesome），恢复方按标记取 pid。 */
    char b[32];
    int n = snprintf(b, sizeof(b), "%d\n", pid > 0 ? pid : 0);
    if (n > 0 && (size_t)n < sizeof(b))
        write_file_atomic(marker_path(), b, (size_t)n);
}

void statusbar_restore(void) {
    statusbar_restore_safe();
    run_quiet("lipc-set-prop", "com.lab126.pillow",
              "disableEnablePillow", "enable", NULL);
    logmsg("状态栏: 已恢复（awesome SIGCONT + pillow enable）");
}

void statusbar_restore_safe(void) {
    char b[32] = { 0 };
    int pid = 0;
    if (read_file_into(marker_path(), b, sizeof(b) - 1) > 0) pid = atoi(b);
    if (pid > 0 && kill(pid, SIGCONT) == 0)
        logmsg_sync("状态栏: awesome(pid=%d) 已 SIGCONT", pid);
    unlink(marker_path());
}
