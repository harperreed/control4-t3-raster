/* ABOUTME: Hardware capability discovery and live power/network/backlight state from sysfs.
 * ABOUTME: Nothing is assumed present: a missing sysfs entry becomes available:false or null. */
#include "sysinfo.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_NAMES 32
#define NAME_LEN 64

struct names {
    int n;
    char v[MAX_NAMES][NAME_LEN];
};

static int cmp_names(const void *a, const void *b) { return strcmp(a, b); }

/* Sorted entries of <root>/<rel>, without dot files. Empty if absent. */
static void list_dir(const char *root, const char *rel, struct names *out) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    out->n = 0;
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && out->n < MAX_NAMES) {
        size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.' || len >= NAME_LEN) continue;
        memcpy(out->v[out->n++], e->d_name, len + 1);
    }
    closedir(d);
    qsort(out->v, (size_t)out->n, NAME_LEN, cmp_names);
}

/* Read <root>/<dir>/<name>/<attr>, trailing whitespace stripped. 0 or -1. */
static int read_attr(const char *root, const char *dir, const char *name, const char *attr, char *buf, size_t n) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s/%s/%s", root, dir, name, attr);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t r = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[r] = 0;
    while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' ' || buf[r - 1] == '\r')) buf[--r] = 0;
    return 0;
}

static long read_long(const char *root, const char *dir, const char *name, const char *attr, long fallback) {
    char buf[64], *end;
    if (read_attr(root, dir, name, attr, buf, sizeof buf) != 0 || !buf[0]) return fallback;
    long v = strtol(buf, &end, 10);
    return *end ? fallback : v;
}

static int has_entry(const struct names *list, const char *name) {
    for (int i = 0; i < list->n; i++)
        if (strcmp(list->v[i], name) == 0) return 1;
    return 0;
}

int modalias_has(const char *modalias, char sec, unsigned code) {
    const char *p = strchr(modalias, '-');
    if (!p) return 0;
    char cur = 0;
    for (p++; *p;) {
        if (*p >= 'a' && *p <= 'z') { /* section letter; codes are uppercase hex */
            cur = *p++;
            continue;
        }
        if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F')) {
            char *end;
            unsigned long v = strtoul(p, &end, 16);
            if (cur == sec && v == code) return 1;
            p = end;
            continue;
        }
        p++; /* the ',' after each code */
    }
    return 0;
}

static void json_names(struct sbuf *sb, const struct names *list, int (*keep)(const char *)) {
    sb_puts(sb, "[");
    int first = 1;
    for (int i = 0; i < list->n; i++) {
        if (keep && !keep(list->v[i])) continue;
        if (!first) sb_puts(sb, ",");
        sb_json_str(sb, list->v[i]);
        first = 0;
    }
    sb_puts(sb, "]");
}

/* pcmC<card>D<dev>p is a playback device, ...c a capture device. */
static int pcm_with(const char *name, char dir) {
    size_t n = strlen(name);
    return strncmp(name, "pcmC", 4) == 0 && n > 5 && name[n - 1] == dir;
}
static int is_playback(const char *name) { return pcm_with(name, 'p'); }
static int is_capture(const char *name) { return pcm_with(name, 'c'); }
static int count_kept(const struct names *list, int (*keep)(const char *)) {
    int n = 0;
    for (int i = 0; i < list->n; i++) n += keep(list->v[i]);
    return n;
}

/* First power supply of a type ("Battery", "Mains"); "" if none. A battery
 * whose `present` reads 0 does not count. */
static void find_supply(const char *root, const char *type, char *out) {
    struct names ps;
    list_dir(root, "class/power_supply", &ps);
    out[0] = 0;
    for (int i = 0; i < ps.n; i++) {
        char t[32];
        if (read_attr(root, "class/power_supply", ps.v[i], "type", t, sizeof t) != 0 || strcmp(t, type) != 0) continue;
        if (read_long(root, "class/power_supply", ps.v[i], "present", 1) == 0) continue;
        snprintf(out, NAME_LEN, "%s", ps.v[i]);
        return;
    }
}

static void json_device(struct sbuf *sb, const char *key, const char *field, const char *name) {
    sb_printf(sb, "\"%s\":{\"available\":%s,\"%s\":", key, name[0] ? "true" : "false", field);
    sb_json_str(sb, name[0] ? name : NULL);
    sb_puts(sb, "}");
}

void sysinfo_capabilities(struct sbuf *sb, const char *root) {
    /* Input: touch = the Silead controller by name, or anything with ABS_MT_POSITION_X
     * (0x35); buttons = key devices without absolute axes (EV_KEY 1, EV_ABS 3). */
    struct names inputs, buttons = {0};
    char touch[NAME_LEN] = "";
    list_dir(root, "class/input", &inputs);
    for (int i = 0; i < inputs.n; i++) {
        char name[NAME_LEN], mod[512];
        if (strncmp(inputs.v[i], "input", 5) != 0) continue;
        if (read_attr(root, "class/input", inputs.v[i], "name", name, sizeof name) != 0) continue;
        if (read_attr(root, "class/input", inputs.v[i], "modalias", mod, sizeof mod) != 0) mod[0] = 0;
        if (!touch[0] && (strstr(name, "gslX680") || modalias_has(mod, 'a', 0x35)))
            snprintf(touch, sizeof touch, "%s", name);
        else if (modalias_has(mod, 'e', 1) && !modalias_has(mod, 'e', 3) && buttons.n < MAX_NAMES)
            snprintf(buttons.v[buttons.n++], NAME_LEN, "%s", name);
    }
    json_device(sb, "touch", "device", touch);
    sb_printf(sb, ",\"buttons\":{\"available\":%s,\"devices\":", buttons.n ? "true" : "false");
    json_names(sb, &buttons, NULL);
    sb_puts(sb, "}");

    struct names bl;
    list_dir(root, "class/backlight", &bl);
    if (bl.n) {
        long max = read_long(root, "class/backlight", bl.v[0], "max_brightness", -1);
        sb_puts(sb, ",\"backlight\":{\"available\":true,\"device\":");
        sb_json_str(sb, bl.v[0]);
        if (max >= 0) sb_printf(sb, ",\"max_brightness\":%ld}", max);
        else sb_puts(sb, ",\"max_brightness\":null}");
    } else {
        sb_puts(sb, ",\"backlight\":{\"available\":false,\"device\":null,\"max_brightness\":null}");
    }

    char battery[NAME_LEN], mains[NAME_LEN];
    find_supply(root, "Battery", battery);
    find_supply(root, "Mains", mains);
    sb_puts(sb, ",");
    json_device(sb, "battery", "device", battery);
    sb_puts(sb, ",");
    json_device(sb, "external_power", "device", mains);
    /* Whether the Mains supply means "docked" has not been checked on the panel. */
    sb_puts(sb, ",\"dock_detection\":{\"available\":null}");

    struct names net;
    list_dir(root, "class/net", &net);
    sb_puts(sb, ",");
    json_device(sb, "wifi", "interface", has_entry(&net, "wlan0") ? "wlan0" : "");
    sb_puts(sb, ",");
    json_device(sb, "ethernet", "interface", has_entry(&net, "eth0") ? "eth0" : "");
    sb_puts(sb, ",");
    json_device(sb, "usb_network", "interface",
                has_entry(&net, "rndis0") ? "rndis0" : has_entry(&net, "usb0") ? "usb0" : "");

    struct names snd;
    list_dir(root, "class/sound", &snd);
    sb_printf(sb, ",\"audio_output\":{\"available\":%s,\"devices\":", count_kept(&snd, is_playback) ? "true" : "false");
    json_names(sb, &snd, is_playback);
    sb_printf(sb, "},\"audio_input\":{\"available\":%s,\"devices\":", count_kept(&snd, is_capture) ? "true" : "false");
    json_names(sb, &snd, is_capture);
    sb_puts(sb, "}");

    /* A video4linux node exists on the TT7, but whether it is the camera is
     * unverified: report the nodes and leave "available" unknown. */
    struct names v4l;
    list_dir(root, "class/video4linux", &v4l);
    sb_printf(sb, ",\"camera\":{\"available\":%s,\"video4linux_devices\":", v4l.n ? "null" : "false");
    json_names(sb, &v4l, NULL);
    sb_puts(sb, "}");
}

void sysinfo_power(struct sbuf *sb, const char *root) {
    char battery[NAME_LEN], mains[NAME_LEN], status[32] = "";
    find_supply(root, "Battery", battery);
    find_supply(root, "Mains", mains);
    long online = mains[0] ? read_long(root, "class/power_supply", mains, "online", -1) : -1;
    long pct = battery[0] ? read_long(root, "class/power_supply", battery, "capacity", -1) : -1;
    long uv = battery[0] ? read_long(root, "class/power_supply", battery, "voltage_now", -1) : -1;
    int have_status = battery[0] && read_attr(root, "class/power_supply", battery, "status", status, sizeof status) == 0;

    const char *source = NULL;
    if (online == 1) source = "external";
    else if (online == 0 && battery[0]) source = "battery";
    sb_puts(sb, "\"power\":{\"source\":");
    sb_json_str(sb, source);

    /* The 3.0 kernel's gauge jumps between boots (gotchas.md): an estimate. */
    if (pct >= 0) sb_printf(sb, ",\"battery_percent\":{\"value\":%ld,\"unit\":\"percent\",\"available\":true,\"estimate\":true}", pct);
    else sb_puts(sb, ",\"battery_percent\":{\"value\":null,\"unit\":\"percent\",\"available\":false,\"estimate\":true}");
    if (uv >= 0) sb_printf(sb, ",\"battery_voltage\":{\"value\":%ld.%03ld,\"unit\":\"volt\",\"available\":true}", uv / 1000000, uv / 1000 % 1000);
    else sb_puts(sb, ",\"battery_voltage\":{\"value\":null,\"unit\":\"volt\",\"available\":false}");

    const char *charging = "null";
    if (strcmp(status, "Charging") == 0) charging = "true";
    else if (!strcmp(status, "Discharging") || !strcmp(status, "Not charging") || !strcmp(status, "Full")) charging = "false";
    sb_printf(sb, ",\"charging\":%s,\"battery_status\":", charging);
    sb_json_str(sb, have_status ? status : NULL);
    sb_printf(sb, ",\"external_power_online\":%s}", online == 1 ? "true" : online == 0 ? "false" : "null");
}

/* The interface's IPv4 address from the kernel, or "" if it has none. */
static void ipv4_of(const char *ifname, char *out, size_t n) {
    out[0] = 0;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifname);
    if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
        struct sockaddr_in sin;
        memcpy(&sin, &ifr.ifr_addr, sizeof sin);
        inet_ntop(AF_INET, &sin.sin_addr, out, (socklen_t)n);
    }
    close(s);
}

void sysinfo_network(struct sbuf *sb, const char *root) {
    struct names net;
    list_dir(root, "class/net", &net);
    sb_puts(sb, "\"network\":{\"interfaces\":{");
    int first = 1;
    for (int i = 0; i < net.n; i++) {
        if (strcmp(net.v[i], "lo") == 0) continue;
        char oper[32], ip[INET_ADDRSTRLEN];
        int have_oper = read_attr(root, "class/net", net.v[i], "operstate", oper, sizeof oper) == 0;
        ipv4_of(net.v[i], ip, sizeof ip);
        if (!first) sb_puts(sb, ",");
        first = 0;
        sb_json_str(sb, net.v[i]);
        sb_puts(sb, ":{\"operstate\":");
        sb_json_str(sb, have_oper ? oper : NULL);
        sb_puts(sb, ",\"ipv4\":");
        sb_json_str(sb, ip[0] ? ip : NULL);
        sb_puts(sb, "}");
    }
    sb_puts(sb, "}}");
}

void sysinfo_backlight(const char *root, int *on, int *percent) {
    struct names bl;
    list_dir(root, "class/backlight", &bl);
    *on = -1;
    *percent = -1;
    if (!bl.n) return;
    long b = read_long(root, "class/backlight", bl.v[0], "brightness", -1);
    long max = read_long(root, "class/backlight", bl.v[0], "max_brightness", -1);
    long power = read_long(root, "class/backlight", bl.v[0], "bl_power", 0); /* 0 = FB_BLANK_UNBLANK */
    if (b >= 0) *on = (b > 0 && power == 0);
    if (b >= 0 && max > 0) *percent = (int)((b * 100 + max / 2) / max);
}
