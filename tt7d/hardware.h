/* ABOUTME: GET /api/v1/hardware: the low-level discovered mappings (SPEC 20) from sysfs and /proc.
 * ABOUTME: Raw attribute strings as the kernel reports them; roots are configurable so tests use fixtures. */
#ifndef TT7D_HARDWARE_H
#define TT7D_HARDWARE_H

#include <stddef.h>

#include "json.h"

/* Append the members "input", "backlight", "power_supplies",
 * "thermal_zones", "network_interfaces", "audio" and "video_devices" (no
 * braces, no leading comma). The caller adds "display", which it knows from
 * the open framebuffer. */
void hardware_members(struct sbuf *sb, const char *sysfs_root, const char *proc_root);

/* One card line of /proc/asound/cards, e.g.
 * " 0 [RK29RT3261     ]: RK29_RT3261 - RK29_RT3261". Returns 0, or -1 for
 * any other line (the indented long-name lines, "--- no soundcards ---"). */
int asound_card_parse(const char *line, int *index, char *id, size_t idlen, char *name, size_t namelen);

#endif
