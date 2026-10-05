/* browser.h -- a small file browser: directories and MIDI files */
#ifndef MIDPLAY_BROWSER_H
#define MIDPLAY_BROWSER_H
#include <stddef.h>

#include "term.h"

typedef struct {
    char name[512];
    int is_dir;
    long long size;
} bentry;

typedef struct {
    char cwd[1024];
    bentry *e;
    size_t n, cap;
    int sel, top;
    int show_all;                /* every file, not only .mid/.midi/.kar/.smf */
    int show_hidden;
    char msg[256];
} browser;

/* list a directory (made absolute); -1 when it cannot be read (the old listing stays) */
int browser_open(browser *b, const char *dir);
void browser_free(browser *b);
enum { BROWSE_NONE, BROWSE_FILE, BROWSE_BACK };
/* handle a key: BROWSE_FILE fills path with the chosen file; BROWSE_BACK: Esc */
int browser_key(browser *b, int key, int page, char *path, size_t n);
void browser_draw(sbuf *sb, browser *b, int W, int H, const char *out_name, const char *now_playing);

#endif
