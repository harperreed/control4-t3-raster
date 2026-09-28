/* ABOUTME: Discovers hardware capabilities and reads live state from sysfs for /info and /state.
 * ABOUTME: Every path is under a configurable root, so host tests point it at test/fixtures. */
#ifndef TT7D_SYSINFO_H
#define TT7D_SYSINFO_H

#include <stddef.h>

#include "json.h"

#define MAX_NAMES 32
#define NAME_LEN 64

/* Directory entries, sorted. */
struct names {
    int n;
    char v[MAX_NAMES][NAME_LEN];
};

/* Sorted entries of <root>/<rel>, without dot files. Empty if absent. */
void list_dir(const char *root, const char *rel, struct names *out);

/* Read <root>/<dir>/<name>/<attr>, trailing whitespace stripped. 0 or -1. */
int read_attr(const char *root, const char *dir, const char *name, const char *attr, char *buf, size_t n);

/* The same attribute as a decimal integer, or fallback if absent or not a number. */
long read_long(const char *root, const char *dir, const char *name, const char *attr, long fallback);

/* 1 if an input device modalias ("input:b...-e0,1,3,k...,a2F,30,...") lists
 * `code` in section `sec` ('e' event types, 'k' keys, 'a' abs axes, ...). */
int modalias_has(const char *modalias, char sec, unsigned code);

/* The interface's IPv4 address from the kernel (SIOCGIFADDR), or "" if it has none. */
void ipv4_of(const char *ifname, char *out, size_t n);

/* What an input device is for: "touchscreen", "buttons" or "other", from its
 * name and modalias. /info capabilities and /hardware share this rule. */
const char *sysinfo_input_role(const char *name, const char *modalias);

/* Append the "capabilities" object's members (no braces) for /info, except the camera (camera.h). */
void sysinfo_capabilities(struct sbuf *sb, const char *sysfs_root);

/* Append the "power" object and the "network" object (with keys) for /state. */
void sysinfo_power(struct sbuf *sb, const char *sysfs_root);
void sysinfo_network(struct sbuf *sb, const char *sysfs_root);

/* Backlight for /state: sets *on (-1 unknown) and *percent (-1 unknown). */
void sysinfo_backlight(const char *sysfs_root, int *on, int *percent);

/* Plain values for MQTT telemetry and Home Assistant discovery. -1 means
 * unknown or absent; strings are "" when absent. */
struct sysinfo_values {
    int has_battery;      /* a Battery supply that is present */
    int battery_percent;  /* an estimate (gotchas.md) */
    int charging;         /* 1, 0, or -1 unknown */
    int external_power;   /* the Mains supply's online: 1, 0, or -1 */
    int has_backlight;
    int backlight_max;
    int display_on;       /* 1, 0, or -1 */
    int brightness_percent;
    int has_wifi, has_ethernet;
    char wifi_ip[16], ethernet_ip[16];
};
void sysinfo_read(const char *sysfs_root, struct sysinfo_values *v);

#endif
