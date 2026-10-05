/* browser.c -- see browser.h */
#include "browser.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static int is_midi_name(const char *n)
{
    const char *dot = strrchr(n, '.');
    return dot && (!strcasecmp(dot, ".mid") || !strcasecmp(dot, ".midi") || !strcasecmp(dot, ".kar") ||
                   !strcasecmp(dot, ".smf"));
}

static int cmp_entry(const void *a, const void *b)
{
    const bentry *x = a, *y = b;
    if (!strcmp(x->name, "..")) return -1;
    if (!strcmp(y->name, "..")) return 1;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

int browser_open(browser *b, const char *dir)
{
    char real[PATH_MAX];
    DIR *d;
    struct dirent *de;
    if (!realpath(dir, real) || !(d = opendir(real))) {
        snprintf(b->msg, sizeof b->msg, "cannot open %s", dir);
        return -1;
    }
    snprintf(b->cwd, sizeof b->cwd, "%s", real);
    b->n = 0;
    while ((de = readdir(d)) != NULL) {
        char full[PATH_MAX];
        struct stat st;
        bentry e;
        if (!strcmp(de->d_name, ".")) continue;
        if (!strcmp(de->d_name, "..") && !strcmp(real, "/")) continue;
        if (de->d_name[0] == '.' && strcmp(de->d_name, "..") && !b->show_hidden) continue;
        snprintf(full, sizeof full, "%s/%s", strcmp(real, "/") ? real : "", de->d_name);
        if (stat(full, &st)) continue;
        memset(&e, 0, sizeof e);
        snprintf(e.name, sizeof e.name, "%s", de->d_name);
        e.is_dir = S_ISDIR(st.st_mode);
        e.size = (long long)st.st_size;
        if (!e.is_dir && !b->show_all && !is_midi_name(e.name)) continue;
        if (b->n == b->cap) {
            b->cap = b->cap ? b->cap * 2 : 256;
            b->e = realloc(b->e, b->cap * sizeof *b->e);
            if (!b->e) abort();
        }
        b->e[b->n++] = e;
    }
    closedir(d);
    qsort(b->e, b->n, sizeof *b->e, cmp_entry);
    b->sel = b->n > 1 && !strcmp(b->e[0].name, "..") ? 1 : 0;
    b->top = 0;
    b->msg[0] = 0;
    return 0;
}

void browser_free(browser *b)
{
    free(b->e);
    b->e = NULL;
    b->n = b->cap = 0;
}

static void go_parent(browser *b)
{
    char old[512], parent[1024];
    const char *slash = strrchr(b->cwd, '/');
    size_t i;
    if (!slash || !strcmp(b->cwd, "/")) return;
    snprintf(old, sizeof old, "%s", slash + 1);
    snprintf(parent, sizeof parent, "%.*s", slash == b->cwd ? 1 : (int)(slash - b->cwd), b->cwd);
    if (browser_open(b, parent)) return;
    for (i = 0; i < b->n; i++)            /* keep the cursor on the directory we came from */
        if (!strcmp(b->e[i].name, old)) b->sel = (int)i;
}

int browser_key(browser *b, int key, int page, char *path, size_t n)
{
    bentry *e = b->n ? &b->e[b->sel] : NULL;
    switch (key) {
    case KEY_UP: case 'k': if (b->sel > 0) b->sel--; break;
    case KEY_DOWN: case 'j': if (b->sel + 1 < (int)b->n) b->sel++; break;
    case KEY_PGUP: b->sel = b->sel > page ? b->sel - page : 0; break;
    case KEY_PGDN: b->sel = b->sel + page < (int)b->n ? b->sel + page : (int)b->n - 1; break;
    case KEY_HOME: case 'g': b->sel = 0; break;
    case KEY_END: case 'G': b->sel = b->n ? (int)b->n - 1 : 0; break;
    case KEY_LEFT: case KEY_BACKSPACE: case 'h': go_parent(b); break;
    case '~': {
        const char *home = getenv("HOME");
        if (home) browser_open(b, home);
        break;
    }
    case 'a':
        b->show_all = !b->show_all;
        browser_open(b, b->cwd);
        break;
    case 'H':
        b->show_hidden = !b->show_hidden;
        browser_open(b, b->cwd);
        break;
    case KEY_ESC:
        return BROWSE_BACK;
    case KEY_RIGHT: case KEY_ENTER: case 'l':
        if (!e) break;
        if (!strcmp(e->name, "..")) {
            go_parent(b);
        } else {
            char full[2048];
            snprintf(full, sizeof full, "%s/%s", strcmp(b->cwd, "/") ? b->cwd : "", e->name);
            if (e->is_dir) {
                browser_open(b, full);
            } else if (key != KEY_RIGHT) {
                snprintf(path, n, "%s", full);
                return BROWSE_FILE;
            }
        }
        break;
    default: break;
    }
    return BROWSE_NONE;
}

void browser_draw(sbuf *sb, browser *b, int W, int H, const char *out_name, const char *now_playing)
{
    int rows = H - 4, i;
    char buf[2048];
    if (rows < 1) rows = 1;
    if (b->sel < b->top) b->top = b->sel;
    if (b->sel >= b->top + rows) b->top = b->sel - rows + 1;
    sb_attr(sb, ATTR(16, 75, 0, 1, 0));
    sb_str(sb, " midplay ");
    sb_attr(sb, ATTR(252, -1, 0, 1, 0));
    sb_str(sb, " ");
    sb_text(sb, b->cwd, W - 10, 0);
    sb_eol(sb);
    sb_attr(sb, ATTR(245, -1, 0, 0, 0));
    snprintf(buf, sizeof buf, " Out %s%s%s", out_name, now_playing ? "   Playing " : "", now_playing ? now_playing : "");
    sb_text(sb, buf, W, 0);
    sb_eol(sb);
    for (i = 0; i < rows; i++) {
        int k = b->top + i;
        if (k < (int)b->n) {
            bentry *e = &b->e[k];
            int sel = k == b->sel, w;
            sb_attr(sb, sel ? ATTR(231, 25, 0, 1, 0) : e->is_dir ? ATTR(117, -1, 0, 1, 0) : A_NONE);
            sb_str(sb, sel ? " > " : "   ");
            snprintf(buf, sizeof buf, "%s%s", e->name, e->is_dir ? "/" : "");
            w = W - 3 - 12 - 1;              /* never the last column: some terminals wrap there */
            sb_text(sb, buf, w > 0 ? w : 0, 1);
            if (e->is_dir) sb_str(sb, "            ");
            else if (e->size < 1024 * 1024) sb_fmt(sb, "%10.1f K", (double)e->size / 1024.0);
            else sb_fmt(sb, "%10.1f M", (double)e->size / 1048576.0);
        }
        sb_eol(sb);
    }
    sb_attr(sb, ATTR(229, -1, 0, 0, 0));
    sb_text(sb, b->msg[0] ? b->msg : (b->n <= 1 ? " (no MIDI files here; a: show all files)" : ""), W, 0);
    sb_eol(sb);
    sb_attr(sb, ATTR(16, 250, 0, 0, 0));
    snprintf(buf, sizeof buf, " Enter play/open  <- parent  ~ home  a %s  H hidden  Esc back  c settings  q quit",
             b->show_all ? "MIDI only" : "all files");
    sb_text(sb, buf, W, 1);
}
