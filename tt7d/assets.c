/* ABOUTME: Lookup in the generated table of embedded files (assets.h).
 * ABOUTME: The table itself is build/gen/tt7d_assets.c, written by tt7d/embed.py. */
#include "assets.h"

#include <string.h>

const struct asset *asset_find(const char *name) {
    for (size_t i = 0; i < assets_count; i++)
        if (!strcmp(assets[i].name, name)) return &assets[i];
    return NULL;
}
