/* view.c -- see view.h */
#include "view.h"

#include <stdio.h>
#include <string.h>

#include "names.h"

static const int ch_color[16] = { 203, 209, 221, 191, 120, 85, 87, 81, 75, 141, 177, 213, 218, 223, 230, 252 };

static void reset_parts(viewstate *v)
{
    int c;
    for (c = 0; c < 16; c++) {
        chstate *h = &v->ch[c];
        float m = h->meter;
        memset(h, 0, sizeof *h);
        h->msb = c == 9 ? 127 : 0;     /* XG: part 10 is a drum part (bank MSB 127) */
        h->vol = 100;
        h->exp = 127;
        h->pan = 64;
        h->rev = 40;
        h->meter = m;
    }
}

void view_reset(viewstate *v)
{
    memset(v, 0, sizeof *v);
    reset_parts(v);
    v->mode = "--";
    v->uspq = 500000.0;
}

static void apply_item(viewstate *v, const item *it)
{
    if (it->kind == K_TEMPO) { v->uspq = it->value; return; }
    if (it->kind == K_TEXT) {
        if (it->meta != 0x03) snprintf(v->text, sizeof v->text, "%s", (char *)it->data);
        return;
    }
    if (it->kind != K_MIDI) return;
    if (it->data[0] == 0xf0) {
        if (is_xg_on(it)) { reset_parts(v); v->mode = "XG"; }
        else if (is_gm_on(it)) { reset_parts(v); v->mode = "GM"; }
        else if (is_gs_reset(it)) { reset_parts(v); v->mode = "GS"; }
        return;
    }
    if (it->data[0] < 0x80 || it->data[0] >= 0xf0) return;
    {
        chstate *h = &v->ch[it->data[0] & 15];
        uint8_t a = it->len > 1 ? it->data[1] & 127 : 0, b = it->len > 2 ? it->data[2] & 127 : 0;
        switch (it->data[0] & 0xf0) {
        case 0x80: h->on[a] = 0; break;
        case 0x90: h->on[a] = b; if (b > h->hit) h->hit = b; break;
        case 0xb0:
            switch (a) {
            case 0: h->msb = b; break;      case 32: h->lsb = b; break;
            case 7: h->vol = b; break;      case 10: h->pan = b; break;
            case 11: h->exp = b; break;     case 91: h->rev = b; break;
            case 93: h->cho = b; break;
            case 120: case 123: memset(h->on, 0, sizeof h->on); break;
            case 121: h->exp = 127; h->bend = 0; break;
            default: break;
            }
            break;
        case 0xc0: h->prog = a; break;
        case 0xe0: h->bend = (int)((b << 7) | a) - 8192; break;
        default: break;
        }
    }
}

void view_advance(viewstate *v, const song *s, uint32_t tick)
{
    if (tick < v->applied_tick) view_reset(v);
    while (v->idx < s->n && s->items[v->idx].tick <= tick) apply_item(v, &s->items[v->idx++]);
    v->applied_tick = tick;
}

uint32_t view_mute_mask(const playui *u)
{
    return u->mute | (u->solo ? ~u->solo & 0xffffu : 0);
}

static void meter(sbuf *s, float v, int w, int color)
{
    static const char *const part[8] = { " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉" };
    int i, eighths = (int)(v * (float)w * 8.0f + 0.5f);
    if (eighths > w * 8) eighths = w * 8;
    if (eighths < 0) eighths = 0;
    sb_attr(s, ATTR(color, 236, 0, 0, 0));
    for (i = 0; i < w; i++) {
        int e = eighths - i * 8;
        if (s->ascii) sb_str(s, e >= 4 ? "#" : " ");
        else sb_str(s, e >= 8 ? "█" : e > 0 ? part[e] : " ");
    }
    sb_attr(s, A_NONE);
}

static void draw_lane(sbuf *sb, const song *s, int row, int muted, int drum, int col, uint32_t tick, int lane, int ph)
{
    static int top[512];
    uint32_t cell = s->division / 4 ? s->division / 4 : 1, base = tick / cell * cell;
    int64_t ws0 = (int64_t)base - (int64_t)ph * cell, we = ws0 + (int64_t)lane * cell;
    const note *nt = s->notes[row];
    size_t lo = 0, hi = s->nnotes[row];
    int64_t from = ws0 - (int64_t)s->maxdur[row];
    int c;
    for (c = 0; c < lane; c++) top[c] = -1;
    while (lo < hi) {                     /* first note that can still be sounding at ws0 */
        size_t mid = (lo + hi) / 2;
        if ((int64_t)nt[mid].on < from) lo = mid + 1; else hi = mid;
    }
    for (; lo < s->nnotes[row] && (int64_t)nt[lo].on < we; lo++) {
        int64_t a = nt[lo].on, b = nt[lo].off > nt[lo].on ? nt[lo].off : nt[lo].on + 1;
        int c0, c1;
        if (b <= ws0) continue;
        c0 = a < ws0 ? 0 : (int)((a - ws0) / cell);
        c1 = (int)((b - 1 - ws0) / cell);
        if (c1 >= lane) c1 = lane - 1;
        for (c = c0; c <= c1; c++)
            if (nt[lo].key > top[c]) top[c] = nt[lo].key;
    }
    for (c = 0; c < lane; c++) {
        int64_t t0 = ws0 + (int64_t)c * cell;
        int bg = c == ph ? 239 : -1;
        if (top[c] >= 0) {
            static const char *const lv[8] = { "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
            int k8 = (top[c] - 24) * 8 / 72;
            if (k8 < 0) k8 = 0;
            if (k8 > 7) k8 = 7;
            sb_attr(sb, ATTR(muted ? 240 : col, bg, c < ph || muted, c == ph, 0));
            if (sb->ascii) sb_str(sb, drum ? "*" : "=");
            else sb_str(sb, drum ? "•" : lv[k8]);
        } else {
            uint32_t b2, bt2, tk2;
            const char *g = " ";
            if (t0 >= 0) {
                smf_tick_to_mbt(s, (uint32_t)t0, &b2, &bt2, &tk2);
                if (tk2 < cell) g = bt2 == 0 ? (sb->ascii ? "|" : "┊") : (sb->ascii ? "." : "·");
            }
            sb_attr(sb, ATTR(c == ph ? 250 : 238, bg, 0, 0, 0));
            sb_str(sb, g);
        }
    }
}

void view_draw(sbuf *sb, player *p, viewstate *v, playui *u, int W, int H)
{
    const song *s = p->s;
    uint64_t pos = atomic_load(&p->pub_pos);
    double sec = (double)pos / p->b->rate, total = (double)s->end_frame / p->b->rate;
    uint32_t tick = (uint32_t)smf_sec_to_tick(s, sec), bar, beat, tk, cell = s->division / 4 ? s->division / 4 : 1;
    int playing = atomic_load(&p->playing), finished = atomic_load(&p->finished), seeking = atomic_load(&p->seek_state);
    int row, c, i, wide, fixed, lane, ph;
    char buf[1024];

    view_advance(v, s, tick);
    {
        float pl = player_take_float(&p->peak_l), pr = player_take_float(&p->peak_r), ld = player_take_float(&p->load);
        u->meter_l = pl > u->meter_l ? pl : u->meter_l * 0.85f;
        u->meter_r = pr > u->meter_r ? pr : u->meter_r * 0.85f;
        u->load_shown = ld > u->load_shown ? ld : u->load_shown * 0.95f + ld * 0.05f;
    }
    if (W > 500) W = 500;
    if (W < 62 || H < 21) {
        sb_fmt(sb, "terminal too small (%dx%d): needs 62x21", W, H);
        return;
    }
    smf_tick_to_mbt(s, tick, &bar, &beat, &tk);

    /* line 1: transport */
    sb_attr(sb, ATTR(16, 75, 0, 1, 0));
    sb_str(sb, " midplay ");
    sb_attr(sb, ATTR(finished ? 245 : playing ? 120 : 221, -1, 0, 1, 0));
    sb_str(sb, seeking ? " SEEK  " : finished ? " END   " : playing ? " PLAY  " : " STOP  ");
    sb_attr(sb, A_NONE);
    {
        const tsig *g = smf_sig_at(s, tick);
        int load = (int)(u->load_shown * 100.0f + 0.5f);
        if (p->b->is_au)
            snprintf(buf, sizeof buf, " %s  BPM %6.2f  %u/%u  %04u:%02u:%03u  %02d:%04.1f/%02d:%02d  DSP %3d%%  ", v->mode,
                     60000000.0 / v->uspq, g->num, g->den, bar + 1, beat + 1, tk, (int)(sec / 60), sec - 60 * (int)(sec / 60),
                     (int)(total / 60), (int)total % 60, load);
        else
            snprintf(buf, sizeof buf, " %s  BPM %6.2f  %u/%u  %04u:%02u:%03u  %02d:%04.1f/%02d:%02d  ", v->mode,
                     60000000.0 / v->uspq, g->num, g->den, bar + 1, beat + 1, tk, (int)(sec / 60), sec - 60 * (int)(sec / 60),
                     (int)(total / 60), (int)total % 60);
        i = 16 + sb_text(sb, buf, W - 16, 0);
        if (p->b->is_au && W - i >= 21) {
            sb_str(sb, "L");
            meter(sb, u->meter_l, 8, u->meter_l >= 1.0f ? 196 : 120);
            sb_str(sb, " R");
            meter(sb, u->meter_r, 8, u->meter_r >= 1.0f ? 196 : 120);
        }
        sb_eol(sb);
    }
    /* line 2: the output and the file */
    sb_attr(sb, ATTR(245, -1, 0, 0, 0));
    sb_str(sb, " Out ");
    sb_attr(sb, ATTR(117, -1, 0, 1, 0));
    if (p->b->is_au) snprintf(buf, sizeof buf, "%s (AU, %.0f Hz)", p->b->name, p->b->rate);
    else snprintf(buf, sizeof buf, "%s (MIDI)", p->b->name);
    i = 5 + sb_text(sb, buf, W - 5, 0);
    if (W - i > 10) {
        const char *file = strrchr(s->path, '/') ? strrchr(s->path, '/') + 1 : s->path;
        sb_attr(sb, ATTR(245, -1, 0, 0, 0));
        sb_str(sb, "   File ");
        sb_attr(sb, ATTR(252, -1, 0, 1, 0));
        if (s->title[0]) snprintf(buf, sizeof buf, "%s  \"%s\"", file, s->title);
        else snprintf(buf, sizeof buf, "%s", file);
        sb_text(sb, buf, W - i - 8, 0);
    }
    sb_eol(sb);

    wide = W >= 110;
    fixed = wide ? 88 : 61;
    lane = W - fixed - 1;
    if (lane < 8) lane = 0;
    if (lane > 512) lane = 512;
    ph = lane / 4;

    /* column titles, bar numbers over the lane */
    sb_attr(sb, ATTR(245, -1, 0, 0, 0));
    sb_str(sb, wide ? "   CH    Voice            Bank    PRG VOL EXP PAN REV CHO Level    Notes        "
                    : "   CH    Voice            Bank    PRG VOL EXP PAN Level    ");
    if (lane) {
        uint32_t base = tick / cell * cell;
        int skip = 0;
        sb_str(sb, " ");
        for (c = 0; c < lane; c++) {
            int64_t t0 = (int64_t)base + (int64_t)(c - ph) * cell;
            uint32_t b2, bt2, tk2;
            if (skip) { skip--; continue; }
            if (t0 >= 0) {
                smf_tick_to_mbt(s, (uint32_t)t0, &b2, &bt2, &tk2);
                if (bt2 == 0 && tk2 < cell) {
                    int k = snprintf(buf, sizeof buf, "%u", b2 + 1);
                    if (c + k <= lane) {
                        sb_attr(sb, ATTR(c == ph ? 231 : 245, -1, 0, c == ph, 0));
                        sb_str(sb, buf);
                        sb_attr(sb, ATTR(245, -1, 0, 0, 0));
                        skip = k - 1;
                        continue;
                    }
                }
            }
            sb_str(sb, " ");
        }
    }
    sb_eol(sb);

    /* 16 parts */
    for (row = 0; row < 16; row++) {
        chstate *h = &v->ch[row];
        int muted = ((u->mute >> row) & 1) || (u->solo && !((u->solo >> row) & 1));
        int drum = h->msb >= 126, col = ch_color[row], k, n = 0, lvl = 0;
        char name[32];
        sb_attr(sb, row == u->sel ? ATTR(231, 238, 0, 1, 0) : A_NONE);
        sb_str(sb, row == u->sel ? ">" : " ");
        sb_attr(sb, ATTR(col, row == u->sel ? 238 : -1, muted, 1, 0));
        sb_fmt(sb, " %2d ", row + 1);
        sb_attr(sb, ATTR(196, -1, 0, 1, 0));
        sb_str(sb, ((u->mute >> row) & 1) ? "M" : " ");
        sb_attr(sb, ATTR(226, -1, 0, 1, 0));
        sb_str(sb, ((u->solo >> row) & 1) ? "S" : " ");
        sb_attr(sb, ATTR(muted ? 242 : 252, -1, muted, 0, 0));
        voice_name(name, sizeof name, h->msb, h->prog);
        sb_fmt(sb, " %-16.16s %03u:%03u %3u %3u %3u ", name, h->msb, h->lsb, h->prog + 1u, h->vol, h->exp);
        if (h->pan == 64) sb_str(sb, " C ");
        else sb_fmt(sb, "%c%2d", h->pan < 64 ? 'L' : 'R', h->pan < 64 ? 64 - h->pan : h->pan - 64);
        if (wide) sb_fmt(sb, " %3u %3u", h->rev, h->cho);
        sb_str(sb, " ");
        for (k = 0; k < 128; k++)
            if (h->on[k]) { n++; if (h->on[k] > lvl) lvl = h->on[k]; }
        if (h->hit > lvl) lvl = h->hit;
        h->hit = 0;
        {
            float target = (float)lvl / 127.0f * (float)h->vol / 127.0f * (float)h->exp / 127.0f;
            h->meter = target > h->meter ? target : h->meter * 0.82f;
        }
        meter(sb, muted ? 0.0f : h->meter, 8, col);
        if (wide) {
            char notes[64] = "";
            size_t len = 0;
            if (drum && n) {
                snprintf(notes, sizeof notes, "%d hit%s", n, n > 1 ? "s" : "");
            } else {
                for (k = 0; k < 128 && len < 13; k++)
                    if (h->on[k])
                        len += (size_t)snprintf(notes + len, sizeof notes - len, "%s%s%d", len ? " " : "", note_names[k % 12], k / 12 - 1);
            }
            sb_attr(sb, ATTR(col, -1, 0, 0, 0));
            sb_fmt(sb, " %-12.12s", notes);
        }
        sb_str(sb, " ");
        if (lane) draw_lane(sb, s, row, muted, drum, col, tick, lane, ph);
        sb_eol(sb);
    }

    /* the latest text/lyric and the event list */
    sb_attr(sb, ATTR(245, -1, 0, 0, 0));
    sb_str(sb, sb->ascii ? "-- Events " : "── Events ");
    sb_attr(sb, ATTR(229, -1, 0, 0, 0));
    sb_text(sb, v->text, W - 11, 0);
    sb_eol(sb);
    {
        int rows = H - 21, first, cur;
        size_t lo = 0, hi = s->nlist;
        while (lo < hi) {                 /* the last listed item at or before the playhead */
            size_t mid = (lo + hi) / 2;
            if (s->items[s->list[mid]].tick <= tick) lo = mid + 1; else hi = mid;
        }
        cur = (int)lo - 1;
        first = cur - rows / 3;
        if (first < 0) first = 0;
        for (i = 0; i < rows; i++) {
            int k = first + i;
            if (k < (int)s->nlist) {
                const item *it = &s->items[s->list[k]];
                uint32_t b2, bt2, tk2;
                char d[512];
                int chn = it->kind == K_MIDI && it->data[0] >= 0x80 && it->data[0] < 0xf0 ? (it->data[0] & 15) : -1;
                smf_tick_to_mbt(s, it->tick, &b2, &bt2, &tk2);
                describe(d, sizeof d, it);
                sb_attr(sb, k == cur ? ATTR(231, 238, 0, 1, 0) : k < cur ? ATTR(244, -1, 0, 0, 0) : A_NONE);
                sb_fmt(sb, "%s %04u:%02u:%03u  ", k == cur ? ">" : " ", b2 + 1, bt2 + 1, tk2);
                if (chn >= 0) {
                    sb_attr(sb, k == cur ? ATTR(ch_color[chn], 238, 0, 1, 0) : ATTR(ch_color[chn], -1, k < cur, 0, 0));
                    sb_fmt(sb, "Ch%-2d ", chn + 1);
                } else {
                    sb_str(sb, "  -  ");
                }
                sb_attr(sb, k == cur ? ATTR(231, 238, 0, 1, 0) : k < cur ? ATTR(244, -1, 0, 0, 0) : A_NONE);
                sb_text(sb, d, W - 20, k == cur);
            }
            sb_eol(sb);
        }
    }
    /* keys and counters (no newline: the last line must not scroll) */
    sb_attr(sb, ATTR(16, 250, 0, 0, 0));
    if (u->status[0])
        snprintf(buf, sizeof buf, " %s", u->status);
    else
        snprintf(buf, sizeof buf, " Space play/stop  <- -> bar  , . 8 bars  Up Down part  m mute  s solo  u clear  "
                 "r top  o open  c settings  q quit   late %u drop %u skip %u ",
                 atomic_load(&p->late), atomic_load(&p->dropped), atomic_load(&p->skipped));
    sb_text(sb, buf, W, 1);
}
