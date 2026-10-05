/* outputs.c -- see outputs.h */
#include "outputs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t outputs_list(output_info *out, size_t cap)
{
    size_t n = au_list(out, cap);
    return n + midi_list(out + n, cap - n);
}

static int contains_nocase(const char *hay, const char *needle)
{
    size_t i, j, n = strlen(needle);
    if (!n) return 0;
    for (i = 0; hay[i]; i++) {
        for (j = 0; j < n && hay[i + j]; j++)
            if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j])) break;
        if (j == n) return 1;
    }
    return 0;
}

int outputs_find(const char *spec, output_info *found)
{
    output_info all[256];
    size_t n = outputs_list(all, 256), i;
    char *end;
    long k = strtol(spec, &end, 10);
    if (*spec && !*end) {
        if (k < 1 || (size_t)k > n) return -1;
        *found = all[k - 1];
        return 0;
    }
    for (i = 0; i < n; i++)               /* exact spec first */
        if (!strcmp(all[i].spec, spec)) { *found = all[i]; return 0; }
    for (i = 0; i < n; i++) {
        const char *want = NULL;
        if (!strncmp(spec, "au:", 3) && all[i].is_au) want = spec + 3;
        else if (!strncmp(spec, "midi:", 5) && !all[i].is_au) want = spec + 5;
        else if (!strchr(spec, ':')) want = spec;
        if (want && (contains_nocase(all[i].name, want) || contains_nocase(all[i].spec, want))) {
            *found = all[i];
            return 0;
        }
    }
    return -1;
}

int outputs_default(output_info *found)
{
    output_info all[256];
    size_t n = outputs_list(all, 256), i;
    for (i = 0; i < n; i++)
        if (!strcmp(all[i].spec, "au:appl/dls ")) { *found = all[i]; return 0; }
    if (!n) return -1;
    *found = all[0];
    return 0;
}

backend *output_open(const output_info *o, int null_audio, char *err, size_t errlen)
{
    return o->is_au ? au_open(o, null_audio, err, errlen) : midi_open(o, err, errlen);
}

int output_render_hash(const output_info *o, song *s, double rate, char hex[65], char *err, size_t errlen)
{
    if (!o->is_au) {
        snprintf(err, errlen, "only an Audio Unit can render offline");
        return -1;
    }
    return au_render_hash(o, s, rate, hex, err, errlen);
}
