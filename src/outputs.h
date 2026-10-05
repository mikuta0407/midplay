/* outputs.h -- the places midplay can play to: Audio Unit instruments (played on the default audio
 * device) and CoreMIDI destinations. */
#ifndef MIDPLAY_OUTPUTS_H
#define MIDPLAY_OUTPUTS_H
#include <stddef.h>

#include "player.h"

typedef struct {
    int is_au;
    char name[256];              /* "Apple: DLSMusicDevice", "IAC Driver Bus 1" */
    char spec[256];              /* "au:appl/dls ", "midi:IAC Driver Bus 1" */
} output_info;

/* every output, AU instruments first; returns the count (at most cap) */
size_t outputs_list(output_info *out, size_t cap);
/* resolve a spec: "au:MANU/SUBT" (four-character codes), "au:TEXT" or "midi:TEXT" (part of the name,
 * any case), or a number from outputs_list (1-based).  Returns 0 and fills *found, or -1. */
int outputs_find(const char *spec, output_info *found);
/* the default when nothing is configured: Apple's DLS synth, else the first */
int outputs_default(output_info *found);

/* open an output; null_audio (AU only): drive the unit from a timer thread and discard the audio */
backend *output_open(const output_info *o, int null_audio, char *err, size_t errlen);

/* offline render through an Audio Unit, for checking: payload sha256 of interleaved float32 */
int output_render_hash(const output_info *o, song *s, double rate, char hex[65], char *err, size_t errlen);

backend *au_open(const output_info *o, int null_audio, char *err, size_t errlen);
backend *midi_open(const output_info *o, char *err, size_t errlen);
size_t au_list(output_info *out, size_t cap);
size_t midi_list(output_info *out, size_t cap);
int au_render_hash(const output_info *o, song *s, double rate, char hex[65], char *err, size_t errlen);

#endif
