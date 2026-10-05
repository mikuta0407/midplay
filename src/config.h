/* config.h -- settings kept between runs, in $MIDPLAY_CONFIG or ~/.config/midplay/config (key=value) */
#ifndef MIDPLAY_CONFIG_H
#define MIDPLAY_CONFIG_H

typedef struct {
    char output[256];            /* an output spec (see outputs.h); empty: the default */
    int autoplay;                /* start playing as soon as a file is opened */
    int exit_at_end;             /* return to the shell when the song ends (else stop and stay) */
    int ascii;                   /* draw with ASCII only */
    char last_dir[1024];         /* where the file browser opens */
} config;

void config_defaults(config *c);
void config_load(config *c);
int config_save(const config *c);
const char *config_path(void);

#endif
