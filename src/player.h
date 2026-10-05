/* player.h -- the transport shared by the outputs.
 *
 * An output ("backend") owns a thread that drives the clock: the audio callback of an Audio Unit, or a
 * 1 ms timer for a MIDI destination.  On that thread it calls player_cycle() and player_dispatch() and
 * advances `pos` (frames at the backend's rate).  The main thread only touches the atomics: play/pause,
 * mutes, and seeks (player_seek(): the backend stops sending, the main thread resets the output and
 * replays the controllers before the target, then the backend jumps there).
 */
#ifndef MIDPLAY_PLAYER_H
#define MIDPLAY_PLAYER_H
#include <stdatomic.h>
#include <stdint.h>

#include "smf.h"

typedef struct player player;
typedef struct backend backend;

enum { SEND_OK = 0, SEND_BUSY = 1, SEND_FAIL = -1 };

struct backend {
    char name[256];              /* shown to the user, e.g. "Apple: DLSMusicDevice" */
    char spec[256];              /* how to ask for it again, e.g. "au:appl/dls " */
    int is_au;
    double rate;                 /* the clock's rate, frames per second */
    uint32_t quantum;            /* events are sent at multiples of this many frames (128 for an AU) */
    int (*send)(backend *b, const uint8_t *m, uint32_t len);   /* any thread */
    void (*reset)(backend *b);                                  /* main thread: back to power-on / all off */
    int (*start)(backend *b, player *p);                        /* start the clock thread */
    void (*stop)(backend *b);                                   /* stop it (and silence) */
    void (*destroy)(backend *b);
};

enum { CYCLE_PLAY, CYCLE_PAUSE, CYCLE_SEEK };

struct player {
    song *s;
    backend *b;
    /* owned by the clock thread */
    uint64_t pos;
    size_t next;
    int fails;
    int jumped;                  /* player_cycle() just moved pos (a seek) */
    /* shared */
    _Atomic uint64_t pub_pos;
    _Atomic int playing, finished, seek_state;
    _Atomic uint64_t seek_pos, seek_next;
    _Atomic uint32_t mute_mask, late, dropped, skipped;
    _Atomic uint32_t peak_l, peak_r, load;   /* float bits; maximum since the view last took them */
};

/* main thread, with the backend stopped: bind a song (schedules it at the backend's rate) */
void player_bind(player *p, song *s, backend *b);
/* clock thread */
int player_cycle(player *p);             /* CYCLE_*: handles the seek hand-over */
void player_dispatch(player *p);         /* send the events due at p->pos */
void player_after(player *p);            /* publish pos, detect the end */
void player_max_float(_Atomic uint32_t *a, float v);
/* main thread */
float player_take_float(_Atomic uint32_t *a);
void player_seek(player *p, uint32_t tick);
void player_set_mutes(player *p, uint32_t mask);
uint32_t player_tick(const player *p);   /* the tick at the published position */

#endif
