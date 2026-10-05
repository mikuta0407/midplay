/* smf.h -- a Standard MIDI File in memory: events in time order, the tempo map, bars, notes. */
#ifndef MIDPLAY_SMF_H
#define MIDPLAY_SMF_H
#include <stddef.h>
#include <stdint.h>

enum { K_MIDI, K_TEMPO, K_TSIG, K_TEXT };

typedef struct {
    uint32_t tick, order, len, value;
    int32_t gate;                /* note-on: ticks to its note-off; -1 otherwise */
    uint64_t frame;              /* K_MIDI: sample position at the scheduled rate (floored) */
    uint8_t kind, meta;
    uint8_t *data;               /* MIDI bytes (F0.. for system exclusive) or UTF-8 text */
} item;

typedef struct { uint32_t on, off; uint8_t key, vel; } note;
typedef struct { uint32_t tick; double sec, uspq; } tseg;
typedef struct { uint32_t tick, bar, num, den; } tsig;

typedef struct {
    item *items;
    size_t n, cap;
    uint32_t *midi;              /* indices of the K_MIDI items, in time order */
    size_t nmidi;
    uint32_t *list;              /* indices of the items the event list shows (no note-offs) */
    size_t nlist;
    tseg *segs;
    size_t nsegs;
    tsig *sigs;
    size_t nsigs;
    note *notes[16];
    size_t nnotes[16];
    uint32_t maxdur[16];
    uint32_t division, last_tick;
    double rate;                 /* of the last smf_schedule() */
    uint64_t end_frame;          /* last event's 128-frame block + one second, in frames */
    char title[256];
    char path[1024];
} song;

/* load a file; on failure returns -1 and writes a message to err */
int smf_load(song *s, const char *path, char *err, size_t errlen);
void smf_free(song *s);
/* event frames at a sample rate: seconds from the tempo map, the first tempo applying from tick 0,
 * times the rate, floored; the song ends one second after the 128-frame block of its last event. */
void smf_schedule(song *s, double rate);

double smf_tick_to_sec(const song *s, uint32_t tick);
double smf_sec_to_tick(const song *s, double sec);
const tsig *smf_sig_at(const song *s, uint32_t tick);
void smf_tick_to_mbt(const song *s, uint32_t tick, uint32_t *bar, uint32_t *beat, uint32_t *tk);
uint32_t smf_bar_to_tick(const song *s, uint32_t bar);
uint32_t smf_ticks_per_bar(const song *s, const tsig *g);
uint32_t smf_ticks_per_beat(const song *s, const tsig *g);

#endif
