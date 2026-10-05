/* midi_sink_alsa.c -- an ALSA sequencer port that logs what it receives, for testing midplay's MIDI
 * output without hardware.  The Linux counterpart of midi_sink.c, with the same output.
 *
 *   build/midi_sink [NAME] > log      (default name "midplay test sink"; stops on SIGINT/SIGTERM)
 *
 * Each event is one line: milliseconds since the first event, then the bytes in hex.
 */
#include <alsa/asoundlib.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>

static volatile sig_atomic_t done;

static void on_signal(int s) { (void)s; done = 1; }

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "midplay test sink";
    snd_seq_t *seq;
    snd_midi_event_t *dec;
    struct pollfd pfd[4];
    int npfd;
    double first = -1;
    unsigned char buf[65536];
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0 ||
        snd_seq_set_client_name(seq, name) < 0 ||
        snd_seq_create_simple_port(seq, name, SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                   SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION) < 0 ||
        snd_midi_event_new(sizeof buf, &dec) < 0) {
        fprintf(stderr, "cannot create the sequencer port\n");
        return 1;
    }
    snd_midi_event_no_status(dec, 1);
    snd_seq_set_input_buffer_size(seq, sizeof buf + 1024);
    npfd = snd_seq_poll_descriptors(seq, pfd, 4, POLLIN);
    fprintf(stderr, "listening\n");
    while (!done) {
        snd_seq_event_t *ev;
        if (poll(pfd, (nfds_t)npfd, 10) <= 0) continue;
        while (snd_seq_event_input(seq, &ev) >= 0) {
            double t = now_ms();
            long n, i;
            snd_midi_event_reset_decode(dec);
            n = snd_midi_event_decode(dec, buf, sizeof buf, ev);
            if (n <= 0) continue;              /* port subscriptions and the like */
            if (first < 0) first = t;
            printf("%.3f", t - first);
            for (i = 0; i < n; i++) printf(" %02x", buf[i]);
            printf("\n");
        }
        fflush(stdout);
    }
    snd_midi_event_free(dec);
    snd_seq_close(seq);
    return 0;
}
