/* names.h -- names for programs, controllers, notes and events */
#ifndef MIDPLAY_NAMES_H
#define MIDPLAY_NAMES_H
#include <stddef.h>
#include <stdint.h>

#include "smf.h"

extern const char *const note_names[12];
/* GM name, XG drum kit (bank MSB 127) or SFX kit (126) */
void voice_name(char *out, size_t n, uint8_t msb, uint8_t prog);
const char *cc_name(int cc);
int is_xg_on(const item *it);
int is_gm_on(const item *it);
int is_gs_reset(const item *it);
/* one line for the event list, e.g. "Note        C4   v100 gate 480" */
void describe(char *out, size_t n, const item *it);

#endif
