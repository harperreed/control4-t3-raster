/* ABOUTME: Builds the /api/v1/hardware members from sysfs classes and /proc/asound/cards.
 * ABOUTME: Reads only allowlisted attributes: some vendor sysfs files have side effects when read. */
#include "hardware.h"

#include <stdio.h>
#include <string.h>

#include "sysinfo.h"

/* Attributes read per class. Never read a whole directory: the stock
 * kernel's USB OTG node has an attribute whose read writes a register
 * (gotchas.md), and other vendor drivers may do the same. */
static const char *const power_attrs[] = {"type", "online", "present", "status", "capacity", "voltage_now",
                                          "current_now", "health", "technology", "temp", NULL};
static const char *const backlight_attrs[] = {"brightness", "max_brightness", "actual_brightness", "bl_power",
                                              "type", NULL};
static const char *const thermal_attrs[] = {"type", "temp", NULL};

/* Key codes from linux/input-event-codes.h that a panel button may send. */
static const struct {
    unsigned code;
    const char *name;
} key_names[] = {
    {0x66, "home"}, {0x71, "mute"}, {0x72, "volume_down"}, {0x73, "volume_up"}, {0x74, "power"},
    {0x8B, "menu"}, {0x8F, "wakeup"}, {0x9E, "back"},
};

/* "attributes":{"a":"value"|null,...} for one sysfs device. */
static void json_attrs(struct sbuf *sb, const char *root, const char *dir, const char *name,
                       const char *const *attrs) {
    sb_puts(sb, "\"attributes\":{");
    for (int i = 0; attrs[i]; i++) {
        char v[256];
        sb_printf(sb, "%s\"%s\":", i ? "," : "", attrs[i]);
        sb_json_str(sb, read_attr(root, dir, name, attrs[i], v, sizeof v) == 0 ? v : NULL);
    }
    sb_puts(sb, "}");
}

static void input_members(struct sbuf *sb, const char *root) {
    struct names inputs;
    list_dir(root, "class/input", &inputs);
    sb_puts(sb, "\"input\":[");
    int first = 1;
    for (int i = 0; i < inputs.n; i++) {
        const char *dev = inputs.v[i];
        char name[NAME_LEN], mod[512], rel[128];
        if (strncmp(dev, "input", 5) != 0) continue;
        int have_name = read_attr(root, "class/input", dev, "name", name, sizeof name) == 0;
        int have_mod = read_attr(root, "class/input", dev, "modalias", mod, sizeof mod) == 0;
        if (!have_name) name[0] = 0;
        if (!have_mod) mod[0] = 0;

        /* The evdev node is the inputN/eventM child directory. */
        struct names children;
        snprintf(rel, sizeof rel, "class/input/%s", dev);
        list_dir(root, rel, &children);
        const char *event = NULL;
        for (int k = 0; k < children.n && !event; k++)
            if (!strncmp(children.v[k], "event", 5)) event = children.v[k];

        sb_puts(sb, first ? "{\"sysfs\":" : ",{\"sysfs\":");
        first = 0;
        sb_json_str(sb, dev);
        sb_puts(sb, ",\"device\":");
        char node[NAME_LEN + 16];
        snprintf(node, sizeof node, "/dev/input/%s", event ? event : "");
        sb_json_str(sb, event ? node : NULL);
        sb_puts(sb, ",\"name\":");
        sb_json_str(sb, have_name ? name : NULL);
        sb_puts(sb, ",\"role\":");
        sb_json_str(sb, sysinfo_input_role(name, mod));
        sb_puts(sb, ",\"keys\":[");
        int nkeys = 0;
        for (size_t k = 0; k < sizeof key_names / sizeof key_names[0]; k++)
            if (modalias_has(mod, 'k', key_names[k].code)) sb_printf(sb, "%s\"%s\"", nkeys++ ? "," : "", key_names[k].name);
        sb_puts(sb, "],\"modalias\":");
        sb_json_str(sb, have_mod ? mod : NULL);
        sb_puts(sb, "}");
    }
    sb_puts(sb, "]");
}

/* A list of {"name": ..., "attributes": {...}} for every entry of a class
 * whose name starts with prefix. */
static void class_list(struct sbuf *sb, const char *root, const char *cls, const char *prefix,
                       const char *const *attrs) {
    struct names list;
    list_dir(root, cls, &list);
    sb_puts(sb, "[");
    int first = 1;
    for (int i = 0; i < list.n; i++) {
        if (strncmp(list.v[i], prefix, strlen(prefix)) != 0) continue;
        sb_puts(sb, first ? "{\"name\":" : ",{\"name\":");
        first = 0;
        sb_json_str(sb, list.v[i]);
        sb_puts(sb, ",");
        json_attrs(sb, root, cls, list.v[i], attrs);
        sb_puts(sb, "}");
    }
    sb_puts(sb, "]");
}

static void network_members(struct sbuf *sb, const char *root) {
    struct names net;
    list_dir(root, "class/net", &net);
    sb_puts(sb, "\"network_interfaces\":[");
    int first = 1;
    for (int i = 0; i < net.n; i++) {
        if (!strcmp(net.v[i], "lo")) continue;
        char oper[32], mac[32], ip[16];
        int have_oper = read_attr(root, "class/net", net.v[i], "operstate", oper, sizeof oper) == 0;
        int have_mac = read_attr(root, "class/net", net.v[i], "address", mac, sizeof mac) == 0;
        ipv4_of(net.v[i], ip, sizeof ip);
        sb_puts(sb, first ? "{\"name\":" : ",{\"name\":");
        first = 0;
        sb_json_str(sb, net.v[i]);
        sb_puts(sb, ",\"operstate\":");
        sb_json_str(sb, have_oper ? oper : NULL);
        sb_puts(sb, ",\"mac\":");
        sb_json_str(sb, have_mac ? mac : NULL);
        sb_puts(sb, ",\"ipv4\":");
        sb_json_str(sb, ip[0] ? ip : NULL);
        sb_puts(sb, "}");
    }
    sb_puts(sb, "]");
}

int asound_card_parse(const char *line, int *index, char *id, size_t idlen, char *name, size_t namelen) {
    char idbuf[64], namebuf[256];
    if (sscanf(line, " %d [%63[^]]]: %255[^\n]", index, idbuf, namebuf) != 3) return -1;
    size_t n = strlen(idbuf);
    while (n > 0 && idbuf[n - 1] == ' ') idbuf[--n] = 0;
    snprintf(id, idlen, "%s", idbuf);
    snprintf(name, namelen, "%s", namebuf);
    return 0;
}

static void audio_members(struct sbuf *sb, const char *root, const char *proc_root) {
    char path[512], line[512];
    snprintf(path, sizeof path, "%s/asound/cards", proc_root);
    sb_puts(sb, "\"audio\":{\"cards\":");
    FILE *f = fopen(path, "r");
    if (f) {
        sb_puts(sb, "[");
        int first = 1, idx;
        char id[64], name[256];
        while (fgets(line, sizeof line, f)) {
            if (asound_card_parse(line, &idx, id, sizeof id, name, sizeof name) != 0) continue;
            sb_printf(sb, "%s{\"index\":%d,\"id\":", first ? "" : ",", idx);
            first = 0;
            sb_json_str(sb, id);
            sb_puts(sb, ",\"name\":");
            sb_json_str(sb, name);
            sb_puts(sb, "}");
        }
        fclose(f);
        sb_puts(sb, "]");
    } else {
        sb_puts(sb, "null");
    }
    struct names snd;
    list_dir(root, "class/sound", &snd);
    sb_puts(sb, ",\"devices\":[");
    for (int i = 0; i < snd.n; i++) {
        if (i) sb_puts(sb, ",");
        sb_json_str(sb, snd.v[i]);
    }
    sb_puts(sb, "]}");
}

static void video_members(struct sbuf *sb, const char *root) {
    struct names v4l;
    list_dir(root, "class/video4linux", &v4l);
    sb_puts(sb, "\"video_devices\":[");
    for (int i = 0; i < v4l.n; i++) {
        char name[128];
        int have = read_attr(root, "class/video4linux", v4l.v[i], "name", name, sizeof name) == 0;
        char dev[NAME_LEN + 8];
        snprintf(dev, sizeof dev, "/dev/%s", v4l.v[i]);
        sb_puts(sb, i ? ",{\"device\":" : "{\"device\":");
        sb_json_str(sb, dev);
        sb_puts(sb, ",\"name\":");
        sb_json_str(sb, have ? name : NULL);
        sb_puts(sb, "}");
    }
    sb_puts(sb, "]");
}

void hardware_members(struct sbuf *sb, const char *root, const char *proc_root) {
    input_members(sb, root);

    struct names bl;
    list_dir(root, "class/backlight", &bl);
    sb_puts(sb, ",\"backlight\":");
    if (bl.n) {
        sb_puts(sb, "{\"device\":");
        sb_json_str(sb, bl.v[0]);
        sb_puts(sb, ",");
        json_attrs(sb, root, "class/backlight", bl.v[0], backlight_attrs);
        sb_puts(sb, "}");
    } else {
        sb_puts(sb, "null");
    }

    sb_puts(sb, ",\"power_supplies\":");
    class_list(sb, root, "class/power_supply", "", power_attrs);
    sb_puts(sb, ",\"thermal_zones\":");
    class_list(sb, root, "class/thermal", "thermal_zone", thermal_attrs);
    sb_puts(sb, ",");
    network_members(sb, root);
    sb_puts(sb, ",");
    audio_members(sb, root, proc_root);
    sb_puts(sb, ",");
    video_members(sb, root);
}
