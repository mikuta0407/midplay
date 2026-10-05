/* out_midi.c -- a CoreMIDI destination (a hardware port, the IAC bus, another program's input).
 *
 * The clock is a thread that wakes every millisecond; events are sent "now" (timestamp 0) when their
 * time has come, so the timing is within about a millisecond plus the driver's latency.  Pausing sends
 * All Notes Off / All Sound Off on every channel; a seek also resets the controllers before replaying
 * them (player_seek()).
 */
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "outputs.h"

typedef struct {
    backend base;
    MIDIClientRef client;
    MIDIPortRef port;
    MIDIEndpointRef dest;
    player *p;
    pthread_t th;
    _Atomic int quit;
    mach_timebase_info_data_t tb;
} midi_backend;

static void name_of(MIDIObjectRef o, char *out, size_t n)
{
    CFStringRef s = NULL;
    out[0] = 0;
    if (MIDIObjectGetStringProperty(o, kMIDIPropertyDisplayName, &s) == noErr && s) {
        CFStringGetCString(s, out, (CFIndex)n, kCFStringEncodingUTF8);
        CFRelease(s);
    }
}

size_t midi_list(output_info *out, size_t cap)
{
    ItemCount i, n = MIDIGetNumberOfDestinations();
    size_t k = 0;
    for (i = 0; i < n && k < cap; i++) {
        MIDIEndpointRef d = MIDIGetDestination(i);
        if (!d) continue;
        out[k].is_synth = 0;
        name_of(d, out[k].name, sizeof out[k].name);
        if (!out[k].name[0]) snprintf(out[k].name, sizeof out[k].name, "MIDI destination %lu", (unsigned long)i + 1);
        snprintf(out[k].spec, sizeof out[k].spec, "midi:%s", out[k].name);
        k++;
    }
    return k;
}

static int midi_send(backend *b, const uint8_t *m, uint32_t len)
{
    midi_backend *mb = (midi_backend *)b;
    Byte small[512];
    size_t cap = len + 64 < sizeof small ? sizeof small : len + 64;
    Byte *buf = cap == sizeof small ? small : malloc(cap);
    MIDIPacketList *pl = (MIDIPacketList *)buf;
    MIDIPacket *pk;
    int rc = SEND_FAIL;
    if (!buf) return SEND_FAIL;
    pk = MIDIPacketListInit(pl);
    pk = MIDIPacketListAdd(pl, cap, pk, 0, len, m);
    if (pk && MIDISend(mb->port, mb->dest, pl) == noErr) rc = SEND_OK;
    if (buf != small) free(buf);
    return rc;
}

static void all_off(midi_backend *mb, int reset_controllers)
{
    int c;
    for (c = 0; c < 16; c++) {
        uint8_t m[3] = { (uint8_t)(0xb0 | c), 123, 0 };
        midi_send(&mb->base, m, 3);
        m[1] = 120;
        midi_send(&mb->base, m, 3);
        if (reset_controllers) {
            m[1] = 121;
            midi_send(&mb->base, m, 3);
        }
    }
}

static void midi_reset(backend *b) { all_off((midi_backend *)b, 1); }

static void *clock_thread(void *arg)
{
    midi_backend *mb = arg;
    player *p = mb->p;
    uint64_t ms = 1000000ull * mb->tb.denom / mb->tb.numer, base_t = 0, base_pos = 0;
    int was_playing = 0;
    while (!atomic_load(&mb->quit)) {
        uint64_t now = mach_absolute_time();
        int mode = player_cycle(p);
        if (mode == CYCLE_PLAY) {
            if (!was_playing || p->jumped) {
                base_t = now;
                base_pos = p->pos;
                p->jumped = 0;
            }
            p->pos = base_pos + (uint64_t)((double)(now - base_t) * mb->tb.numer / mb->tb.denom * 1e-9 * mb->base.rate);
            player_dispatch(p);
            player_after(p);
        } else if (was_playing && mode == CYCLE_PAUSE) {
            all_off(mb, 0);
        }
        was_playing = mode == CYCLE_PLAY && atomic_load(&p->playing);
        mach_wait_until(now + ms);
    }
    return NULL;
}

static int midi_start(backend *b, player *p)
{
    midi_backend *mb = (midi_backend *)b;
    mb->p = p;
    atomic_store(&mb->quit, 0);
    return pthread_create(&mb->th, NULL, clock_thread, mb) ? -1 : 0;
}

static void midi_stop(backend *b)
{
    midi_backend *mb = (midi_backend *)b;
    if (!mb->p) return;
    atomic_store(&mb->quit, 1);
    pthread_join(mb->th, NULL);
    all_off(mb, 0);
    mb->p = NULL;
}

static void midi_destroy(backend *b)
{
    midi_backend *mb = (midi_backend *)b;
    midi_stop(b);
    if (mb->port) MIDIPortDispose(mb->port);
    if (mb->client) MIDIClientDispose(mb->client);
    free(mb);
}

backend *midi_open(const output_info *o, char *err, size_t errlen)
{
    midi_backend *mb = calloc(1, sizeof *mb);
    ItemCount i, n = MIDIGetNumberOfDestinations();
    if (!mb) return NULL;
    mach_timebase_info(&mb->tb);
    for (i = 0; i < n; i++) {
        char name[256];
        MIDIEndpointRef d = MIDIGetDestination(i);
        name_of(d, name, sizeof name);
        if (!strcmp(name, o->name)) { mb->dest = d; break; }
    }
    if (!mb->dest) {
        snprintf(err, errlen, "MIDI destination \"%s\" is not there", o->name);
        free(mb);
        return NULL;
    }
    if (MIDIClientCreate(CFSTR("midplay"), NULL, NULL, &mb->client) != noErr ||
        MIDIOutputPortCreate(mb->client, CFSTR("midplay out"), &mb->port) != noErr) {
        snprintf(err, errlen, "cannot create a CoreMIDI client");
        if (mb->client) MIDIClientDispose(mb->client);
        free(mb);
        return NULL;
    }
    snprintf(mb->base.name, sizeof mb->base.name, "%s", o->name);
    snprintf(mb->base.spec, sizeof mb->base.spec, "%s", o->spec);
    mb->base.is_synth = 0;
    mb->base.rate = 44100.0;           /* the clock's unit: frames of a nominal 44.1 kHz */
    mb->base.quantum = 1;
    mb->base.send = midi_send;
    mb->base.reset = midi_reset;
    mb->base.start = midi_start;
    mb->base.stop = midi_stop;
    mb->base.destroy = midi_destroy;
    return &mb->base;
}
