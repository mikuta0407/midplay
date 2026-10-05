/* out_alsa.c -- an ALSA sequencer port (a hardware port, a software synth such as FluidSynth or
 * TiMidity++, another program's input).  The Linux counterpart of out_midi.c.
 *
 * The clock is a thread that wakes every millisecond; events are sent directly (not queued) when their
 * time has come, so the timing is within about a millisecond plus the receiver's latency.  Pausing sends
 * All Notes Off / All Sound Off on every channel; a seek also resets the controllers before replaying
 * them (player_seek()).
 */
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "outputs.h"

#define SYSEX_MAX 65536

typedef struct {
    backend base;
    snd_seq_t *seq;
    snd_midi_event_t *enc;
    int port;
    pthread_mutex_t lock;        /* the clock thread and the main thread both send */
    player *p;
    pthread_t th;
    _Atomic int quit;
} alsa_backend;

/* alsa-lib reports to stderr, which would scribble over the screen */
static void no_errors(const char *file, int line, const char *fn, int err, const char *fmt, ...)
{
    (void)file; (void)line; (void)fn; (void)err; (void)fmt;
}

static int open_seq(snd_seq_t **seq)
{
    snd_lib_error_set_handler(no_errors);
    if (snd_seq_open(seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) return -1;
    snd_seq_set_client_name(*seq, "midplay");
    return 0;
}

/* the ports others can write to, as "client:port" addresses and "Client name: Port name" names */
typedef void (*port_fn)(void *ctx, int client, int port, const char *name);

static void each_port(snd_seq_t *seq, port_fn fn, void *ctx)
{
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t *pi;
    int self = snd_seq_client_id(seq);
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(seq, ci) >= 0) {
        int c = snd_seq_client_info_get_client(ci);
        if (c == SND_SEQ_CLIENT_SYSTEM || c == self) continue;
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(seq, pi) >= 0) {
            unsigned want = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
            char name[256];
            if ((snd_seq_port_info_get_capability(pi) & want) != want) continue;
            if (snd_seq_port_info_get_capability(pi) & SND_SEQ_PORT_CAP_NO_EXPORT) continue;
            snprintf(name, sizeof name, "%s: %s", snd_seq_client_info_get_name(ci), snd_seq_port_info_get_name(pi));
            fn(ctx, c, snd_seq_port_info_get_port(pi), name);
        }
    }
}

typedef struct { output_info *out; size_t cap, n; } list_ctx;

static void add_port(void *ctx, int client, int port, const char *name)
{
    list_ctx *l = ctx;
    (void)client; (void)port;
    if (l->n >= l->cap) return;
    l->out[l->n].is_synth = 0;
    snprintf(l->out[l->n].name, sizeof l->out[l->n].name, "%s", name);
    snprintf(l->out[l->n].spec, sizeof l->out[l->n].spec, "midi:%s", name);
    l->n++;
}

size_t midi_list(output_info *out, size_t cap)
{
    snd_seq_t *seq;
    list_ctx l = { out, cap, 0 };
    if (open_seq(&seq)) return 0;
    each_port(seq, add_port, &l);
    snd_seq_close(seq);
    return l.n;
}

typedef struct { const char *want; int client, port; } find_ctx;

static void match_port(void *ctx, int client, int port, const char *name)
{
    find_ctx *f = ctx;
    if (f->client < 0 && !strcmp(name, f->want)) {
        f->client = client;
        f->port = port;
    }
}

static int alsa_send(backend *b, const uint8_t *m, uint32_t len)
{
    alsa_backend *ab = (alsa_backend *)b;
    snd_seq_event_t ev;
    int rc = SEND_FAIL;
    pthread_mutex_lock(&ab->lock);
    snd_midi_event_reset_encode(ab->enc);
    snd_seq_ev_clear(&ev);
    if (snd_midi_event_encode(ab->enc, m, (long)len, &ev) == (long)len && ev.type != SND_SEQ_EVENT_NONE) {
        snd_seq_ev_set_source(&ev, ab->port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        if (snd_seq_event_output_direct(ab->seq, &ev) >= 0) rc = SEND_OK;
    }
    pthread_mutex_unlock(&ab->lock);
    return rc;
}

static void all_off(alsa_backend *ab, int reset_controllers)
{
    int c;
    for (c = 0; c < 16; c++) {
        uint8_t m[3] = { (uint8_t)(0xb0 | c), 123, 0 };
        alsa_send(&ab->base, m, 3);
        m[1] = 120;
        alsa_send(&ab->base, m, 3);
        if (reset_controllers) {
            m[1] = 121;
            alsa_send(&ab->base, m, 3);
        }
    }
}

static void alsa_reset(backend *b) { all_off((alsa_backend *)b, 1); }

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_until_ns(uint64_t t)
{
    struct timespec ts = { (time_t)(t / 1000000000ull), (long)(t % 1000000000ull) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
        ;
}

static void *clock_thread(void *arg)
{
    alsa_backend *ab = arg;
    player *p = ab->p;
    uint64_t base_t = 0, base_pos = 0;
    int was_playing = 0;
    while (!atomic_load(&ab->quit)) {
        uint64_t now = now_ns();
        int mode = player_cycle(p);
        if (mode == CYCLE_PLAY) {
            if (!was_playing || p->jumped) {
                base_t = now;
                base_pos = p->pos;
                p->jumped = 0;
            }
            p->pos = base_pos + (uint64_t)((double)(now - base_t) * 1e-9 * ab->base.rate);
            player_dispatch(p);
            player_after(p);
        } else if (was_playing && mode == CYCLE_PAUSE) {
            all_off(ab, 0);
        }
        was_playing = mode == CYCLE_PLAY && atomic_load(&p->playing);
        sleep_until_ns(now + 1000000ull);
    }
    return NULL;
}

static int alsa_start(backend *b, player *p)
{
    alsa_backend *ab = (alsa_backend *)b;
    ab->p = p;
    atomic_store(&ab->quit, 0);
    return pthread_create(&ab->th, NULL, clock_thread, ab) ? -1 : 0;
}

static void alsa_stop(backend *b)
{
    alsa_backend *ab = (alsa_backend *)b;
    if (!ab->p) return;
    atomic_store(&ab->quit, 1);
    pthread_join(ab->th, NULL);
    all_off(ab, 0);
    ab->p = NULL;
}

static void alsa_destroy(backend *b)
{
    alsa_backend *ab = (alsa_backend *)b;
    alsa_stop(b);
    if (ab->seq) {
        snd_seq_drain_output(ab->seq);
        snd_seq_close(ab->seq);
    }
    if (ab->enc) snd_midi_event_free(ab->enc);
    pthread_mutex_destroy(&ab->lock);
    free(ab);
}

backend *midi_open(const output_info *o, char *err, size_t errlen)
{
    alsa_backend *ab = calloc(1, sizeof *ab);
    find_ctx f = { o->name, -1, -1 };
    if (!ab) return NULL;
    pthread_mutex_init(&ab->lock, NULL);
    if (open_seq(&ab->seq)) {
        snprintf(err, errlen, "cannot open the ALSA sequencer");
        free(ab);
        return NULL;
    }
    each_port(ab->seq, match_port, &f);
    if (f.client < 0) {
        snprintf(err, errlen, "MIDI port \"%s\" is not there", o->name);
        alsa_destroy(&ab->base);
        return NULL;
    }
    ab->port = snd_seq_create_simple_port(ab->seq, "midplay out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                          SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    snd_seq_set_output_buffer_size(ab->seq, SYSEX_MAX + 1024);
    if (ab->port < 0 || snd_midi_event_new(SYSEX_MAX, &ab->enc) < 0 ||
        snd_seq_connect_to(ab->seq, ab->port, f.client, f.port) < 0) {
        snprintf(err, errlen, "cannot connect to %s", o->name);
        alsa_destroy(&ab->base);
        return NULL;
    }
    snd_midi_event_no_status(ab->enc, 1);
    snprintf(ab->base.name, sizeof ab->base.name, "%s", o->name);
    snprintf(ab->base.spec, sizeof ab->base.spec, "%s", o->spec);
    ab->base.is_synth = 0;
    ab->base.rate = 44100.0;           /* the clock's unit: frames of a nominal 44.1 kHz */
    ab->base.quantum = 1;
    ab->base.send = alsa_send;
    ab->base.reset = alsa_reset;
    ab->base.start = alsa_start;
    ab->base.stop = alsa_stop;
    ab->base.destroy = alsa_destroy;
    return &ab->base;
}
