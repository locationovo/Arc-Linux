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
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---- tunables --------------------------------------------------------- */
#define MAX_TTY         6       /* spawn gettys on tty1 through tty6       */
#define MAX_RC_SCRIPTS  256     /* maximum number of rc.d service scripts   */
#define RC_DIR          "/etc/rc.d"
#define HOSTNAME_FILE   "/etc/hostname"
#define SHELL_PATH      "/bin/sh"
#define AGETTY_PATH     "/sbin/agetty"
#define TTY_RATE        "38400" /* ARM serial: change to "115200"          */
#define TTY_TERM        "linux" /* ARM serial: change to "vt100"           */
#define DEV_MODE_TOKEN  "arc-linux.devmode"

/* ---- globals ----------------------------------------------------------- */
static volatile sig_atomic_t g_shutdown_pending = 0;
static volatile sig_atomic_t g_reboot_pending   = 0;
static volatile sig_atomic_t g_sigchld_received = 0;

/* per-tty child tracking ------------------------------------------------- */
typedef struct {
    int   tty_num;
    pid_t pid;
    int   respawn;
} tty_entry_t;

static tty_entry_t g_ttys[MAX_TTY];

/* ---- devmode detection (cached, called once) --------------------------- */
static int
is_devmode(void)
{
    static int checked = 0;
    static int devmode = 0;

    if (checked)
        return devmode;

    checked = 1;
    FILE *fp = fopen("/proc/cmdline", "r");
    if (!fp)
        return 0;

    char *line = NULL;
    size_t len = 0;
    if (getline(&line, &len, fp) > 0) {
        char *saveptr = NULL;
        char *token   = strtok_r(line, " \t\n\r", &saveptr);
        while (token) {
            if (strcmp(token, DEV_MODE_TOKEN) == 0) {
                devmode = 1;
                break;
            }
            token = strtok_r(NULL, " \t\n\r", &saveptr);
        }
    }
    free(line);
    fclose(fp);
    return devmode;
}

/* ---- logging helpers --------------------------------------------------- */
static void
log_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

/* ---- signal handlers (async-signal-safe only) -------------------------- */
static void
sig_handler(int sig)
{
    switch (sig) {
    case SIGTERM:  g_shutdown_pending = 1; break;
    case SIGINT:   g_reboot_pending   = 1; break;  /* Ctrl-Alt-Del        */
    case SIGUSR1:  g_reboot_pending   = 1; break;
    case SIGUSR2:  g_shutdown_pending = 1; break;
    case SIGCHLD:  g_sigchld_received = 1; break;
    default:       break;
    }
}

static void
setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;

    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);

    /* never let PID 1 be killed or suspended by job-control signals */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP,  SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
}

/* ---- filesystem mounting ----------------------------------------------- */
static int
mount_fs(const char *source, const char *target, const char *fstype,
         unsigned long flags, const char *data)
{
    if (mount(source, target, fstype, flags, data) < 0) {
        log_msg("init: mount %s -> %s failed: %s\n",
                source ? source : "none", target, strerror(errno));
        return -1;
    }
    return 0;
}

static void
mount_filesystems(void)
{
    /* /proc - process information */
    mount_fs("proc", "/proc", "proc",
             MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);

    /* /sys - kernel & device info */
    mount_fs("sysfs", "/sys", "sysfs",
             MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);

    /* /dev - device nodes via devtmpfs (kernel-managed, no mknod needed) */
    mount_fs("devtmpfs", "/dev", "devtmpfs",
             MS_NOSUID, "mode=0755");

    /* /run - runtime data */
    mount_fs("tmpfs", "/run", "tmpfs",
             MS_NOSUID | MS_NODEV, "mode=0755,size=10%");

    /* /tmp - world-writable temporary storage */
    mount_fs("tmpfs", "/tmp", "tmpfs",
             MS_NOSUID | MS_NODEV, "mode=1777,size=25%");

    /* /dev/pts - pseudoterminals */
    if (mkdir("/dev/pts", 0755) < 0 && errno != EEXIST)
        log_msg("init: mkdir /dev/pts failed: %s\n", strerror(errno));
    mount_fs("devpts", "/dev/pts", "devpts",
             MS_NOSUID | MS_NOEXEC, "mode=0620,gid=5");

    /* /dev/shm - shared memory, noexec to prevent abuse */
    if (mkdir("/dev/shm", 1777) < 0 && errno != EEXIST)
        log_msg("init: mkdir /dev/shm failed: %s\n", strerror(errno));
    mount_fs("tmpfs", "/dev/shm", "tmpfs",
             MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=1777");
}

/* ---- sysctl helpers ---------------------------------------------------- */
static void
sysctl_write(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return;
    ssize_t len = (ssize_t)strlen(value);
    if (write(fd, value, (size_t)len) != len) {
        /* best-effort; /proc/sys writes are atomic for small values */
    }
    close(fd);
}

/* ---- kernel hardening -------------------------------------------------- */
static void
apply_sysctl_hardening(void)
{
    int devmode = is_devmode();

    /* ASLR: always full randomization */
    sysctl_write("/proc/sys/kernel/randomize_va_space", "2");

    if (devmode) {
        /* relaxed: allow kernel symbol / log access for debugging */
        sysctl_write("/proc/sys/kernel/kptr_restrict", "0");
        sysctl_write("/proc/sys/kernel/dmesg_restrict", "0");
        /* allow ptrace of any process (gdb, strace, etc.) */
        sysctl_write("/proc/sys/kernel/yama/ptrace_scope", "0");
        /* allow SUID programs to dump core for debugging */
        sysctl_write("/proc/sys/fs/suid_dumpable", "1");
        log_msg("init: devmode active — ptrace, dmesg, kptr unrestricted\n");
    } else {
        /* normal: Arch Linux security baseline */
        sysctl_write("/proc/sys/kernel/kptr_restrict", "1");
        sysctl_write("/proc/sys/kernel/dmesg_restrict", "1");
        sysctl_write("/proc/sys/kernel/yama/ptrace_scope", "1");
        sysctl_write("/proc/sys/fs/suid_dumpable", "0");
    }

    /* always active regardless of mode */
    sysctl_write("/proc/sys/net/core/bpf_jit_harden", "1");
    sysctl_write("/proc/sys/fs/protected_hardlinks", "1");
    sysctl_write("/proc/sys/fs/protected_symlinks", "1");

    /* network hardening — always active */
    sysctl_write("/proc/sys/net/ipv4/ip_forward", "0");
    sysctl_write("/proc/sys/net/ipv4/conf/all/accept_source_route", "0");
    sysctl_write("/proc/sys/net/ipv4/conf/default/accept_source_route", "0");
    sysctl_write("/proc/sys/net/ipv4/conf/all/accept_redirects", "0");
    sysctl_write("/proc/sys/net/ipv4/conf/default/accept_redirects", "0");
    sysctl_write("/proc/sys/net/ipv4/conf/all/send_redirects", "0");
    sysctl_write("/proc/sys/net/ipv4/conf/default/send_redirects", "0");
    sysctl_write("/proc/sys/net/ipv4/tcp_syncookies", "1");

    if (devmode) {
        /* allow kernel module loading for driver development */
        sysctl_write("/proc/sys/kernel/modules_disabled", "0");
    }
    /*
     * In normal mode, modules_disabled is left alone.
     * To lock it down, uncomment the next line (requires all drivers
     * to be built-in or pre-loaded):
     *
     *   if (!devmode) sysctl_write("/proc/sys/kernel/modules_disabled", "1");
     */
}

/* ---- hostname setup ---------------------------------------------------- */
static void
set_hostname(void)
{
    FILE *fp = fopen(HOSTNAME_FILE, "r");
    char  buf;
    if (fp) {
        if (fgets(buf, sizeof(buf), fp)) {
            size_t len = strlen(buf);
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
                buf[--len] = '\0';
            if (len > 0 && sethostname(buf, len) < 0)
                log_msg("init: sethostname failed: %s\n", strerror(errno));
        }
        fclose(fp);
    } else {
        sethostname("localhost", 9);
    }
}

/* ---- rc.d service management ------------------------------------------- */
static int
rc_script_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void
run_rc_scripts(const char *action)
{
    struct dirent *de;
    char          *scripts[MAX_RC_SCRIPTS];
    int            count = 0;

    DIR *d = opendir(RC_DIR);
    if (!d) {
        log_msg("init: %s not found, skipping service scripts\n", RC_DIR);
        return;
    }

    /*
     * Collect regular files.  No pre-check of executable bits —
     * the kernel performs the permission check at execve() time,
     * eliminating the TOCTOU race window between stat() and exec().
     */
    while ((de = readdir(d)) && count < MAX_RC_SCRIPTS) {
        if (de->d_name == '.')
            continue;
        char path;
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

    /* sort alphabetically for deterministic execution order */
    qsort(scripts, (size_t)count, sizeof(char *), rc_script_cmp);

    /* reverse for "stop" action (LIFO dependency-aware teardown) */
    int start = 0, end = count, step = 1;
    if (strcmp(action, "stop") == 0) {
        start = count - 1;
        end   = -1;
        step  = -1;
    }

    for (int i = start; i != end; i += step) {
        char path;
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
            /* rc.d scripts may need SUID tools — do NOT set no_new_privs */
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

/* ---- getty management -------------------------------------------------- */
static void
spawn_getty(tty_entry_t *tty)
{
    pid_t pid = fork();
    if (pid < 0) {
        log_msg("init: fork getty tty%d failed: %s\n",
                tty->tty_num, strerror(errno));
        return;
    }
    if (pid == 0) {
        setsid();

        /*
         * In normal (non-devmode) operation, prevent the getty and its
         * descendants from gaining new privileges via SUID binaries or
         * file capabilities.  In devmode this is skipped so that sudo,
         * su, and other privileged tools work from the console.
         */
        if (!is_devmode())
            prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

        char tty_name;
        int n = snprintf(tty_name, sizeof(tty_name), "tty%d", tty->tty_num);
        if (n < 0 || (size_t)n >= sizeof(tty_name)) {
            log_msg("init: tty name overflow for tty%d\n", tty->tty_num);
            _exit(1);
        }

        int max_fd = (int)sysconf(_SC_OPEN_MAX);
        if (max_fd > 3) {
            for (int fd = 3; fd < max_fd; fd++)
                close(fd);
        }

        execl(AGETTY_PATH, AGETTY_PATH,
              "--noclear", tty_name, TTY_RATE, TTY_TERM, (char *)NULL);
        log_msg("init: exec agetty on %s failed: %s\n",
                tty_name, strerror(errno));
        _exit(127);
    }
    tty->pid     = pid;
    tty->respawn = 1;
    log_msg("init: spawned getty on tty%d (pid %d)\n", tty->tty_num, pid);
}

static void
respawn_getty(tty_entry_t *tty)
{
    tty->pid     = 0;
    tty->respawn = 1;
    spawn_getty(tty);
}

static void
init_gettys(void)
{
    for (int i = 0; i < MAX_TTY; i++) {
        g_ttys[i].tty_num = i + 1;
        g_ttys[i].pid     = 0;
        g_ttys[i].respawn = 1;
        spawn_getty(&g_ttys[i]);
    }
}

/* ---- reap children & respawn gettys ----------------------------------- */
static void
reap_children(void)
{
    int  status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < MAX_TTY; i++) {
            if (g_ttys[i].pid == pid) {
                if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    log_msg("init: getty tty%d (pid %d) terminated, "
                            "respawning\n",
                            g_ttys[i].tty_num, pid);
                }
                if (!g_shutdown_pending && !g_reboot_pending)
                    respawn_getty(&g_ttys[i]);
                else
                    g_ttys[i].pid = 0;
                break;
            }
        }
    }

    if (pid < 0 && errno != ECHILD && errno != EINTR)
        log_msg("init: waitpid error: %s\n", strerror(errno));
}

/* ---- shutdown / reboot ------------------------------------------------- */
static void
kill_all_processes(int sig)
{
    kill(-1, sig);
    /* brief grace period for processes to handle the signal */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000000 }; /* 200ms */
    nanosleep(&ts, NULL);
}

static void
do_shutdown(int reboot)
{
    log_msg("init: %s initiated...\n", reboot ? "reboot" : "shutdown");

    /* stop respawning gettys */
    for (int i = 0; i < MAX_TTY; i++) {
        g_ttys[i].respawn = 0;
        if (g_ttys[i].pid > 0)
            kill(g_ttys[i].pid, SIGTERM);
    }

    /* run rc.d stop scripts in reverse order */
    run_rc_scripts("stop");

    /* terminate remaining processes gracefully */
    kill_all_processes(SIGTERM);

    /* wait at least 1 second, surviving signal interruptions */
    struct timespec remaining = { .tv_sec = 1, .tv_nsec = 0 };
    while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR)
        ;

    kill_all_processes(SIGKILL);

    remaining.tv_sec = 1;
    remaining.tv_nsec = 0;
    while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR)
        ;

    /* flush all filesystem buffers */
    sync();

    /* unmount filesystems in strict reverse mount order */
    umount("/dev/shm");
    umount("/dev/pts");
    umount("/tmp");
    umount("/run");
    umount("/dev");
    umount("/sys");
    umount("/proc");

    /* final sync before the jump to kernel */
    sync();

    if (reboot)
        reboot(RB_AUTOBOOT);
    else
        reboot(RB_POWER_OFF);

    /* reboot() should not return; hang forever if it does */
    while (1)
        pause();
}

/* ---- main -------------------------------------------------------------- */
int
main(void)
{
    /* PID 1 must never exit; the kernel panics if it does */
    if (getpid() != 1) {
        log_msg("init: must be run as PID 1\n");
        return 1;
    }

    /* set a safe umask — Arch Linux default is 022;
     * for stricter security, change to 027 (no world-readable by default) */
    umask(022);

    /*
     * In normal mode, disable core dumps for PID 1 and all children.
     * In devmode, leave the rlimit untouched so the user can enable
     * core dumps with "ulimit -c unlimited" for debugging.
     */
    if (!is_devmode()) {
        struct rlimit rl = { 0, 0 };
        setrlimit(RLIMIT_CORE, &rl);
    }

    /* early setup: mount virtual filesystems */
    mount_filesystems();

    /* create essential /dev symlinks if missing */
    if (symlink("/proc/self/fd",   "/dev/fd")     < 0 && errno != EEXIST)
        log_msg("init: symlink /dev/fd failed: %s\n", strerror(errno));
    if (symlink("/proc/self/fd/0", "/dev/stdin")  < 0 && errno != EEXIST)
        log_msg("init: symlink /dev/stdin failed: %s\n", strerror(errno));
    if (symlink("/proc/self/fd/1", "/dev/stdout") < 0 && errno != EEXIST)
        log_msg("init: symlink /dev/stdout failed: %s\n", strerror(errno));
    if (symlink("/proc/self/fd/2", "/dev/stderr") < 0 && errno != EEXIST)
        log_msg("init: symlink /dev/stderr failed: %s\n", strerror(errno));

    /* set hostname before any service needs it */
    set_hostname();

    /* apply kernel hardening */
    apply_sysctl_hardening();

    /* install signal handlers */
    setup_signals();

    /* run startup services in alphabetical order */
    run_rc_scripts("start");

    /* spawn virtual terminals */
    init_gettys();

    log_msg("init: system initialization complete, entering main loop\n");

    /* ---- main event loop ---- */
    while (1) {
        /* check for shutdown/reboot requests */
        if (g_shutdown_pending) {
            do_shutdown(0);
            /* unreachable */
        }
        if (g_reboot_pending) {
            do_shutdown(1);
            /* unreachable */
        }

        /* reap terminated children and respawn gettys */
        if (g_sigchld_received) {
            g_sigchld_received = 0;
            reap_children();
        }

        /*
         * pause() blocks until a signal is delivered.
         * SA_RESTART ensures it restarts automatically after handled
         * signals, so we only wake up here when a signal actually
         * needs attention (shutdown, reboot, or SIGCHLD).
         */
        pause();

        /* drain any SIGCHLD that arrived during pause() handling */
        if (g_sigchld_received) {
            g_sigchld_received = 0;
            reap_children();
        }
    }

    return 0; /* unreachable */
}
