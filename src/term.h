/* term.h -- the terminal: raw keys, size, and a screen buffer with colours and display widths. */
#ifndef MIDPLAY_TERM_H
#define MIDPLAY_TERM_H
#include <signal.h>
#include <stddef.h>

/* keys returned by term_key(); printable characters are returned as themselves */
enum {
    KEY_NONE = -1,
    KEY_UP = 0x1000, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_PGUP, KEY_PGDN, KEY_HOME, KEY_END,
    KEY_ENTER, KEY_BACKSPACE, KEY_ESC
};

extern volatile sig_atomic_t term_quit;   /* set by SIGINT / SIGTERM */

int term_init(void);                      /* raw mode + alternate screen; -1 when not a terminal */
void term_restore(void);
void term_size(int *w, int *h);
/* wait up to `ms` for input; returns the next key or KEY_NONE */
int term_key(int ms);

/* ---- screen buffer ---- */
typedef struct {
    char *b;
    size_t n, cap;
    int attr;
    int ascii;                            /* draw with ASCII only */
} sbuf;

/* attributes: 256-colour fg / bg (-1 = default), dim, bold, reverse */
#define ATTR(fg, bg, dim, bold, rev) \
    (((fg) & 511) | (((bg) & 511) << 9) | ((dim) << 18) | ((bold) << 19) | ((rev) << 20))
#define A_NONE ATTR(-1, -1, 0, 0, 0)

void sb_begin(sbuf *s);                   /* empty, cursor home */
void sb_put(sbuf *s, const char *p, size_t n);
void sb_str(sbuf *s, const char *p);
void sb_fmt(sbuf *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_attr(sbuf *s, int attr);
/* append at most `w` columns of UTF-8 text; pad with spaces to `w` when `pad`; returns columns used */
int sb_text(sbuf *s, const char *t, int w, int pad);
void sb_eol(sbuf *s);                     /* reset attributes, clear to end of line, newline */
void sb_flush(sbuf *s);                   /* clear below and write it all */
int text_width(const char *t);

#endif
