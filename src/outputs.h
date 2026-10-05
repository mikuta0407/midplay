/* outputs.h -- the places midplay can play to.
 *
 * Two kinds, one per platform each:
 *   synth  renders the sound here and plays it on the default audio device -- an Audio Unit instrument
 *          on macOS ("au:MANU/SUBT"), FluidSynth with a SoundFont on Linux ("sf:/path/to/file.sf2")
 *   MIDI   sends the messages elsewhere -- a CoreMIDI destination on macOS, an ALSA sequencer port on
 *          Linux ("midi:NAME")
 */
#ifndef MIDPLAY_OUTPUTS_H
#define MIDPLAY_OUTPUTS_H
#include <stddef.h>

#include "player.h"

#ifdef __APPLE__
#define SYNTH_PREFIX "au:"
#define SYNTH_KIND   "AU"
#define SYNTH_NOUN   "Audio Unit instruments"
#else
#define SYNTH_PREFIX "sf:"
#define SYNTH_KIND   "SF2"
#define SYNTH_NOUN   "SoundFonts"
#endif

typedef struct {
    int is_synth;
    char name[256];              /* "Apple: DLSMusicDevice", "IAC Driver Bus 1", "FluidSynth: FluidR3_GM.sf2" */
    char spec[256];              /* "au:appl/dls ", "midi:IAC Driver Bus 1", "sf:/usr/share/sounds/sf2/FluidR3_GM.sf2" */
} output_info;

/* every output, synths first; returns the count (at most cap) */
size_t outputs_list(output_info *out, size_t cap);
/* resolve a spec: "au:MANU/SUBT" (four-character codes), "sf:PATH", "au:TEXT", "sf:TEXT" or "midi:TEXT"
 * (part of the name, any case), or a number from outputs_list (1-based).  Returns 0 and fills *found, or -1. */
int outputs_find(const char *spec, output_info *found);
/* the default when nothing is configured: Apple's DLS synth on macOS, FluidR3_GM on Linux (if
 * installed); else the first */
int outputs_default(output_info *found);

/* open an output; null_audio (synth only): drive the synth from a timer thread and discard the audio */
backend *output_open(const output_info *o, int null_audio, char *err, size_t errlen);

/* offline render through a synth, for checking: payload sha256 of interleaved float32 */
int output_render_hash(const output_info *o, song *s, double rate, char hex[65], char *err, size_t errlen);

/* per platform: out_au.c and out_midi.c (macOS), out_fluid.c and out_alsa.c (Linux) */
backend *synth_open(const output_info *o, int null_audio, char *err, size_t errlen);
backend *midi_open(const output_info *o, char *err, size_t errlen);
size_t synth_list(output_info *out, size_t cap);
size_t midi_list(output_info *out, size_t cap);
int synth_render_hash(const output_info *o, song *s, double rate, char hex[65], char *err, size_t errlen);
#ifndef __APPLE__
int synth_from_path(const char *path, output_info *found);   /* a SoundFont that is not in the list */
#endif

#endif
