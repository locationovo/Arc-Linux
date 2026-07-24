/* Two operational modes:
 *   Normal (default) — full hardening, suitable for daily use.
 *   Devmode           — relaxed restrictions for development tooling.
 *     Triggered by adding "arc-linux.devmode" to the kernel command line.
 *     In devmode: ptrace, dmesg, kptr are unrestricted; no_new_privs
 *     and core-dump suppression are disabled so that debuggers, sudo,
 *     and kernel introspection tools work as expected.
 *     Core hardening (ASLR, link protection, network lockdown, mount
 *     flags) remains active in both modes.
 *
 * ARM compatibility: cross-compile with same source; adjust TTY_RATE
 * to "115200" and TTY_TERM to "vt100" for serial console if needed.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

/* ---- tunables ---- */
#define MAX_TTY         6
#define MAX_RC_SCRIPTS  256
#define RC_DIR          "/etc/rc.d"
#define HOSTNAME_FILE   "/etc/hostname"
#define OS_RELEASE_FILE "/etc/os-release"
#define FSTAB_FILE      "/etc/fstab"
#define SHELL_PATH      "/bin/sh"
#define AGETTY_PATH     "/sbin/agetty"
#define TTY_RATE        "38400"
#define TTY_TERM        "linux"
#define DEV_MODE_TOKEN  "arc-linux.devmode"
#define PATH_MAX_LEN  512
#define LINE_BUF_SIZE  256

/* ---- globals ---- */
static volatile sig_atomic_t g_shutdown_pending = 0;
static volatile sig_atomic_t g_reboot_pending   = 0;
static volatile sig_atomic_t g_sigchld_received = 0;

typedef struct {
    int   tty_num;
    pid_t pid;
    int   respawn;
} tty_info;

static tty_info g_ttys[MAX_TTY];

/* ---- logging ---- */
static void
log_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ---- mount helper ---- */
static void
mount_fs(const char *source, const char *target, const char *fstype,
         unsigned long flags, const char *data)
{
    if (mount(source, target, fstype, flags, data) < 0)
        log_msg("init: mount %s failed: %s\n", target, strerror(errno));
}

/* ---- development mode check ---- */
static int
is_development_mode(void)
{
    static int cached = -1;
    if (cached >= 0)
        return cached;

    FILE *fp = fopen("/proc/cmdline", "r");
    if (!fp) {
        cached = 0;
        return 0;
    }

    char *line = NULL;
    size_t len = 0;
    cached = 0;

    if (getline(&line, &len, fp) > 0) {
        char *saveptr = NULL;
        char *token = strtok_r(line, " \t\n", &saveptr);
        while (token) {
            if (strcmp(token, DEV_MODE_TOKEN) == 0) {
                cached = 1;
                break;
            }
            token = strtok_r(NULL, " \t\n", &saveptr);
        }
    }
    free(line);
    fclose(fp);
    return cached;
}

/* ---- filesystem mounting ---- */
static void
mount_filesystems(void)
{
    mount_fs("proc", "/proc", "proc",
             MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
    mount_fs("sysfs", "/sys", "sysfs",
             MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);

    /*
     * devtmpfs is auto-mounted by the kernel before /init runs,
     * via CONFIG_DEVTMPFS_MOUNT=y. /dev is already available,
     * so we skip the mount call and proceed directly.
     */

    mount_fs("tmpfs", "/run", "tmpfs",
             MS_NOSUID | MS_NODEV, "mode=0755,size=10%");
    mount_fs("tmpfs", "/tmp", "tmpfs",
             MS_NOSUID | MS_NODEV, "mode=1777,size=25%");

    if (mkdir("/dev/pts", 0755) < 0 && errno != EEXIST)
        log_msg("init: mkdir /dev/pts failed: %s\n", strerror(errno));
    mount_fs("devpts", "/dev/pts", "devpts",
             MS_NOSUID | MS_NOEXEC, "mode=0620,gid=5");

    if (mkdir("/dev/shm", 1777) < 0 && errno != EEXIST)
        log_msg("init: mkdir /dev/shm failed: %s\n", strerror(errno));
    mount_fs("tmpfs", "/dev/shm", "tmpfs",
             MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=1777,size=50%");
}

/* ---- fstab-based mounting ---- */
static void
mount_fstab(void)
{
    FILE *fp = setmntent(FSTAB_FILE, "r");
    if (!fp) {
        log_msg("init: cannot open %s: %s\n", FSTAB_FILE, strerror(errno));
        return;
    }

    struct mntent *me;
    while ((me = getmntent(fp)) != NULL) {
        if (strcmp(me->mnt_dir, "/proc") == 0 ||
            strcmp(me->mnt_dir, "/sys")  == 0 ||
            strcmp(me->mnt_dir, "/dev")  == 0)
            continue;

        if (mkdir(me->mnt_dir, 0755) < 0 && errno != EEXIST)
            log_msg("init: mkdir %s failed: %s\n",
                    me->mnt_dir, strerror(errno));

        if (mount(me->mnt_fsname, me->mnt_dir, me->mnt_type,
                  0, me->mnt_opts) < 0)
            log_msg("init: mount %s failed: %s\n",
                    me->mnt_dir, strerror(errno));
    }
    endmntent(fp);
}

/* ---- sysctl helpers ---- */
static void
sysctl_write(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return;
    ssize_t len = (ssize_t)strlen(value);
    if (write(fd, value, (size_t)len) != len)
        log_msg("init: sysctl write to %s failed: %s\n",
                path, strerror(errno));
    close(fd);
}

/* ---- kernel hardening ---- */
static void
apply_sysctl_hardening(void)
{
    int devmode = is_development_mode();

    sysctl_write("/proc/sys/kernel/randomize_va_space", "2");

    if (devmode) {
        sysctl_write("/proc/sys/kernel/kptr_restrict", "0");
        sysctl_write("/proc/sys/kernel/dmesg_restrict", "0");
        sysctl_write("/proc/sys/kernel/yama/ptrace_scope", "0");
        sysctl_write("/proc/sys/fs/suid_dumpable", "1");
        log_msg("init: devmode active — ptrace, dmesg, kptr unrestricted\n");
    } else {
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
            log_msg("init: PR_SET_NO_NEW_PRIVS failed: %s\n", strerror(errno));

        sysctl_write("/proc/sys/kernel/kptr_restrict", "2");
        sysctl_write("/proc/sys/kernel/dmesg_restrict", "1");
        sysctl_write("/proc/sys/kernel/yama/ptrace_scope", "1");
        sysctl_write("/proc/sys/fs/suid_dumpable", "0");
    }
}

/* ---- hostname ---- */
static void
set_hostname(void)
{
    FILE *fp = fopen(HOSTNAME_FILE, "r");
    if (!fp)
        return;

    char buf[256];
    if (fgets(buf, sizeof(buf), fp)) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';
        if (len > 0 && sethostname(buf, len) < 0)
            log_msg("init: sethostname failed: %s\n", strerror(errno));
    }
    fclose(fp);
}

/* ---- os-release display ---- */
static void
print_os_release(void)
{
    FILE *fp = fopen(OS_RELEASE_FILE, "r");
    if (!fp)
        return;

    char *line = NULL;
    size_t len = 0;
    int found = 0;

    while (getline(&line, &len, fp) > 0) {
        char *cp = line;
        while (*cp == ' ' || *cp == '\t') cp++;
        if (strncmp(cp, "PRETTY_NAME=", 12) == 0) {
            cp += 12;
            size_t l = strlen(cp);
            while (l > 0 && (cp[l - 1] == '\n' || cp[l - 1] == '\r'
                             || cp[l - 1] == '"' || cp[l - 1] == '\''))
                cp[--l] = '\0';
            if (l > 0) {
                log_msg("init: %s\n", cp);
                found = 1;
            }
            break;
        }
    }
    free(line);
    fclose(fp);

    if (!found)
        log_msg("init: starting\n");
}

/* ---- network setup (loopback + ethernet) ---- */
static void
setup_network(void)
{
    const char *ifaces[] = { "lo", "eth0", NULL };
    struct ifreq ifr;
    int sock;
    int i;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        log_msg("init: socket() failed: %s\n", strerror(errno));
        return;
    }

    for (i = 0; ifaces[i] != NULL; i++) {
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, ifaces[i], IFNAMSIZ - 1);
        ifr.ifr_name[IFNAMSIZ - 1] = '\0';  /* ensure null-termination */

        if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
            /* Interface doesn't exist, skip silently */
            continue;
        }

        ifr.ifr_flags |= IFF_UP;
        if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0)
            log_msg("init: cannot bring up %s: %s\n",
                    ifaces[i], strerror(errno));
        else
            log_msg("init: %s up\n", ifaces[i]);
    }

    close(sock);
}

/* ---- spawn getty ---- */
static void
spawn_getty(tty_info *tty)
{
    if (!tty->respawn)
        return;

    pid_t pid = fork();
    if (pid < 0) {
        log_msg("init: fork for tty%d failed: %s\n",
                tty->tty_num, strerror(errno));
        return;
    }
    if (pid == 0) {
        char tty_name[32];
        snprintf(tty_name, sizeof(tty_name), "tty%d", tty->tty_num);
        execl(AGETTY_PATH, AGETTY_PATH, tty_name, TTY_RATE, TTY_TERM,
              (char *)NULL);
        log_msg("init: exec %s failed: %s\n", AGETTY_PATH, strerror(errno));
        _exit(127);
    }
    tty->pid = pid;
}

/* ---- rc.d script comparison (for qsort) ---- */
static int
rc_script_cmp(const void *a, const void *b)
{
    const char *sa = *(const char **)a;
    const char *sb = *(const char **)b;
    return strcmp(sa, sb);
}

/* ---- run rc.d scripts ---- */
static void
run_rc_scripts(const char *action)
{
    DIR *d = opendir(RC_DIR);
    if (!d)
        return;

    char *scripts[MAX_RC_SCRIPTS];
    int count = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && count < MAX_RC_SCRIPTS) {
        if (de->d_name[0] == '.')
            continue;

        char path[PATH_MAX_LEN];
        int n = snprintf(path, sizeof(path), RC_DIR "/%s", de->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            log_msg("init: path too long for %s, skipping\n", de->d_name);
            continue;
        }

        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            char *dup = strdup(de->d_name);
            if (!dup) {
                log_msg("init: strdup failed for %s, skipping\n", de->d_name);
                continue;
            }
            scripts[count++] = dup;
        }
    }
    closedir(d);

    if (count == 0)
        return;

    qsort(scripts, (size_t)count, sizeof(char *), rc_script_cmp);

    int start = 0, end = count, step = 1;
    if (strcmp(action, "stop") == 0) {
        start = count - 1;
        end   = -1;
        step  = -1;
    }

    for (int i = start; i != end; i += step) {
        char path[PATH_MAX_LEN];
        int n = snprintf(path, sizeof(path), RC_DIR "/%s", scripts[i]);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            log_msg("init: path too long for %s, skipping\n", scripts[i]);
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            log_msg("init: fork failed for %s: %s\n", path, strerror(errno));
            continue;
        }
        if (pid == 0) {
            execl(SHELL_PATH, SHELL_PATH, path, action, (char *)NULL);
            log_msg("init: exec %s failed: %s\n", path, strerror(errno));
            _exit(127);
        }
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            log_msg("init: %s %s exited with %d\n",
                    path, action, WEXITSTATUS(status));
    }

    for (int i = 0; i < count; i++)
        free(scripts[i]);
}

/* ---- reap children ---- */
static void
reap_children(void)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < MAX_TTY; i++) {
            if (g_ttys[i].pid == pid) {
                g_ttys[i].pid = 0;
                if (g_ttys[i].respawn)
                    spawn_getty(&g_ttys[i]);
                break;
            }
        }
    }
}

/* ---- kill all processes ---- */
static void
kill_all_processes(int sig)
{
    DIR *d = opendir("/proc");
    if (!d)
        return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9')
            continue;
        char *endptr = NULL;
        long val = strtol(de->d_name, &endptr, 10);
        if (*endptr != '\0' || val <= 1)
            continue;
        kill((pid_t)val, sig);
    }
    closedir(d);
}

/* ---- signal handler ---- */
static void
sig_handler(int sig)
{
    switch (sig) {
    case SIGTERM:
    case SIGUSR2:
        g_shutdown_pending = 1;
        break;
    case SIGINT:
    case SIGUSR1:
        g_reboot_pending = 1;
        break;
    case SIGCHLD:
        g_sigchld_received = 1;
        break;
    }
}

/* ---- setup signals ---- */
static void
setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;

    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);

    struct sigaction sa_ignore;
    memset(&sa_ignore, 0, sizeof(sa_ignore));
    sa_ignore.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa_ignore, NULL);
    sigaction(SIGHUP,  &sa_ignore, NULL);
    sigaction(SIGTSTP, &sa_ignore, NULL);
    sigaction(SIGTTIN, &sa_ignore, NULL);
    sigaction(SIGTTOU, &sa_ignore, NULL);
}

/* ---- shutdown / reboot ---- */
static void
do_shutdown(int reboot_flag)
{
    log_msg("init: %s initiated...\n", reboot_flag ? "reboot" : "shutdown");

    for (int i = 0; i < MAX_TTY; i++) {
        g_ttys[i].respawn = 0;
        if (g_ttys[i].pid > 0)
            kill(g_ttys[i].pid, SIGTERM);
    }

    run_rc_scripts("stop");

    kill_all_processes(SIGTERM);

    struct timespec remaining = { .tv_sec = 1, .tv_nsec = 0 };
    while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR)
        ;

    kill_all_processes(SIGKILL);

    remaining.tv_sec = 1;
    remaining.tv_nsec = 0;
    while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR)
        ;

    sync();

    if (umount("/dev/shm") < 0)
        log_msg("init: umount /dev/shm failed: %s\n", strerror(errno));
    if (umount("/dev/pts") < 0)
        log_msg("init: umount /dev/pts failed: %s\n", strerror(errno));
    if (umount("/tmp") < 0)
        log_msg("init: umount /tmp failed: %s\n", strerror(errno));
    if (umount("/run") < 0)
        log_msg("init: umount /run failed: %s\n", strerror(errno));
    if (umount("/dev") < 0)
        log_msg("init: umount /dev failed: %s\n", strerror(errno));
    if (umount("/sys") < 0)
        log_msg("init: umount /sys failed: %s\n", strerror(errno));
    if (umount("/proc") < 0)
        log_msg("init: umount /proc failed: %s\n", strerror(errno));

    sync();

    if (reboot_flag)
        reboot(RB_AUTOBOOT);
    else
        reboot(RB_POWER_OFF);

    log_msg("init: reboot() returned unexpectedly, hanging\n");
    while (1)
        pause();
}

/* ----load kernel modules---- */
static void
load_kernel_modules(void)
{
    FILE *fp = fopen("/etc/modules", "r");
    if (!fp)
        return;

    char line[LINE_BUF_SIZE];
    char mod_path[PATH_MAX_LEN];

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (len == 0 || line[0] == '#')
            continue;
        
        if (line[0] == '/')
            snprintf(mod_path, sizeof(mod_path), "%s", line);
        else
            snprintf(mod_path, sizeof(mod_path),
                     "/lib/modules/%s", line);

        int fd = open(mod_path, O_RDONLY);
        if (fd < 0) {
            log_msg("init: cannot open module '%s': %s\n",
                    mod_path, strerror(errno));
            continue;
        }

        long ret = syscall(SYS_finit_module, fd, "", 0);
        if (ret < 0)
            log_msg("init: failed to load '%s': %s\n",
                    mod_path, strerror(errno));

        close(fd);
    }
    fclose(fp);
}

/* ----clean up---- */
static void
cleanup_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return;

    struct dirent *de;
    char full[PATH_MAX_LEN];

    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        int n = snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(full))
            continue;

        struct stat st;
        if (lstat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode))
                continue;          /* skip subdirectories, only remove files */
            if (unlink(full) < 0 && errno != ENOENT)
                log_msg("init: unlink %s failed: %s\n",
                        full, strerror(errno));
        }
    }
    closedir(d);
}

static void
cleanup_temp_files(void)
{
    cleanup_dir("/tmp");
    cleanup_dir("/var/tmp");
    cleanup_dir("/var/run");
}

/* ---- main ---- */
int
main(void)
{
    if (getpid() != 1) {
        log_msg("init: must be run as PID 1\n");
        return 1;
    }

    umask(022);

    if (!is_development_mode()) {
        struct rlimit rl = { 0, 0 };
        setrlimit(RLIMIT_CORE, &rl);
    }

    /*
     * Phase 0 — Cleanup
     * Remove stale files from the root filesystem *before* tmpfs
     * mounts cover them.  /tmp, /var/tmp and /var/run must exist as
     * real directories on the rootfs for this to take effect.
     */
    cleanup_temp_files();

    /* Phase 1 — Mount virtual filesystems */
    mount_filesystems();       /* proc, sys, devpts, tmpfs on /tmp, /run */
    mount_fstab();             /* /etc/fstab entries (e.g. /usr, /boot) */

    /* Phase 2 — Host identity */
    set_hostname();
    print_os_release();

    /* Phase 3 — Network */
    setup_network();           /* lo + eth0 (requires iproute2) */

    /* Phase 4 — Kernel modules */
    load_kernel_modules();     /* /etc/modules → finit_module(2) */

    /* Phase 5 — Kernel tuning */
    apply_sysctl_hardening();  /* /etc/sysctl.conf */

    /* Phase 6 — Signal handlers */
    setup_signals();           /* SIGCHLD, SIGINT, SIGUSR1 */

    /* Phase 7 — User rc.d scripts */
    run_rc_scripts("start");

    /* Phase 8 — Spawn login terminals */
    for (int i = 0; i < MAX_TTY; i++) {
        g_ttys[i].tty_num = i + 1;
        g_ttys[i].pid     = 0;
        g_ttys[i].respawn = 1;
        spawn_getty(&g_ttys[i]);
    }

    /* Phase 9 — Main event loop */
    while (1) {
        sigset_t empty_mask;

        if (g_shutdown_pending) {
            g_shutdown_pending = 0;
            do_shutdown(0);
        }
        if (g_reboot_pending) {
            g_reboot_pending = 0;
            do_shutdown(1);
        }
        if (g_sigchld_received) {
            g_sigchld_received = 0;
            reap_children();
        }

        /*
         * sigsuspend() atomically replaces the signal mask and waits,
         * avoiding the race window that exists between the flag check
         * above and a plain pause() call below.
         */
        sigemptyset(&empty_mask);
        sigsuspend(&empty_mask);

        if (g_sigchld_received) {
            g_sigchld_received = 0;
            reap_children();
        }
    }

    return 0;
}
