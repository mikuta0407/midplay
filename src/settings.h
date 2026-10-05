/* settings.h -- the settings screen: output, autoplay, what happens at the end, ASCII drawing */
#ifndef MIDPLAY_SETTINGS_H
#define MIDPLAY_SETTINGS_H

#include "config.h"
#include "outputs.h"
#include "term.h"

typedef struct {
    int sel;
    int picking;                 /* choosing an output */
    output_info outs[256];
    size_t nouts;
    int osel;
    char msg[256];
} settings_ui;

enum { SET_NONE, SET_CLOSE, SET_OUTPUT, SET_CHANGED };

void settings_enter(settings_ui *s, const char *current_spec);
/* SET_OUTPUT: *chosen is the output picked; SET_CHANGED: cfg was edited (save it) */
int settings_key(settings_ui *s, config *cfg, int key, output_info *chosen);
void settings_draw(sbuf *sb, settings_ui *s, const config *cfg, const char *current_name, int W, int H);

#endif
