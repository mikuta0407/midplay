/* midi_sink.c -- a virtual CoreMIDI destination that logs what it receives, for testing midplay's MIDI
 * output without hardware.
 *
 *   build/midi_sink [NAME] > log      (default name "midplay test sink"; stops on SIGINT/SIGTERM)
 *
 * Each packet is one line: milliseconds since the first packet, then the bytes in hex.  A packet may
 * hold several messages (tests/check_midi.py splits them).
 */
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t done;
static uint64_t first;
static mach_timebase_info_data_t tb;

static void on_signal(int s) { (void)s; done = 1; }

static void read_proc(const MIDIPacketList *pl, void *ref, void *src)
{
    const MIDIPacket *p = &pl->packet[0];
    UInt32 i, j;
    uint64_t now = mach_absolute_time();
    (void)ref; (void)src;
    if (!first) first = now;
    for (i = 0; i < pl->numPackets; i++) {
        printf("%.3f", (double)(now - first) * tb.numer / tb.denom / 1e6);
        for (j = 0; j < p->length; j++) printf(" %02x", p->data[j]);
        printf("\n");
        p = MIDIPacketNext(p);
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    MIDIClientRef client;
    MIDIEndpointRef dest;
    CFStringRef name = CFStringCreateWithCString(NULL, argc > 1 ? argv[1] : "midplay test sink", kCFStringEncodingUTF8);
    mach_timebase_info(&tb);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (MIDIClientCreate(CFSTR("midi_sink"), NULL, NULL, &client) != noErr ||
        MIDIDestinationCreate(client, name, read_proc, NULL, &dest) != noErr) {
        fprintf(stderr, "cannot create the virtual destination\n");
        return 1;
    }
    fprintf(stderr, "listening\n");
    while (!done) usleep(10000);
    MIDIEndpointDispose(dest);
    MIDIClientDispose(client);
    return 0;
}
