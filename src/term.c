/* term.c -- see term.h */
#include "term.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

volatile sig_atomic_t term_quit;
static struct termios saved;
static int active;
static unsigned char inbuf[64];
static int in_n, in_pos;

static void out_str(const char *t) { (void)!write(STDOUT_FILENO, t, strlen(t)); }

static void on_signal(int sig) { (void)sig; term_quit = 1; }

int term_init(void)
{
    struct termios t;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || tcgetattr(STDIN_FILENO, &saved))
        return -1;
    active = 1;
    atexit(term_restore);
    t = saved;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    out_str("\x1b[?1049h\x1b[?25l\x1b[2J");
    return 0;
}

void term_restore(void)
{
    if (!active) return;
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    out_str("\x1b[0m\x1b[?25h\x1b[?1049l");
    active = 0;
}

void term_size(int *w, int *h)
{
    struct winsize ws;
    *w = 80;
    *h = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    }
}

static int fill(int ms)
{
    fd_set fds;
    struct timeval tv;
    ssize_t n;
    if (in_pos < in_n) return 1;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0) return 0;
    n = read(STDIN_FILENO, inbuf, sizeof inbuf);
    if (n <= 0) return 0;
    in_n = (int)n;
    in_pos = 0;
    return 1;
}

static int next_byte(int ms)
{
    if (!fill(ms)) return -1;
    return inbuf[in_pos++];
}

int term_key(int ms)
{
    int c = next_byte(ms), c2, c3;
    if (c < 0) return KEY_NONE;
    if (c == '\r' || c == '\n') return KEY_ENTER;
    if (c == 127 || c == 8) return KEY_BACKSPACE;
    if (c != 0x1b) return c;
    c2 = next_byte(20);                   /* the rest of an escape sequence arrives together */
    if (c2 < 0) return KEY_ESC;
    if (c2 != '[' && c2 != 'O') return KEY_ESC;
    c3 = next_byte(20);
    switch (c3) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    default: break;
    }
    if (c3 >= '0' && c3 <= '9') {
        int c4 = next_byte(20);
        while (c4 >= '0' && c4 <= '9') c4 = next_byte(20);   /* modifiers etc.: skip to the final byte */
        if (c4 == '~') {
            switch (c3) {
            case '1': case '7': return KEY_HOME;
            case '4': case '8': return KEY_END;
            case '5': return KEY_PGUP;
            case '6': return KEY_PGDN;
            default: break;
            }
        }
    }
    return KEY_NONE;
}

/* ---- screen buffer ---- */
void sb_put(sbuf *s, const char *p, size_t n)
{
    if (s->n + n + 1 > s->cap) {
        s->cap = (s->n + n + 1) * 2;
        s->b = realloc(s->b, s->cap);
        if (!s->b) abort();
    }
    memcpy(s->b + s->n, p, n);
    s->n += n;
}

void sb_str(sbuf *s, const char *p) { sb_put(s, p, strlen(p)); }

void sb_fmt(sbuf *s, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) sb_put(s, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

void sb_begin(sbuf *s)
{
    s->n = 0;
    s->attr = -2;
    sb_str(s, "\x1b[H");
}

void sb_attr(sbuf *s, int a)
{
    int fg = a & 511, bg = (a >> 9) & 511;
    if (a == s->attr) return;
    s->attr = a;
    sb_str(s, "\x1b[0");
    if (fg != 511) sb_fmt(s, ";38;5;%d", fg);
    if (bg != 511) sb_fmt(s, ";48;5;%d", bg);
    if (a & (1 << 18)) sb_str(s, ";2");
    if (a & (1 << 19)) sb_str(s, ";1");
    if (a & (1 << 20)) sb_str(s, ";7");
    sb_str(s, "m");
}

void sb_eol(sbuf *s)
{
    sb_attr(s, A_NONE);
    sb_str(s, "\x1b[K\r\n");
}

void sb_flush(sbuf *s)
{
    sb_attr(s, A_NONE);
    sb_str(s, "\x1b[J");
    (void)!write(STDOUT_FILENO, s->b, s->n);
}

/* columns of one code point: combining marks 0 (macOS file names are decomposed: "で" is "て" + U+3099),
 * wide East Asian ranges 2, the rest 1 */
static int cp_width(uint32_t c)
{
    if ((c >= 0x0300 && c <= 0x036f) || c == 0x3099 || c == 0x309a || (c >= 0xfe00 && c <= 0xfe0f) ||
        (c >= 0xfe20 && c <= 0xfe2f) || c == 0x200b || c == 0x200d)
        return 0;
    if ((c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf) || (c >= 0xac00 && c <= 0xd7a3) ||
        (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe30 && c <= 0xfe4f) || (c >= 0xff00 && c <= 0xff60) ||
        (c >= 0xffe0 && c <= 0xffe6) || (c >= 0x1f300 && c <= 0x1faff) || c >= 0x20000)
        return 2;
    return 1;
}

static int decode(const uint8_t *p, uint32_t *cp)
{
    uint32_t c = p[0];
    int k = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1, j;
    for (j = 1; j < k; j++)
        if ((p[j] & 0xc0) != 0x80) { *cp = '?'; return j; }
    if (k == 2) c = ((c & 0x1fu) << 6) | (p[1] & 0x3fu);
    else if (k == 3) c = ((c & 0x0fu) << 12) | ((p[1] & 0x3fu) << 6) | (p[2] & 0x3fu);
    else if (k == 4) c = ((c & 0x07u) << 18) | ((p[1] & 0x3fu) << 12) | ((p[2] & 0x3fu) << 6) | (p[3] & 0x3fu);
    else if (c >= 0x80) c = '?';
    *cp = c;
    return k;
}

int sb_text(sbuf *s, const char *t, int w, int pad)
{
    const uint8_t *p = (const uint8_t *)t;
    int used = 0;
    while (*p) {
        uint32_t c;
        int k = decode(p, &c), cw = cp_width(c);
        if (used + cw > w) break;
        if (c < 0x20) sb_put(s, " ", 1);
        else sb_put(s, (const char *)p, (size_t)k);
        used += cw;
        p += k;
    }
    if (pad)
        for (; used < w; used++) sb_put(s, " ", 1);
    return used;
}

int text_width(const char *t)
{
    const uint8_t *p = (const uint8_t *)t;
    int used = 0;
    while (*p) {
        uint32_t c;
        p += decode(p, &c);
        used += cp_width(c);
    }
    return used;
}
