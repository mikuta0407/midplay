/* smf.c -- see smf.h.  Running status, the truncated last event of a track; events are ordered by
 * tick, then by position in the file. */
#include "smf.h"

#include <iconv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK 128u

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return q;
}

static void add_item(song *s, const item *it)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 1024;
        s->items = xrealloc(s->items, s->cap * sizeof *s->items);
    }
    s->items[s->n++] = *it;
}

/* text metas: keep UTF-8, else convert from Shift_JIS (CP932), else replace non-ASCII by '?' */
static uint8_t *to_utf8(const uint8_t *p, size_t n)
{
    size_t i = 0, cap = n * 3 + 1;
    uint8_t *out = xrealloc(NULL, cap);
    int valid = 1;
    while (i < n && valid) {
        uint8_t c = p[i];
        size_t k = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0, j;
        if (!k || i + k > n) { valid = 0; break; }
        for (j = 1; j < k; j++)
            if ((p[i + j] & 0xc0) != 0x80) valid = 0;
        i += k;
    }
    if (valid) {
        memcpy(out, p, n);
        out[n] = 0;
    } else {
        iconv_t cd = iconv_open("UTF-8", "CP932");
        char *in = (char *)p, *o = (char *)out;
        size_t inl = n, outl = cap - 1;
        int ok = cd != (iconv_t)-1 && iconv(cd, &in, &inl, &o, &outl) != (size_t)-1;
        if (cd != (iconv_t)-1) iconv_close(cd);
        if (ok) {
            *o = 0;
        } else {
            for (i = 0; i < n; i++) out[i] = p[i] < 0x80 ? p[i] : '?';
            out[n] = 0;
        }
    }
    for (i = 0; out[i]; i++)
        if (out[i] < 0x20) out[i] = ' ';
    return out;
}

static int read_vlq(const uint8_t *d, size_t end, size_t *off, uint32_t *v)
{
    int i;
    *v = 0;
    for (i = 0; i < 4; i++) {
        uint8_t b;
        if (*off >= end) return -1;
        b = d[(*off)++];
        *v = (*v << 7) | (b & 0x7fu);
        if (!(b & 0x80u)) return 0;
    }
    return -1;
}

static int parse_track(song *s, const uint8_t *d, size_t size, size_t off, size_t end, uint32_t *order, int first)
{
    uint32_t tick = 0;
    int running = -1;
    while (off < end) {
        uint32_t delta, len;
        uint8_t st;
        item it;
        memset(&it, 0, sizeof it);
        it.gate = -1;
        if (read_vlq(d, end, &off, &delta) || delta > UINT32_MAX - tick || off >= end) return -1;
        tick += delta;
        it.tick = tick;
        st = d[off];
        if (st == 0xff) {
            uint8_t type;
            if (++off >= end) return -1;
            type = d[off++];
            if (read_vlq(d, end, &off, &len) || (size_t)len > end - off) return -1;
            if (type == 0x51 && len >= 3) {
                it.kind = K_TEMPO;
                it.value = ((uint32_t)d[off] << 16) | ((uint32_t)d[off + 1] << 8) | d[off + 2];
                if (!it.value) return -1;
            } else if (type == 0x58 && len >= 2) {
                it.kind = K_TSIG;
                it.value = ((uint32_t)d[off] << 8) | (d[off + 1] < 8 ? d[off + 1] : 2);
            } else if (type == 0x01 || type == 0x03 || type == 0x05 || type == 0x06) {
                it.kind = K_TEXT;
                it.meta = type;
                it.data = to_utf8(d + off, len);
                if (type == 0x03 && first && !s->title[0])
                    snprintf(s->title, sizeof s->title, "%s", (char *)it.data);
            } else {
                it.kind = 0xff;
            }
            if (it.kind != 0xff) {
                it.order = (*order)++;
                add_item(s, &it);
            }
            off += len;
            running = -1;
        } else if (st == 0xf0 || st == 0xf7) {
            off++;
            if (read_vlq(d, end, &off, &len) || (size_t)len > end - off) return -1;
            it.kind = K_MIDI;
            it.len = len + (st == 0xf0);
            if (!it.len) return -1;
            it.data = xrealloc(NULL, it.len);
            if (st == 0xf0) {
                it.data[0] = 0xf0;
                memcpy(it.data + 1, d + off, len);
            } else {
                memcpy(it.data, d + off, len);
            }
            it.order = (*order)++;
            add_item(s, &it);
            off += len;
            running = -1;
        } else {
            uint32_t n;
            if (st & 0x80) {
                if (st >= 0xf0) return -1;
                running = st;
                off++;
            } else if (running < 0) {
                return -1;
            } else {
                st = (uint8_t)running;
            }
            n = ((st & 0xf0) == 0xc0 || (st & 0xf0) == 0xd0) ? 1 : 2;
            if ((size_t)n > size - off) return -1;
            it.kind = K_MIDI;
            it.len = n + 1;
            it.data = xrealloc(NULL, 3);
            it.data[0] = st;
            memcpy(it.data + 1, d + off, n);
            it.order = (*order)++;
            add_item(s, &it);
            off += n;
        }
    }
    return 0;
}

static int cmp_item(const void *a, const void *b)
{
    const item *x = a, *y = b;
    if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

uint32_t smf_ticks_per_bar(const song *s, const tsig *g) { return s->division * 4u * g->num / g->den; }
uint32_t smf_ticks_per_beat(const song *s, const tsig *g) { return s->division * 4u / g->den; }

static void build_maps(song *s)
{
    size_t i;
    /* tempo segments */
    for (i = 0; i < s->n; i++) {
        const item *it = &s->items[i];
        if (it->kind != K_TEMPO) continue;
        s->segs = xrealloc(s->segs, (s->nsegs + 1) * sizeof *s->segs);
        if (!s->nsegs) {
            s->segs[0].tick = 0;
            s->segs[0].sec = 0.0;
        } else {
            const tseg *p = &s->segs[s->nsegs - 1];
            s->segs[s->nsegs].tick = it->tick;
            s->segs[s->nsegs].sec = p->sec + (double)(it->tick - p->tick) * p->uspq / 1000000.0 / (double)s->division;
        }
        s->segs[s->nsegs++].uspq = (double)it->value;
    }
    if (!s->nsegs) {
        s->segs = xrealloc(NULL, sizeof *s->segs);
        s->segs[0].tick = 0;
        s->segs[0].sec = 0.0;
        s->segs[0].uspq = 500000.0;
        s->nsegs = 1;
    }
    /* time signatures; a change is taken to start a bar */
    s->sigs = xrealloc(NULL, sizeof *s->sigs);
    s->sigs[0].tick = 0; s->sigs[0].bar = 0; s->sigs[0].num = 4; s->sigs[0].den = 4;
    s->nsigs = 1;
    for (i = 0; i < s->n; i++) {
        const item *it = &s->items[i];
        tsig *p = &s->sigs[s->nsigs - 1], g;
        if (it->kind != K_TSIG) continue;
        g.tick = it->tick;
        g.num = (it->value >> 8) ? (it->value >> 8) : 4;
        g.den = 1u << (it->value & 0xff);
        if (!smf_ticks_per_beat(s, &g) || !smf_ticks_per_bar(s, &g)) continue;
        g.bar = p->bar + (it->tick - p->tick) / smf_ticks_per_bar(s, p);
        if (it->tick == p->tick) {
            g.bar = p->bar;
            *p = g;
        } else {
            s->sigs = xrealloc(s->sigs, (s->nsigs + 1) * sizeof *s->sigs);
            s->sigs[s->nsigs++] = g;
        }
    }
}

static void build_notes(song *s)
{
    static uint32_t fifo[16][128][8];
    static uint8_t fifo_n[16][128];
    size_t i;
    int ch;
    memset(fifo_n, 0, sizeof fifo_n);
    s->midi = xrealloc(NULL, s->n * sizeof *s->midi);
    s->list = xrealloc(NULL, s->n * sizeof *s->list);
    for (i = 0; i < s->n; i++) {
        item *it = &s->items[i];
        uint8_t st, c, key;
        if (it->tick > s->last_tick) s->last_tick = it->tick;
        if (!(it->kind == K_MIDI && it->len >= 3 &&
              ((it->data[0] & 0xf0) == 0x80 || ((it->data[0] & 0xf0) == 0x90 && !it->data[2]))))
            s->list[s->nlist++] = (uint32_t)i;
        if (it->kind != K_MIDI) continue;
        s->midi[s->nmidi++] = (uint32_t)i;
        st = it->data[0] & 0xf0;
        c = it->data[0] & 0x0f;
        if ((st != 0x80 && st != 0x90) || it->len < 3) continue;
        key = it->data[1] & 0x7f;
        if (st == 0x90 && it->data[2]) {
            if (fifo_n[c][key] == 8) {          /* too many overlapping: end the oldest */
                s->notes[c][fifo[c][key][0]].off = it->tick;
                memmove(fifo[c][key], fifo[c][key] + 1, 7 * sizeof(uint32_t));
                fifo_n[c][key]--;
            }
            s->notes[c] = xrealloc(s->notes[c], (s->nnotes[c] + 1) * sizeof(note));
            s->notes[c][s->nnotes[c]].on = it->tick;
            s->notes[c][s->nnotes[c]].off = UINT32_MAX;
            s->notes[c][s->nnotes[c]].key = key;
            s->notes[c][s->nnotes[c]].vel = it->data[2];
            fifo[c][key][fifo_n[c][key]++] = (uint32_t)s->nnotes[c]++;
        } else if (fifo_n[c][key]) {
            s->notes[c][fifo[c][key][0]].off = it->tick;
            memmove(fifo[c][key], fifo[c][key] + 1, 7 * sizeof(uint32_t));
            fifo_n[c][key]--;
        }
    }
    for (ch = 0; ch < 16; ch++) {
        size_t k, j = 0;
        for (k = 0; k < s->nnotes[ch]; k++) {
            note *n = &s->notes[ch][k];
            if (n->off == UINT32_MAX) n->off = s->last_tick;
            if (n->off - n->on > s->maxdur[ch]) s->maxdur[ch] = n->off - n->on;
        }
        /* the k-th note-on of a channel (in time order) is the k-th note of that channel */
        for (k = 0; k < s->nmidi; k++) {
            item *it = &s->items[s->midi[k]];
            if ((it->data[0] & 0xf0) == 0x90 && (it->data[0] & 15) == ch && it->len >= 3 && it->data[2]) {
                it->gate = (int32_t)(s->notes[ch][j].off - s->notes[ch][j].on);
                j++;
            }
        }
    }
}

int smf_load(song *s, const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d;
    long size;
    size_t off;
    uint32_t ntrk, t, order = 0;

    memset(s, 0, sizeof *s);
    snprintf(s->path, sizeof s->path, "%s", path);
    if (!f) {
        snprintf(err, errlen, "cannot open %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 14) {
        fclose(f);
        snprintf(err, errlen, "not a Standard MIDI File");
        return -1;
    }
    d = xrealloc(NULL, (size_t)size);
    if (fread(d, 1, (size_t)size, f) != (size_t)size || memcmp(d, "MThd", 4)) {
        fclose(f);
        free(d);
        snprintf(err, errlen, "not a Standard MIDI File");
        return -1;
    }
    fclose(f);
    ntrk = ((uint32_t)d[10] << 8) | d[11];
    s->division = ((uint32_t)d[12] << 8) | d[13];
    if (!ntrk || !s->division || (s->division & 0x8000)) {
        free(d);
        snprintf(err, errlen, "unsupported header (SMPTE time or no tracks)");
        return -1;
    }
    off = 8 + (((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | d[7]);
    for (t = 0; t < ntrk; t++) {
        uint32_t len;
        if (off > (size_t)size || (size_t)size - off < 8 || memcmp(d + off, "MTrk", 4)) break;
        len = ((uint32_t)d[off + 4] << 24) | ((uint32_t)d[off + 5] << 16) | ((uint32_t)d[off + 6] << 8) | d[off + 7];
        if ((size_t)len > (size_t)size - off - 8 ||
            parse_track(s, d, (size_t)size, off + 8, off + 8 + len, &order, t == 0)) {
            free(d);
            smf_free(s);
            snprintf(err, errlen, "malformed track %u", t + 1);
            return -1;
        }
        off += 8 + len;
    }
    free(d);
    qsort(s->items, s->n, sizeof *s->items, cmp_item);
    build_maps(s);
    build_notes(s);
    smf_schedule(s, 44100.0);
    return 0;
}

void smf_free(song *s)
{
    size_t i;
    int c;
    for (i = 0; i < s->n; i++) free(s->items[i].data);
    free(s->items);
    free(s->midi);
    free(s->list);
    free(s->segs);
    free(s->sigs);
    for (c = 0; c < 16; c++) free(s->notes[c]);
    memset(s, 0, sizeof *s);
}

double smf_tick_to_sec(const song *s, uint32_t tick)
{
    size_t i = 0;
    while (i + 1 < s->nsegs && s->segs[i + 1].tick <= tick) i++;
    return s->segs[i].sec + (double)(tick - s->segs[i].tick) * s->segs[i].uspq / 1000000.0 / (double)s->division;
}

double smf_sec_to_tick(const song *s, double sec)
{
    size_t i = 0;
    double t;
    while (i + 1 < s->nsegs && s->segs[i + 1].sec <= sec) i++;
    t = s->segs[i].tick + (sec - s->segs[i].sec) * 1000000.0 * (double)s->division / s->segs[i].uspq;
    return t < 0 ? 0 : t;
}

const tsig *smf_sig_at(const song *s, uint32_t tick)
{
    size_t i = 0;
    while (i + 1 < s->nsigs && s->sigs[i + 1].tick <= tick) i++;
    return &s->sigs[i];
}

void smf_tick_to_mbt(const song *s, uint32_t tick, uint32_t *bar, uint32_t *beat, uint32_t *tk)
{
    const tsig *g = smf_sig_at(s, tick);
    uint32_t dt = tick - g->tick, tpb = smf_ticks_per_bar(s, g), r;
    *bar = g->bar + dt / tpb;
    r = dt % tpb;
    *beat = r / smf_ticks_per_beat(s, g);
    *tk = r % smf_ticks_per_beat(s, g);
}

uint32_t smf_bar_to_tick(const song *s, uint32_t bar)
{
    size_t i = 0;
    while (i + 1 < s->nsigs && s->sigs[i + 1].bar <= bar) i++;
    return s->sigs[i].tick + (bar - s->sigs[i].bar) * smf_ticks_per_bar(s, &s->sigs[i]);
}

void smf_schedule(song *s, double rate)
{
    size_t k;
    uint64_t last_block = 0;
    s->rate = rate;
    for (k = 0; k < s->nmidi; k++) {
        item *it = &s->items[s->midi[k]];
        it->frame = (uint64_t)(smf_tick_to_sec(s, it->tick) * rate);
        if (it->frame / BLOCK > last_block) last_block = it->frame / BLOCK;
    }
    s->end_frame = (last_block + (uint32_t)rate / BLOCK + 1u) * BLOCK;
}
