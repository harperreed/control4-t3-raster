/* Custom PID-1 init for the repurposed Control4 T3 (RK3188).
 *
 * Replaces Android's /init entirely.
 *
 * Key lesson from bring-up: this Android-4.4-era kernel does NOT provide a
 * working devtmpfs (Android populates /dev via ueventd, not devtmpfs). So we
 * mount a tmpfs on /dev and create the device nodes ourselves -- essential
 * char nodes via mknod(), and the NAND mtdblock nodes after the NAND module
 * loads. Without this, /dev is empty: no /dev/urandom (dropbear dies), no
 * block nodes (partition mounts fail), no /dev/kmsg (no logging) -- which is
 * exactly the silent early-death we hit on the first custom boots.
 *
 * Observability without UART or a working USB console: every step is logged
 * to /data/mmkinit-boot.log (opened+written+closed per line, so a later hang
 * or crash still leaves a complete trail we can read after recovering to
 * stock), and to /dev/kmsg (readable via `dmesg` once SSH is up).
 *
 * Bring-up order is chosen so the network/SSH comes up BEFORE the riskier
 * optional steps, so a later failure still leaves the device reachable.
 *
 * Runs as PID 1: never returns. Phase 3 will execve() into the LVGL app.
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BOOTLOG "/data/mmkinit-boot.log"

static int data_mounted = 0;

/* Everything logged before /data mounts (network/eth/dhcp/wifi bring-up — the
 * exact stuff we need for post-mortem) used to go ONLY to volatile kmsg/console,
 * so it never reached the durable on-disk log. We now also accumulate every line
 * in memory and flush the whole backlog the instant /data mounts. Combined with
 * O_SYNC+fsync on each durable write, a freeze/power-cut can no longer erase the
 * trail (it did on the 10": delayed-alloc dropped the un-synced log entirely). */
static char log_buf[32768];
static size_t log_len = 0;
static int log_flushed = 0; /* backlog written to disk once */

static void log_buf_append(const char *msg) {
    size_t n = strlen(msg);
    if (log_len + n + 1 < sizeof(log_buf)) {
        memcpy(log_buf + log_len, msg, n);
        log_len += n;
        log_buf[log_len++] = '\n';
    }
}

static void log_line(const char *msg) {
    /* kernel ring buffer (dmesg-readable over SSH later) */
    int fd = open("/dev/kmsg", O_WRONLY | O_NOCTTY);
    if (fd >= 0) {
        dprintf(fd, "mmkeypad-init: %s\n", msg);
        close(fd);
    }
    /* always keep an in-RAM copy so the pre-/data-mount trail can be flushed */
    log_buf_append(msg);
    /* durable on-disk log, once /data is mounted. O_SYNC + fsync: a freeze or
     * power-cut AFTER this returns cannot drop the line. */
    if (data_mounted) {
        fd = open(BOOTLOG, O_WRONLY | O_CREAT | O_APPEND | O_SYNC, 0644);
        if (fd >= 0) {
            dprintf(fd, "%s\n", msg);
            fsync(fd);
            close(fd);
        }
    }
    fd = open("/dev/console", O_WRONLY | O_NOCTTY);
    if (fd >= 0) {
        dprintf(fd, "mmkeypad-init: %s\n", msg);
        close(fd);
    }
}

/* Called right after /data mounts: write the entire accumulated backlog (every
 * line logged before the mount) to disk, once, durably. */
static void log_flush_backlog(void) {
    if (log_flushed || !data_mounted) return;
    int fd = open(BOOTLOG, O_WRONLY | O_CREAT | O_APPEND | O_SYNC, 0644);
    if (fd >= 0) {
        ssize_t w = write(fd, log_buf, log_len);
        (void)w;
        fsync(fd);
        close(fd);
    }
    log_flushed = 1;
}

/* Append a small proc/sys file's contents to the boot log (tagged), so the real
 * device state (interfaces, wifi chip, sdio ids) lands in the durable trail we
 * read back over the loader-USB link. No-op-logs a missing/empty file. */
static void log_file(const char *tag, const char *path) {
    char hdr[200];
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(hdr, sizeof(hdr), "%s: %s absent", tag, path);
        log_line(hdr);
        return;
    }
    char buf[2048];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        snprintf(hdr, sizeof(hdr), "%s: %s empty", tag, path);
        log_line(hdr);
        return;
    }
    buf[n] = 0;
    snprintf(hdr, sizeof(hdr), "%s <%s>:", tag, path);
    log_line(hdr);
    char *save = NULL;
    for (char *ln = strtok_r(buf, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        char l2[600];
        snprintf(l2, sizeof(l2), "  %s", ln);
        log_line(l2);
    }
}

/* Read the first byte of a sysfs file (e.g. /sys/class/net/eth0/carrier). */
static int read_first_char(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char c = -1;
    if (read(fd, &c, 1) != 1) c = -1;
    close(fd);
    return c;
}

/* Return the IPv4 of an interface as a string (and whether it has one). */
static int iface_ip(const char *ifn, char *out, int n) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifn);
    int ok = 0;
    if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
        struct sockaddr_in *si = (struct sockaddr_in *)&ifr.ifr_addr;
        if (si->sin_addr.s_addr) {
            ok = 1;
            if (out && n > 0) snprintf(out, n, "%s", inet_ntoa(si->sin_addr));
        }
    }
    close(s);
    return ok;
}

static void make_node(const char *path, mode_t mode, int maj, int min) {
    mknod(path, mode, makedev(maj, min)); /* EEXIST is fine */
}

static void run(char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        execv(argv[0], argv);
        _exit(127);
    }
    if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    }
}

static pid_t spawn(char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execv(argv[0], argv);
        _exit(127);
    }
    return pid;
}

/* Like spawn(), but the child's stdout+stderr go to `logpath` instead of the
 * inherited console (which is the hardware UART -- console=ttyFIQ0 -- and thus
 * invisible over SSH). The previous run's log is rotated to "<logpath>.prev" so
 * a crash's final output survives the respawn. Used for the app so its status
 * (audio_start, wifi, SIP, etc.) is readable with `cat /data/mmkeypad.log`. */
static pid_t spawn_logged(char *const argv[], const char *logpath) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        char prev[256];
        snprintf(prev, sizeof prev, "%s.prev", logpath);
        rename(logpath, prev); /* keep last run for crash post-mortems */
        int fd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
        execv(argv[0], argv);
        _exit(127);
    }
    return pid;
}

static void insmod(const char *path) {
    char *argv[] = {"/bin/busybox", "insmod", (char *)path, NULL};
    run(argv);
}

/* insmod with a module parameter string (e.g. mali's clock/dvfs args). */
static void insmod_p(const char *path, const char *param) {
    char *argv[] = {"/bin/busybox", "insmod", (char *)path, (char *)param, NULL};
    run(argv);
}

/* Resolve the kernel-matched build of a /system vendor module. Stock Control4
 * ships TWO vermagic builds of each vendor .ko in /system/lib/modules: the plain
 * name (built for the 3.0.8+ kernel) and a "<name>.<uname -r>" variant (e.g.
 * mali.ko.3.0.36+) for the newer kernel; it insmods whichever matches. We mirror
 * that: return the "<path>.<release>" variant if it exists on disk, else the
 * plain path. This is what makes the 3.0.36+ vendor blobs load on our 4.0.0
 * kernel instead of getting vermagic-rejected. `out` must be >= 256B. */
static const char *kver_module(const char *path, char *out, size_t n) {
    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(out, n, "%s.%s", path, u.release);
        if (access(out, F_OK) == 0) return out;
    }
    return path;
}

static void insmod_kver(const char *path, const char *param) {
    char buf[256];
    const char *chosen = kver_module(path, buf, sizeof buf);
    if (param) insmod_p(chosen, param);
    else insmod(chosen);
}

/* Find an mtd partition index by name from /proc/mtd. Lines look like:
 *   mtd3: 01d00000 00020000 "system"
 * Returns the integer N (from mtdN) or -1 if not found. */
static int mtd_index_by_name(const char *name) {
    FILE *f = fopen("/proc/mtd", "r");
    if (!f)
        return -1;
    char line[256];
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        int idx;
        char nm[64];
        /* mtdN: <size> <erasesize> "name" */
        if (sscanf(line, "mtd%d: %*x %*x \"%63[^\"]\"", &idx, nm) == 2) {
            if (strcmp(nm, name) == 0) {
                found = idx;
                break;
            }
        }
    }
    fclose(f);
    return found;
}

/* mknod /dev/mtdblockN (major 31) then mount it ext4 at target. */
static int mount_mtd(const char *name, const char *target, unsigned long flags) {
    int idx = mtd_index_by_name(name);
    char buf[128];
    if (idx < 0) {
        snprintf(buf, sizeof(buf), "mount_mtd(%s): not found in /proc/mtd", name);
        log_line(buf);
        return -1;
    }
    char dev[64];
    snprintf(dev, sizeof(dev), "/dev/mtdblock%d", idx);
    make_node(dev, S_IFBLK | 0600, 31, idx);
    /* noatime/nodiratime are mount FLAGS (MS_*), not ext4 data-string options
     * -- passing them as the data arg makes ext4 reject the mount with
     * "Unrecognized mount option". Fold them into the flags instead. */
    int r = mount(dev, target, "ext4", flags | MS_NOATIME | MS_NODIRATIME, NULL);
    snprintf(buf, sizeof(buf), "mount_mtd(%s -> %s via %s): r=%d", name, target, dev, r);
    log_line(buf);
    return r;
}

/* Bring up an RNDIS ethernet-over-USB gadget so an in-wall unit with no working
 * PoE/WiFi is still reachable over the micro-USB. Drives the stock kernel's
 * legacy android_usb composite (the same sysfs Android's USB tethering uses).
 * Best-effort: no-ops if the node is absent. Static 10.55.0.1/24; dropbear on
 * :22 answers over it. NOTE: enumerates on Linux/Windows hosts; Apple-Silicon
 * macOS has no RNDIS driver, so this is the fleet/field hatch, not the Mac one
 * (Mac side: the loader-USB link + this durable log, or a UART on ttyFIQ0). */
static void usb_gadget_rndis(void) {
    /* RK3188 DWC-OTG (usb20_otg) comes up in OTG/host-detect mode; a gadget
     * never enumerates to a USB *host* (the Mac) until the port is forced to
     * peripheral mode. force_usb_mode: 2 = force device on rk3188 dwc_otg. Try
     * the known sysfs paths; best-effort. THIS is what made the gadget invisible
     * on the Mac (no 2207: device at all, not even a config failure). */
    {
        const char *fm[] = {
            "/sys/devices/platform/usb20_otg/force_usb_mode",
            "/sys/bus/platform/devices/usb20_otg/force_usb_mode",
            "/sys/devices/platform/usb20_otg/usb_mode", NULL};
        for (int i = 0; fm[i]; i++) {
            int mf = open(fm[i], O_WRONLY);
            if (mf >= 0) { ssize_t w = write(mf, "2", 1); (void)w; close(mf);
                char lm[160]; snprintf(lm, sizeof(lm), "usb: forced device mode via %s", fm[i]); log_line(lm); }
        }
        usleep(200 * 1000);
    }
    const char *A = "/sys/class/android_usb/android0";
    if (access(A, F_OK) != 0) {
        log_line("usb: no android_usb gadget node -> skip RNDIS hatch");
        return;
    }
    char p[256];
#define GWR(rel, val)                                                          \
    do {                                                                       \
        snprintf(p, sizeof(p), "%s/%s", A, rel);                               \
        int gf = open(p, O_WRONLY);                                            \
        if (gf >= 0) {                                                         \
            ssize_t gw = write(gf, val, strlen(val));                          \
            (void)gw;                                                          \
            close(gf);                                                         \
        }                                                                      \
    } while (0)
    GWR("enable", "0");
    GWR("idVendor", "2207");
    GWR("idProduct", "0003");
    GWR("f_rndis/ethaddr", "02:11:22:33:44:55");
    GWR("f_acm/instances", "1");    /* one ACM port -> /dev/ttyGS0 */
    /* rndis,acm composite (the KNOWN-WORKING config): RNDIS for Linux/Windows
     * field hosts + a CDC-ACM serial macOS sees as /dev/cu.usbmodem*. IAD device
     * class (239/2/1) is REQUIRED so macOS parses the ACM data interface -- with
     * a bare class the port enumerates but no data flows. (The gadget only failed
     * before because /sys wasn't mounted; the mkdir fix, not the function set,
     * was the real fix.) */
    GWR("functions", "rndis,acm");
    GWR("bDeviceClass", "239");      /* 0xEF Misc */
    GWR("bDeviceSubClass", "2");
    GWR("bDeviceProtocol", "1");     /* Interface Association Descriptor */
    GWR("enable", "1");
#undef GWR
    usleep(300 * 1000); /* let the rndis0/usb0 netdev + ttyGS0 appear */
    char *up1[] = {"/bin/busybox", "ifconfig", "rndis0", "10.55.0.1",
                   "netmask", "255.255.255.0", "up", NULL};
    char *up2[] = {"/bin/busybox", "ifconfig", "usb0", "10.55.0.1",
                   "netmask", "255.255.255.0", "up", NULL};
    run(up1);
    run(up2);
    log_line("usb: rndis+acm gadget enabled (rndis0 @ 10.55.0.1/24 for Linux/Win; ttyGS0 serial for macOS)");
}

/* Root shell on the USB CDC-ACM port (/dev/ttyGS0), so the unit is reachable
 * over the micro-USB from a Mac (screen /dev/cu.usbmodem* 115200) even with no
 * PoE/WiFi. No working devtmpfs, so resolve ttyGS0's major:minor from sysfs and
 * mknod it ourselves. Returns the child pid (respawned by the main loop when the
 * host disconnects), or -1 if the ACM port never appeared. */
static pid_t spawn_serial_console(void) {
    char devs[32] = {0};
    int fd = open("/sys/class/tty/ttyGS0/dev", O_RDONLY);
    if (fd >= 0) {
        int n = read(fd, devs, sizeof(devs) - 1);
        if (n > 0) devs[n] = 0;
        close(fd);
    }
    int maj = -1, min = -1;
    if (sscanf(devs, "%d:%d", &maj, &min) != 2) {
        log_line("serial console: ttyGS0 absent (ACM function not active)");
        return -1;
    }
    make_node("/dev/ttyGS0", S_IFCHR | 0600, maj, min);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int t = open("/dev/ttyGS0", O_RDWR);
        if (t >= 0) {
            dup2(t, 0);
            dup2(t, 1);
            dup2(t, 2);
            if (t > 2) close(t);
            ioctl(0, TIOCSCTTY, 0);
        }
        char *sh[] = {"/bin/busybox", "sh", "-i", NULL};
        execv(sh[0], sh);
        _exit(127);
    }
    char m[128];
    snprintf(m, sizeof(m), "serial console: root sh on /dev/ttyGS0 (%d:%d) — Mac: screen /dev/cu.usbmodem* 115200", maj, min);
    log_line(m);
    return pid;
}

static void bring_up_network(void) {
    char *lo_up[] = {"/bin/busybox", "ifconfig", "lo", "up", NULL};
    run(lo_up);
    log_file("net.ifaces", "/proc/net/dev");

    char *eth_up[] = {"/bin/busybox", "ifconfig", "eth0", "up", NULL};
    run(eth_up);
    /* Wait for the PoE link to negotiate before DHCP. Stock's dhcpcd waits on
     * link too; our old fire-and-forget `udhcpc -b` could DISCOVER into a
     * not-yet-up link and quietly give up (a suspect for "PoE won't connect").
     * Poll carrier up to ~8s and LOG the outcome. */
    int carrier = 0;
    for (int i = 0; i < 16; i++) {
        if (read_first_char("/sys/class/net/eth0/carrier") == '1') {
            carrier = 1;
            break;
        }
        usleep(500 * 1000);
    }
    log_line(carrier ? "eth0: carrier up" : "eth0: NO carrier after 8s (link/PoE/PHY?)");

    /* Foreground DHCP with retries; log the actual lease. -n: give up per try so
     * we can loop and observe; -q: exit once a lease is obtained. */
    int leased = 0;
    for (int i = 0; i < 4 && !leased; i++) {
        char *dhcp[] = {"/bin/busybox", "udhcpc", "-i", "eth0",
                        "-n", "-q", "-t", "6", "-T", "1", NULL};
        run(dhcp);
        char ip[32];
        if (iface_ip("eth0", ip, sizeof(ip))) {
            char m[80];
            snprintf(m, sizeof(m), "eth0: DHCP lease %s", ip);
            log_line(m);
            leased = 1;
        } else {
            log_line("eth0: no lease this attempt, retrying");
            sleep(2);
        }
    }
    if (!leased) {
        /* Last resort: a backgrounded udhcpc keeps retrying, so a late-asserting
         * link still recovers without another boot. */
        char *dhcpbg[] = {"/bin/busybox", "udhcpc", "-i", "eth0", "-b", NULL};
        spawn(dhcpbg);
        log_line("eth0: still no lease -> backgrounded udhcpc keeps trying");
    }

    /* USB escape hatch alongside wired net (independent of eth/wifi success). */
    usb_gadget_rndis();

    // Sync the clock from NTP. This glass has no RTC battery, so it boots at the
    // epoch; the license trial countdown ("Trial (Pro) · N days left") and any
    // correct timestamps depend on a real clock.
    char *ntp[] = {"/bin/busybox", "ntpd", "-n", "-p", "pool.ntp.org", NULL};
    spawn(ntp);
}

/* ── WiFi driver selection (mirror the stock universal image) ────────────────
 * The stock /system ships Broadcom (rkwifi), Realtek (8723/8189/8188/8192),
 * MediaTek (mt7601) and ESP (esp8089) SDIO drivers and loads the one matching
 * the populated chip. C4 may have changed wifi silicon across production runs,
 * so we must NOT hardcode Broadcom. Detect via the RK framework's chip sysfs and
 * the SDIO ids; unknown/absent falls back to Broadcom rkwifi.oob.ko (the known
 * 7"/10" part) so nothing regresses. */
static void log_sdio_devices(void) {
    DIR *d = opendir("/sys/bus/sdio/devices");
    if (!d) {
        log_line("sdio: /sys/bus/sdio/devices absent");
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[320], v[32] = "?", dv[32] = "?";
        snprintf(p, sizeof(p), "/sys/bus/sdio/devices/%s/vendor", e->d_name);
        int f = open(p, O_RDONLY);
        if (f >= 0) { int n = read(f, v, sizeof(v) - 1); if (n > 0) v[n] = 0; close(f); }
        snprintf(p, sizeof(p), "/sys/bus/sdio/devices/%s/device", e->d_name);
        f = open(p, O_RDONLY);
        if (f >= 0) { int n = read(f, dv, sizeof(dv) - 1); if (n > 0) dv[n] = 0; close(f); }
        char m[200];
        snprintf(m, sizeof(m), "sdio dev %s vendor=%s device=%s", e->d_name, v, dv);
        for (char *q = m; *q; q++) if (*q == '\n' || *q == '\r') *q = ' ';
        log_line(m);
    }
    closedir(d);
}

static const char *wifi_module(void) {
    char chip[80] = "";
    int fd = open("/sys/class/rkwifi/chip", O_RDONLY);
    if (fd >= 0) {
        int n = read(fd, chip, sizeof(chip) - 1);
        if (n > 0) chip[n] = 0;
        close(fd);
    }
    for (char *p = chip; *p; p++) {
        if (*p == '\n' || *p == '\r') *p = 0;
        else if (*p >= 'a' && *p <= 'z') *p -= 32; /* upcase for matching */
    }
    char m[160];
    snprintf(m, sizeof(m), "wifi: /sys/class/rkwifi/chip = '%s'", chip[0] ? chip : "(none)");
    log_line(m);

    static const struct { const char *key, *ko; } map[] = {
        {"AP6", "/system/lib/modules/rkwifi.oob.ko"},  /* Broadcom AP6xxx */
        {"BCM", "/system/lib/modules/rkwifi.oob.ko"},
        {"8723", "/system/lib/modules/8723as.ko"},     /* Realtek RTL8723 SDIO */
        {"8189", "/system/lib/modules/8189es.ko"},
        {"8188", "/system/lib/modules/8188eu.ko"},
        {"8192", "/system/lib/modules/8192cu.ko"},
        {"8089", "/system/lib/modules/esp8089.ko"},
        {"ESP", "/system/lib/modules/esp8089.ko"},
        {"7601", "/system/lib/modules/mt7601sta.ko"},
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strstr(chip, map[i].key)) return map[i].ko;
    return "/system/lib/modules/rkwifi.oob.ko"; /* safe default */
}

/* ── Split-init OTA: the init logic is updatable without reflashing boot ───────
 * This ONE binary plays two roles, chosen by argv[1]:
 *   - BOOTSTRAP (no arg): baked into boot.img as /init, runs as PID 1, NEVER
 *     updated. Does the minimal, can't-fail bring-up (mount /dev/proc/sys, NAND,
 *     /data, /system, network + dropbear + USB serial) so the unit is ALWAYS
 *     reachable, then execs the OTA overlay if one is installed and healthy;
 *     otherwise falls through to run the built-in worker itself.
 *   - OVERLAY ("overlay <dropbear_pid> <serial_pid>"): the SAME binary shipped to
 *     /data/init.overlay by OTA. The bootstrap execs it (it becomes PID 1); it
 *     skips the early bring-up (already done) and runs the worker.
 * The worker (wifi bring-up + app launch/respawn/OTA/rollback) is the evolving
 * part and thus lives past the exec, so init updates ride the same /data-overlay
 * mechanism as the app — the boot partition is never rewritten at runtime, so a
 * bad init update can't brick: a crash (PID1 dies) panics+reboots, the bootstrap
 * runs again, and after INIT_TRIAL_MAX unconfirmed boots it quarantines the
 * overlay and runs the known-good built-in worker. */
#define INIT_OVERLAY   "/data/init.overlay"
#define INIT_OVERLAY_BAD "/data/init.overlay.bad"
#define INIT_TRIAL     "/data/nvx/init.trial"
#define INIT_TRIAL_MAX 3   /* unconfirmed overlay boots before rollback */
#define INIT_CONFIRM_SEC 30 /* app healthy this long -> commit the overlay */

static int trial_read(void) {
    int fd = open(INIT_TRIAL, O_RDONLY);
    if (fd < 0) return 0;
    char b[16] = {0};
    int n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return 0;
    return atoi(b);
}
static void trial_write(int v) {
    mkdir("/data/nvx", 0700);
    int fd = open(INIT_TRIAL, O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0600);
    if (fd < 0) return;
    char b[16];
    int n = snprintf(b, sizeof(b), "%d\n", v);
    ssize_t w = write(fd, b, n);
    (void)w;
    fsync(fd);
    close(fd);
}

/* Bootstrap only: if a healthy init overlay is installed, exec into it (never
 * returns on success). Bumps the trial counter first; the overlay resets it once
 * the app is healthy. Quarantines an overlay that has burned through its trials
 * (crash-loop) so we fall back to the built-in worker. `db`/`ser` are the pids of
 * the dropbear + serial-console children we already spawned, handed to the
 * overlay so it keeps respawning them across the exec. */
#ifdef MMK_DIAG
#include <linux/fb.h>
#include <sys/mman.h>
#include "font8x8_basic.h"

/* Append a truncated file's contents to buf under a header. */
static void di_file(char *buf, size_t cap, const char *label, const char *path, size_t maxlen) {
    size_t l = strlen(buf);
    snprintf(buf + l, cap - l, "== %s ==\n", label);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { strncat(buf, "  <absent>\n", cap - strlen(buf) - 1); return; }
    char t[2048]; ssize_t n = read(fd, t, sizeof(t) - 1); close(fd);
    if (n <= 0) { strncat(buf, "  <empty>\n", cap - strlen(buf) - 1); return; }
    if ((size_t)n > maxlen) n = maxlen;
    t[n] = 0;
    strncat(buf, t, cap - strlen(buf) - 1);
    if (strlen(buf) && buf[strlen(buf) - 1] != '\n') strncat(buf, "\n", cap - strlen(buf) - 1);
}

/* Append a directory's entry names (one line) under a header. */
static void di_dir(char *buf, size_t cap, const char *label, const char *path) {
    size_t l = strlen(buf);
    snprintf(buf + l, cap - l, "== %s ==\n  ", label);
    DIR *d = opendir(path);
    if (!d) { strncat(buf, "<absent>\n", cap - strlen(buf) - 1); return; }
    struct dirent *e; int c = 0;
    while ((e = readdir(d)) && c < 48) {
        if (e->d_name[0] == '.') continue;
        strncat(buf, e->d_name, cap - strlen(buf) - 1);
        strncat(buf, " ", cap - strlen(buf) - 1);
        c++;
    }
    closedir(d);
    strncat(buf, "\n", cap - strlen(buf) - 1);
}

/* insmod a module, return the child exit code (0 = loaded). */
static int di_insmod(char *buf, size_t cap, const char *path) {
    pid_t p = fork();
    if (p == 0) { char *a[] = {"/bin/busybox", "insmod", (char *)path, NULL}; execv(a[0], a); _exit(127); }
    int st; waitpid(p, &st, 0);
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    size_t l = strlen(buf);
    snprintf(buf + l, cap - l, "insmod %s -> %d\n", path, rc);
    return rc;
}

/* Append the TAIL of the kernel ring buffer (recent nand/probe/insmod msgs). */
static void di_kmsg(char *buf, size_t cap) {
    strncat(buf, "== dmesg (tail) ==\n", cap - strlen(buf) - 1);
    static char kb[18000];
    int n = (int)syscall(SYS_syslog, 3 /*READ_ALL*/, kb, (int)sizeof(kb) - 1);
    if (n <= 0) { strncat(buf, "  <klog fail>\n", cap - strlen(buf) - 1); return; }
    kb[n] = 0;
    const char *tail = (n > 1500) ? kb + n - 1500 : kb;
    strncat(buf, tail, cap - strlen(buf) - 1);
    if (strlen(buf) && buf[strlen(buf) - 1] != '\n') strncat(buf, "\n", cap - strlen(buf) - 1);
}

static void di_putc(uint16_t *fb, int stride, int x, int y, unsigned char ch, uint16_t col) {
    if (ch > 127) ch = '?';
    const char *g = font8x8_basic[ch];
    for (int r = 0; r < 8; r++)
        for (int b = 0; b < 8; b++)
            if (g[r] & (1 << b)) fb[(y + r) * stride + (x + b)] = col;
}

/* Paint the current kernel storage/net/gadget state to the panel, wait so it can
 * be photographed, then reboot into loader for the next flash (no button). */
static void mmk_fb_diag_and_reboot(void) {
    static char buf[16000]; buf[0] = 0;
    /* Load the NAND driver candidates, THEN read the resulting storage state --
     * this answers: does either driver create the /system,/data block devices? */
    di_insmod(buf, sizeof buf, "/lib/modules/drmboot.ko");
    di_insmod(buf, sizeof buf, "/lib/modules/rk30xxnand_ko.ko");
    di_file(buf, sizeof buf, "mtd", "/proc/mtd", 700);
    di_file(buf, sizeof buf, "partitions", "/proc/partitions", 1200);
    di_dir(buf, sizeof buf, "/sys/block", "/sys/block");
    di_dir(buf, sizeof buf, "/dev/block", "/dev/block");
    di_file(buf, sizeof buf, "mounts", "/proc/mounts", 800);
    di_file(buf, sizeof buf, "modules", "/proc/modules", 500);
    di_kmsg(buf, sizeof buf);

    int fd = open("/dev/fb0", O_RDWR);
    if (fd >= 0) {
        struct fb_var_screeninfo vi; struct fb_fix_screeninfo fi;
        if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) == 0 && ioctl(fd, FBIOGET_FSCREENINFO, &fi) == 0) {
            int stride = fi.line_length / 2, w = vi.xres, h = vi.yres;
            size_t sz = (size_t)fi.line_length * h;
            uint16_t *fb = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (fb != MAP_FAILED) {
                for (int i = 0; i < stride * h; i++) fb[i] = 0x0010; /* dark */
                int x = 2, y = 2;
                for (char *p = buf; *p; p++) {
                    if (*p == '\n') { x = 2; y += 9; continue; }
                    if (x + 8 > w) { x = 2; y += 9; }
                    if (y + 8 > h) break;
                    di_putc(fb, stride, x, y, (unsigned char)*p, 0xFFFF);
                    x += 8;
                }
                munmap(fb, sz);
            }
        }
        close(fd);
    }
    log_line("MMK_DIAG: painted fb; serial shell held 3min then reboot->loader");
    sleep(180);
    sync();
    syscall(SYS_reboot, 0xfee1dead, 672274793, 0xA1B2C3D4, "loader"); /* RESTART2 "loader" */
    syscall(SYS_reboot, 0xfee1dead, 672274793, 0x01234567, (void *)0); /* fallback: plain restart */
    for (;;) pause();
}
#endif

static void maybe_exec_overlay(pid_t db, pid_t ser) {
    if (access(INIT_OVERLAY, X_OK) != 0) return; /* no overlay -> built-in */
    /* Guarantee that a crashing overlay (PID1 death -> panic) AUTO-REBOOTS, so
     * the trial counter can quarantine it without a manual power-cycle. Don't
     * rely on the kernel's compiled panic-timeout default (this one is 1s, but
     * be explicit). */
    int pf = open("/proc/sys/kernel/panic", O_WRONLY);
    if (pf >= 0) { ssize_t w = write(pf, "3\n", 2); (void)w; close(pf); }
    int trials = trial_read();
    if (trials >= INIT_TRIAL_MAX) {
        rename(INIT_OVERLAY, INIT_OVERLAY_BAD); /* quarantine the crash-looper */
        trial_write(0);
        log_line("init overlay crash-looped -> quarantined, using built-in worker");
        return;
    }
    trial_write(trials + 1);
    char m[160], dbs[16], sers[16];
    snprintf(dbs, sizeof(dbs), "%d", (int)db);
    snprintf(sers, sizeof(sers), "%d", (int)ser);
    snprintf(m, sizeof(m), "exec init overlay (trial %d/%d): %s", trials + 1, INIT_TRIAL_MAX, INIT_OVERLAY);
    log_line(m);
    char *ov[] = {(char *)INIT_OVERLAY, "overlay", dbs, sers, NULL};
    execv(ov[0], ov);
    log_line("init overlay execv failed -> built-in worker"); /* falls through */
}

/* insmod that cannot wedge PID-1: run it in a child and stop waiting after
 * `secs`. A wrong-chip driver probing an absent SDIO card can block in-kernel;
 * the old run()+waitpid hung here so the app never launched (looked like a
 * freeze). Logged before AND after, so the durable trail shows if this was the
 * last thing that happened. */
static int insmod_timeboxed(const char *path, int secs) {
    char m[200];
    snprintf(m, sizeof(m), "insmod (timebox %ds): %s", secs, path);
    log_line(m);
    char *argv[] = {"/bin/busybox", "insmod", (char *)path, NULL};
    pid_t pid = fork();
    if (pid == 0) {
        execv(argv[0], argv);
        _exit(127);
    }
    if (pid < 0) {
        log_line("insmod: fork failed");
        return -1;
    }
    for (int i = 0; i < secs * 10; i++) {
        int st;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            snprintf(m, sizeof(m), "insmod done (exit %d): %s",
                     WIFEXITED(st) ? WEXITSTATUS(st) : -1, path);
            log_line(m);
            return 0;
        }
        usleep(100 * 1000);
    }
    snprintf(m, sizeof(m), "insmod WEDGED (>%ds) -> abandon child, continue: %s", secs, path);
    log_line(m);
    kill(pid, SIGKILL); /* frees our reap slot; won't unblock an in-kernel wait */
    return -1;
}

int main(int argc, char **argv) {
    /* Role: bootstrap (no arg, PID1 from boot.img) vs OTA overlay ("overlay").
     * dropbear + serial are shared across the exec so both roles respawn them. */
    int is_overlay = (argc > 1 && !strcmp(argv[1], "overlay"));
    // -s: NO password authentication, ever. Today root has no /etc/shadow entry so
    // password logins already fail -- but that is an accident of packaging, not a
    // decision, and it would silently stop being true the moment a shadow file
    // appeared. Key auth only; a shipped image has no authorized_keys at all
    // (dev builds stage one in via MMK_DEV=1).
    char *dropbear[] = {"/usr/sbin/dropbear", "-R", "-E", "-s", "-p", "22", NULL};
    pid_t dropbear_pid = 0, serial_pid = 0;

    if (is_overlay) {
        /* The bootstrap already did /dev,/data,/system + network + dropbear +
         * serial, then exec'd us; adopt those children's pids and mark /data
         * mounted so durable logging works. */
        data_mounted = 1;
        if (argc > 2) dropbear_pid = (pid_t)atoi(argv[2]);
        if (argc > 3) serial_pid = (pid_t)atoi(argv[3]);
        log_line("init overlay running (handed off from bootstrap)");
    } else {
    /* Create the mount points FIRST. These are empty dirs; git can't track empty
     * dirs, so the ramdisk doesn't ship /proc /sys /dev -- and without a mount
     * point mount() fails silently, so /proc/mtd (nand mount) and /sys (wifi,
     * gadget) all read empty and the whole boot cascades to nothing. This
     * missing mkdir was the root cause of every "nothing mounts" symptom. */
    mkdir("/proc", 0555);
    mkdir("/sys", 0555);
    mkdir("/dev", 0755);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) log_line("MOUNT /proc FAILED");
    if (mount("sysfs", "/sys", "sysfs", 0, NULL) != 0) log_line("MOUNT /sys FAILED");

    /* /dev: this kernel has no working devtmpfs, so tmpfs + manual nodes. */
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) != 0)
        mount("tmpfs", "/dev", "tmpfs", 0, "mode=0755");
    make_node("/dev/console", S_IFCHR | 0600, 5, 1);
    make_node("/dev/null", S_IFCHR | 0666, 1, 3);
    make_node("/dev/zero", S_IFCHR | 0666, 1, 5);
    make_node("/dev/full", S_IFCHR | 0666, 1, 7);
    make_node("/dev/random", S_IFCHR | 0666, 1, 8);
    make_node("/dev/urandom", S_IFCHR | 0666, 1, 9);
    make_node("/dev/tty", S_IFCHR | 0666, 5, 0);
    make_node("/dev/kmsg", S_IFCHR | 0644, 1, 11);
    mkdir("/dev/pts", 0755);
    make_node("/dev/ptmx", S_IFCHR | 0666, 5, 2);
    mount("devpts", "/dev/pts", "devpts", 0, NULL);

    /* Scratch dirs that lots of userspace assumes exist (git can't track empty
     * dirs, so they aren't in the ramdisk). /tmp gets the standard sticky 1777;
     * /var/run is where the app parks the wpa_supplicant control socket. */
    mkdir("/tmp", 01777);
    mkdir("/var", 0755);
    mkdir("/var/run", 0755);

    log_line("alive: /proc /sys /dev ready (tmpfs+mknod)");

    /* Networking + SSH come up FIRST, before any nand/mount work -- these
     * only need /dev (urandom etc) + the ramdisk-resident config, none of
     * which can hang. So even if the NAND/partition steps below hang or
     * crash, we still get a live SSH shell to debug from. (Earlier boots put
     * the /data mount first; a hang there killed all observability.) */
    bring_up_network();
    /* dropbear -R generates host keys on demand, but it WRITES them into
     * /etc/dropbear/ -- if that dir is missing, keygen fails and dropbear drops
     * every connection right at the host-key exchange (looks like ":22 closed").
     * git can't track the empty dir, so create it here (same reason we mkdir
     * /proc,/sys,/dev above). */
    mkdir("/etc/dropbear", 0700);
    dropbear_pid = spawn(dropbear);
    log_line("network + dropbear(:22) started -- device should be SSH-reachable");

    /* Root shell on the USB serial port (Mac-native reachability when there's no
     * PoE/WiFi). Respawned in the reap loop below when the host disconnects. */
    serial_pid = spawn_serial_console();

    /* Kick the NAND probe. rk29xxnand is compiled into the kernel, but on this
     * hardware its built-in probe does NOT create the MTD partitions on its own
     * -- loading rk30xxnand_ko is what actually triggers the partition scan.
     * (The insmod logs a harmless -EEXIST for the duplicate rknand_sys_storage
     * sysfs node, but the mtd partition creation still runs.) EMPIRICAL: with
     * this insmod + a working busybox, /proc/mtd fills in with all 11 parts and
     * /system + /data mount; WITHOUT it, /proc/mtd stays empty and nothing
     * mounts. So this is required, not redundant. */
    insmod("/lib/modules/rk30xxnand_ko.ko");
    log_line("rk30xxnand_ko insmod attempted (kicks NAND partition scan)");

    /* The scan is ASYNC: /proc/mtd and the /dev/mtdblockN nodes appear ~1-2s
     * after the insmod. Mounting before then silently fails (empty /proc/mtd ->
     * partition "not found"). Poll up to ~10s for the 'system' partition. */
    {
        int ready = 0;
        for (int i = 0; i < 100; i++) {
            int fd = open("/proc/mtd", O_RDONLY);
            if (fd >= 0) {
                char b[1024];
                int n = read(fd, b, sizeof(b) - 1);
                close(fd);
                if (n > 0) {
                    b[n] = 0;
                    if (strstr(b, "\"system\"")) { ready = 1; break; }
                }
            }
            usleep(100 * 1000);
        }
        log_line(ready ? "mtd partitions ready" : "mtd partitions TIMEOUT (mounting anyway)");
    }

    /* Mount /data (also makes logging durable). Non-fatal. */
    mkdir("/data", 0771);
    if (mount_mtd("userdata", "/data", 0) == 0) {
        data_mounted = 1;
        unlink(BOOTLOG);        /* fresh log each boot */
        log_flush_backlog();    /* everything logged before now -> durable, once */
        log_line("=== mmkeypad custom init boot log (backlog flushed above) ===");
        log_line("/data mounted, durable O_SYNC logging active");
    }

    /* Remaining partitions (non-fatal). */
    mkdir("/system", 0755);
    mkdir("/cache", 0770);
    mkdir("/metadata", 0500);
    mount_mtd("system", "/system", MS_RDONLY);
    mount_mtd("cache", "/cache", 0);
    mount_mtd("metadata", "/metadata", 0);
    log_line("all partitions attempted, bring-up complete");

    /* Vendor graphics/media modules from the stock /system. /system ships a
     * per-kernel build of each (mali.ko for 3.0.8+, mali.ko.3.0.36+ for the
     * 4.0.0 kernel we run) -- insmod_kver() picks the one whose vermagic matches
     * `uname -r`, exactly like stock. Load order matters: ump before mali (mali
     * depends on ump). rk29-ipp is REQUIRED for camera capture -- the CIF
     * driver's frame copy is an IPP blit that silently no-ops (buffer never
     * written) without it. vpu_service enables HW H.264 decode (door-camera
     * video). All non-fatal: a wrong/absent kernel just fails the insmod. */
    insmod_kver("/system/lib/modules/ump.ko", NULL);
    /* No mali_dvfs/mali_init_clock params: the .3.0.36+ mali build rejects the
     * old comma-list dvfs format ("mali: `50' invalid for parameter mali_dvfs")
     * and won't load with it. It loads fine on default clocks without them. */
    insmod_kver("/system/lib/modules/mali.ko", NULL);
    insmod_kver("/system/lib/modules/rk29-ipp.ko", NULL);
    /* vpu_service.ko is DELIBERATELY not auto-loaded: it panicked the kernel at
     * boot (no .3.0.36+ variant ships, and the 3.0.8+ build faults on this
     * kernel), taking down USB + network mid-boot. It's only needed for HW
     * H.264 decode (door-camera video, a later feature), not the UI or camera
     * capture -- revisit once a matching build exists or it's proven safe. */
    log_line("vendor modules (ump/mali/rk29-ipp) kver-matched insmod attempted");

#ifdef MMK_DIAG
    /* Debug build: paint kernel storage/net/gadget state to the panel + return
     * to loader. Never returns. Build with EXTRA_CFLAGS=-DMMK_DIAG. */
    mmk_fb_diag_and_reboot();
#endif

    /* Bootstrap: hand off to the OTA'd init overlay if installed + healthy — this
     * execs and never returns on success. Otherwise fall through to run the
     * built-in (known-good) worker ourselves. */
    maybe_exec_overlay(dropbear_pid, serial_pid);
    } /* end bootstrap-only bring-up; is_overlay skips straight to the worker */

    /* WiFi. The driver + firmware + per-variant NVRAM live on the stock /system,
     * so load after it mounts. We DETECT the chip and load the matching driver
     * (C4 may have shipped Broadcom/Realtek/MTK/ESP across runs) rather than
     * hardcoding one, and do it timeboxed in a child so a wrong-chip probe can't
     * wedge PID-1 and stall the app launch. For Broadcom the selected module is
     * the OUT-OF-BAND-IRQ build (rkwifi.oob.ko): the in-band rkwifi.ko registers
     * wlan0 but the firmware never answers (SDIO rxctl timeout); the OOB variant
     * brings the bus alive. This only registers wlan0; association is
     * wpa_supplicant's job. Everything below lands in the durable log for the
     * post-mortem read over the loader-USB link. */
    log_sdio_devices();
    /* Same kernel-matched selection as the vendor blobs above: if /system ships
     * a "<name>.<uname -r>" build of the detected wifi module, use it. On this
     * unit rkwifi.oob.ko has no variant (its single build already matches), so
     * this falls back to the plain path -- but Realtek/ESP parts or a future
     * /system may ship one, and matching keeps us from a vermagic reject. */
    char wifi_kbuf[256];
    const char *wifi_ko = kver_module(wifi_module(), wifi_kbuf, sizeof wifi_kbuf);
    {
        char m[160];
        snprintf(m, sizeof(m), "wifi: selected module %s", wifi_ko);
        log_line(m);
    }
    insmod_timeboxed(wifi_ko, 8);
    log_file("wifi.ifaces.after", "/proc/net/dev");

    /* GPU/2D/VPU vendor modules are loaded in the bootstrap bring-up above
     * (ump/mali/rk29-ipp/vpu_service), once /system is mounted. */

    /* Launch the LVGL app (the whole point). Spawned, not exec'd, so init
     * stays PID 1 to reap dropbear/dhcp and respawn the app if it crashes.
     *
     * OTA overlay: prefer the persistent /data/mmkeypad build (installed by the
     * app-overlay OTA) over the factory binary baked into this initramfs. If the
     * overlay crash-loops (dies faster than OTA_MIN_UPTIME, OTA_MAX_FAILS times
     * in a row) roll it back so the unit always recovers to a known-good build. */
#define APP_OVERLAY "/data/mmkeypad"
#define APP_FACTORY "/usr/bin/mmkeypad"
#define APP_LOG     "/data/mmkeypad.log"
#define OTA_MIN_UPTIME 20 /* seconds a healthy run lasts at least */
#define OTA_MAX_FAILS 3
    char *app_overlay[] = {APP_OVERLAY, NULL};
    char *app_factory[] = {APP_FACTORY, NULL};
    int use_overlay = (access(APP_OVERLAY, X_OK) == 0);
    char **app = use_overlay ? app_overlay : app_factory;
    int ota_fails = 0;
    time_t app_started = time(NULL);
    pid_t lvgl_pid = spawn_logged(app, APP_LOG);
    log_line(use_overlay ? "mmkeypad launched (OTA overlay) -> /data/mmkeypad.log"
                         : "mmkeypad launched (factory) -> /data/mmkeypad.log");

    /* Overlay-only: a one-shot timer. Reaping it (below) means the init overlay
     * reached this worker loop and survived INIT_CONFIRM_SEC -> commit it (reset
     * the trial counter) so the bootstrap won't roll it back next boot. */
    pid_t confirm_pid = -1;
    if (is_overlay) {
        confirm_pid = fork();
        if (confirm_pid == 0) { sleep(INIT_CONFIRM_SEC); _exit(0); }
    }

    for (;;) {
        int status;
        pid_t died = wait(&status);
        if (died == dropbear_pid) {
            dropbear_pid = spawn(dropbear);
        } else if (serial_pid > 0 && died == serial_pid) {
            /* host closed the serial port -> offer a fresh shell for next open */
            serial_pid = spawn_serial_console();
        } else if (confirm_pid > 0 && died == confirm_pid) {
            trial_write(0); /* init overlay proven healthy -> committed */
            log_line("init overlay confirmed healthy -> committed");
            confirm_pid = -1;
        } else if (died == lvgl_pid) {
            if (use_overlay && (time(NULL) - app_started) < OTA_MIN_UPTIME) {
                if (++ota_fails >= OTA_MAX_FAILS) {
                    rename(APP_OVERLAY, "/data/mmkeypad.bad"); /* quarantine */
                    log_line("OTA overlay crash-looped -> rolled back to factory");
                    use_overlay = 0;
                    app = app_factory;
                    ota_fails = 0;
                }
            } else {
                ota_fails = 0; /* a healthy run clears the strike count */
            }
            app_started = time(NULL);
            lvgl_pid = spawn_logged(app, APP_LOG); /* respawn the UI if it exits/crashes */
        }
        if (died < 0)
            sleep(1);
    }
}
