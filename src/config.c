/* config.c -- see config.h */
#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void config_defaults(config *c)
{
    memset(c, 0, sizeof *c);
    c->autoplay = 1;
}

const char *config_path(void)
{
    static char path[1024];
    const char *env = getenv("MIDPLAY_CONFIG"), *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (env && *env) snprintf(path, sizeof path, "%s", env);
    else if (xdg && *xdg) snprintf(path, sizeof path, "%s/midplay/config", xdg);
    else snprintf(path, sizeof path, "%s/.config/midplay/config", home ? home : ".");
    return path;
}

void config_load(config *c)
{
    FILE *f = fopen(config_path(), "r");
    char line[1400];
    config_defaults(c);
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *v;
        size_t n;
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        v = eq + 1;
        n = strlen(v);
        while (n && (v[n - 1] == '\n' || v[n - 1] == '\r')) v[--n] = 0;
        if (!strcmp(line, "output")) snprintf(c->output, sizeof c->output, "%s", v);
        else if (!strcmp(line, "autoplay")) c->autoplay = atoi(v) != 0;
        else if (!strcmp(line, "exit_at_end")) c->exit_at_end = atoi(v) != 0;
        else if (!strcmp(line, "ascii")) c->ascii = atoi(v) != 0;
        else if (!strcmp(line, "last_dir")) snprintf(c->last_dir, sizeof c->last_dir, "%s", v);
    }
    fclose(f);
}

static void make_parents(const char *path)
{
    char tmp[1024];
    char *p;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) && errno != EEXIST) return;
            *p = '/';
        }
}

int config_save(const config *c)
{
    const char *path = config_path();
    FILE *f;
    make_parents(path);
    f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# midplay settings\noutput=%s\nautoplay=%d\nexit_at_end=%d\nascii=%d\nlast_dir=%s\n",
            c->output, c->autoplay, c->exit_at_end, c->ascii, c->last_dir);
    return fclose(f) ? -1 : 0;
}
