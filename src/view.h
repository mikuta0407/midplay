/* view.h -- the player screen: transport, the output, 16 parts with a piano-roll lane, the event list */
#ifndef MIDPLAY_VIEW_H
#define MIDPLAY_VIEW_H
#include <stdint.h>

#include "player.h"
#include "term.h"

typedef struct {
    uint8_t prog, msb, lsb, vol, exp, pan, rev, cho, hit;
    int bend;
    uint8_t on[128];
    float meter;
} chstate;

/* the song's state at the playhead, replayed from the events (for display only) */
typedef struct {
    chstate ch[16];
    size_t idx;
    uint32_t applied_tick;
    const char *mode;
    double uspq;
    char text[256];
} viewstate;

typedef struct {
    int sel;                     /* selected part */
    uint32_t mute, solo;
    float meter_l, meter_r, load_shown;
    char status[160];            /* a message for the bottom line */
} playui;

void view_reset(viewstate *v);
void view_advance(viewstate *v, const song *s, uint32_t tick);
void view_draw(sbuf *sb, player *p, viewstate *v, playui *u, int W, int H);
/* the effective mute mask for the player */
uint32_t view_mute_mask(const playui *u);

#endif
