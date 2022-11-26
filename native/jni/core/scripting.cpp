#include <string>
#include <vector>
#include <sys/wait.h>

#include <magisk.hpp>
#include <utils.hpp>
#include <selinux.hpp>
#include <daemon.hpp>

#include "core.hpp"

using namespace std;

#define BBEXEC_CMD bbpath(), "sh"

static const char *bbpath() {
    static string path;
    if (path.empty())
        path = MAGISKTMP + "/" BBPATH "/busybox";
    return path.data();
}

static void set_script_env() {
    setenv("ASH_STANDALONE", "1", 1);
    char new_path[4096];
    sprintf(new_path, "%s:%s", getenv("PATH"), MAGISKTMP.data());
    setenv("PATH", new_path, 1);
    if (zygisk_enabled)
        setenv("ZYGISK_ENABLED", "1", 1);
};

void exec_script(const char *script) {
    exec_t exec {
        .pre_exec = set_script_env,
        .fork = fork_no_orphan
    };
    exec_command_sync(exec, BBEXEC_CMD, script);
}

static timespec pfs_timeout;

#define PFS_SETUP() \
if (pfs) { \
    if (int pid = xfork()) { \
        if (pid < 0) \
            return; \
        /* In parent process, simply wait for child to finish */ \
        waitpid(pid, nullptr, 0); \
        return; \
    } \
    timer_pid = xfork(); \
    if (timer_pid == 0) { \
        /* In timer process, count down */ \
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &pfs_timeout, nullptr); \
        exit(0); \
    } \
}

#define PFS_WAIT() \
if (pfs) { \
    /* If we ran out of time, don't block */ \
    if (timer_pid < 0) \
        continue; \
    if (int pid = waitpid(-1, nullptr, 0); pid == timer_pid) { \
        LOGW("* post-fs-data scripts blocking phase timeout\n"); \
        timer_pid = -1; \
    } \
}

#define PFS_DONE() \
if (pfs) { \
    if (timer_pid > 0) \
        kill(timer_pid, SIGKILL); \
    exit(0); \
}

void exec_common_scripts(const char *stage) {
    LOGI("* Running %s.d scripts\n", stage);
    char path[4096];
    char *name = path + sprintf(path, SECURE_DIR "/%s.d", stage);
    auto dir = xopen_dir(path);
    if (!dir) return;

    bool pfs = stage == "post-fs-data"sv;
    int timer_pid = -1;
    if (pfs) {
        // Setup timer
        clock_gettime(CLOCK_MONOTONIC, &pfs_timeout);
        pfs_timeout.tv_sec += POST_FS_DATA_SCRIPT_MAX_TIME;
    }
    PFS_SETUP()

    *(name++) = '/';
    int dfd = dirfd(dir.get());
    for (dirent *entry; (entry = xreaddir(dir.get()));) {
        if (entry->d_type == DT_REG) {
            if (faccessat(dfd, entry->d_name, X_OK, 0) != 0)
                continue;
            LOGI("%s.d: exec [%s]\n", stage, entry->d_name);
            strcpy(name, entry->d_name);
            exec_t exec {
                .pre_exec = set_script_env,
                .fork = pfs ? xfork : fork_dont_care
            };
            exec_command(exec, BBEXEC_CMD, path);
            PFS_WAIT()
        }
    }

    PFS_DONE()
}

static bool operator>(const timespec &a, const timespec &b) {
    if (a.tv_sec != b.tv_sec)
        return a.tv_sec > b.tv_sec;
    return a.tv_nsec > b.tv_nsec;
}

void exec_module_scripts(const char *stage, const vector<string_view> &modules) {
    LOGI("* Running module %s scripts\n", stage);
    if (modules.empty())
        return;

    bool pfs = stage == "post-fs-data"sv;
    if (pfs) {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        // If we had already timed out, treat it as service mode
        if (now > pfs_timeout)
            pfs = false;
    }
    int timer_pid = -1;
    PFS_SETUP()

    char path[4096];
    for (auto &m : modules) {
        const char *module = m.data();
        sprintf(path, MODULEROOT "/%s/%s.sh", module, stage);
        if (access(path, F_OK) == -1)
            continue;
        LOGI("%s: exec [%s.sh]\n", module, stage);
        exec_t exec {
            .pre_exec = set_script_env,
            .fork = pfs ? xfork : fork_dont_care
        };
        exec_command(exec, BBEXEC_CMD, path);
        PFS_WAIT()
    }

    PFS_DONE()
}

constexpr char install_script[] = R"EOF(
APK=%s
log -t Magisk "apk_install: $APK"
log -t Magisk "apk_install: $(pm install -r $APK 2>&1)"
rm -f $APK
)EOF";

void install_apk(const char *apk) {
    setfilecon(apk, "u:object_r:" SEPOL_FILE_TYPE ":s0");
    exec_t exec {
        .fork = fork_no_orphan
    };
    char cmds[sizeof(install_script) + 4096];
    sprintf(cmds, install_script, apk);
    exec_command_sync(exec, "/system/bin/sh", "-c", cmds);
}

[[noreturn]] __printflike(2, 3)
static void abort(FILE *fp, const char *fmt, ...) {
    va_list valist;
    va_start(valist, fmt);
    vfprintf(fp, fmt, valist);
    fprintf(fp, "\n\n");
    va_end(valist);
    exit(1);
}

constexpr char install_module_script[] = R"EOF(
exec $(magisk --path)/.magisk/busybox/busybox sh -c '
. /data/adb/magisk/util_functions.sh
install_module
exit 0'
)EOF";

void install_module(const char *file) {
    if (getuid() != 0)
        abort(stderr, "Run this command with root");
    if (access(DATABIN, F_OK) ||
        access(DATABIN "/busybox", X_OK) ||
        access(DATABIN "/util_functions.sh", F_OK))
        abort(stderr, "Incomplete Magisk install");
    if (access(file, F_OK))
        abort(stderr, "'%s' does not exist", file);

    char *zip = realpath(file, nullptr);
    setenv("OUTFD", "1", 1);
    setenv("ZIPFILE", zip, 1);
    setenv("ASH_STANDALONE", "1", 1);
    free(zip);

    int fd = xopen("/dev/null", O_RDONLY);
    xdup2(fd, STDERR_FILENO);
    close(fd);

    const char *argv[] = { "/system/bin/sh", "-c", install_module_script, nullptr };
    execve(argv[0], (char **) argv, environ);
    abort(stdout, "Failed to execute BusyBox shell");
}