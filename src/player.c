/* player.c -- see player.h */
#include "player.h"

#include <string.h>
#include <unistd.h>

#define MAX_RETRIES 8            /* cycles an event may wait for room in an Audio Unit's MIDI queue */

void player_bind(player *p, song *s, backend *b)
{
    p->s = s;
    p->b = b;
    p->pos = 0;
    p->next = 0;
    p->fails = 0;
    p->jumped = 0;
    smf_schedule(s, b->rate);
    atomic_store(&p->pub_pos, 0);
    atomic_store(&p->playing, 0);
    atomic_store(&p->finished, 0);
    atomic_store(&p->seek_state, 0);
    atomic_store(&p->late, 0);
    atomic_store(&p->dropped, 0);
    atomic_store(&p->skipped, 0);
}

void player_max_float(_Atomic uint32_t *a, float v)
{
    uint32_t bits, old;
    if (!(v > 0.0f)) return;
    memcpy(&bits, &v, 4);
    old = atomic_load_explicit(a, memory_order_relaxed);
    while (bits > old &&
           !atomic_compare_exchange_weak_explicit(a, &old, bits, memory_order_relaxed, memory_order_relaxed))
        ;
}

float player_take_float(_Atomic uint32_t *a)
{
    uint32_t bits = atomic_exchange_explicit(a, 0, memory_order_relaxed);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

int player_cycle(player *p)
{
    int ss = atomic_load(&p->seek_state);
    if (ss == 1) {
        atomic_store(&p->seek_state, 2);
        return CYCLE_SEEK;
    }
    if (ss == 2) return CYCLE_SEEK;
    if (ss == 3) {
        p->pos = atomic_load(&p->seek_pos);
        p->next = (size_t)atomic_load(&p->seek_next);
        p->fails = 0;
        p->jumped = 1;
        atomic_store(&p->pub_pos, p->pos);
        atomic_store(&p->seek_state, 0);
    }
    return atomic_load(&p->playing) ? CYCLE_PLAY : CYCLE_PAUSE;
}

static int is_raw_packet(uint8_t st) { return st < 0x80 || (st >= 0xf0 && st != 0xf0); }

void player_dispatch(player *p)
{
    const song *s = p->s;
    uint32_t mute = atomic_load_explicit(&p->mute_mask, memory_order_relaxed), q = p->b->quantum;
    while (p->next < s->nmidi) {
        const item *it = &s->items[s->midi[p->next]];
        uint8_t st = it->data[0];
        int rc;
        if (it->frame / q * q > p->pos) break;
        if (is_raw_packet(st)) {          /* an F7 escape packet: no output takes raw bytes */
            atomic_fetch_add_explicit(&p->skipped, 1, memory_order_relaxed);
            p->next++;
            continue;
        }
        if ((st & 0xf0) == 0x90 && it->len >= 3 && it->data[2] && ((mute >> (st & 15)) & 1u)) {
            p->next++;
            continue;
        }
        rc = p->b->send(p->b, it->data, it->len);
        if (rc == SEND_OK) {
            p->next++;
            p->fails = 0;
            continue;
        }
        if (rc != SEND_BUSY || ++p->fails > MAX_RETRIES) {
            atomic_fetch_add_explicit(&p->dropped, 1, memory_order_relaxed);
            p->next++;
            p->fails = 0;
            continue;
        }
        if (p->fails == 1) atomic_fetch_add_explicit(&p->late, 1, memory_order_relaxed);
        break;                            /* the queue is full: retry next cycle */
    }
}

void player_after(player *p)
{
    atomic_store(&p->pub_pos, p->pos);
    if (p->next >= p->s->nmidi && p->pos >= p->s->end_frame) {
        atomic_store(&p->finished, 1);
        atomic_store(&p->playing, 0);
    }
}

static int wait_state(player *p, int want)
{
    int i;
    for (i = 0; i < 3000; i++) {
        if (atomic_load(&p->seek_state) == want) return 0;
        usleep(1000);
    }
    return -1;
}

static void send_retry(player *p, const item *it)
{
    int tries;
    for (tries = 0; tries < 500; tries++) {
        if (p->b->send(p->b, it->data, it->len) != SEND_BUSY) return;
        usleep(1000);
    }
}

/* Controllers whose last value is the whole state; everything else (system exclusive, RPN/NRPN and
 * data entry, which are sequences) is replayed in full. */
static int chase_key(const uint8_t *m, uint32_t len)
{
    uint8_t st = m[0] & 0xf0, ch = m[0] & 15;
    if (st == 0xb0 && len >= 3) {
        uint8_t cc = m[1] & 127;
        if (cc == 6 || cc == 38 || (cc >= 96 && cc <= 101)) return -1;
        return ch * 128 + cc;
    }
    if (st == 0xc0) return 2048 + ch;
    if (st == 0xd0) return 2064 + ch;
    if (st == 0xe0) return 2080 + ch;
    return -1;
}

void player_seek(player *p, uint32_t tick)
{
    const song *s = p->s;
    uint32_t q = p->b->quantum;
    uint64_t frame = (uint64_t)(smf_tick_to_sec(s, tick) * p->b->rate) / q * q;
    static size_t last[2096];
    size_t k, end;
    atomic_store(&p->seek_state, 1);
    if (wait_state(p, 2)) {               /* the clock thread did not answer */
        atomic_store(&p->seek_state, 0);
        return;
    }
    p->b->reset(p->b);
    for (end = 0; end < s->nmidi && s->items[s->midi[end]].frame / q * q < frame; end++)
        ;
    for (k = 0; k < sizeof last / sizeof last[0]; k++) last[k] = (size_t)-1;
    for (k = 0; k < end; k++) {
        const item *it = &s->items[s->midi[k]];
        int key = chase_key(it->data, it->len);
        if (key >= 0) last[key] = k;
    }
    for (k = 0; k < end; k++) {
        const item *it = &s->items[s->midi[k]];
        uint8_t st = it->data[0];
        int key;
        if (is_raw_packet(st) || (st & 0xe0) == 0x80 || (st & 0xf0) == 0xa0) continue;   /* notes */
        key = chase_key(it->data, it->len);
        if (key >= 0 && last[key] != k) continue;
        send_retry(p, it);
    }
    atomic_store(&p->seek_pos, frame);
    atomic_store(&p->seek_next, (uint64_t)end);
    atomic_store(&p->finished, 0);
    atomic_store(&p->seek_state, 3);
    wait_state(p, 0);
}

void player_set_mutes(player *p, uint32_t mask)
{
    uint32_t before = atomic_exchange(&p->mute_mask, mask), now_muted = mask & ~before;
    int c;
    for (c = 0; c < 16; c++)              /* release what is sounding on a newly muted part */
        if ((now_muted >> c) & 1) {
            uint8_t m[3] = { (uint8_t)(0xb0 | c), 123, 0 };
            p->b->send(p->b, m, 3);
        }
}

uint32_t player_tick(const player *p)
{
    uint64_t pos = atomic_load(&((player *)p)->pub_pos);
    return (uint32_t)smf_sec_to_tick(p->s, (double)pos / p->b->rate);
}
