/* midplay -- a terminal MIDI file player for macOS and Linux.
 *
 *   midplay [options] [FILE.mid | DIRECTORY]   play a file, or browse (the last directory, or here)
 *   midplay list                               the outputs: synths (Audio Units / SoundFonts) and MIDI ports
 *   midplay hash [--rate HZ] [-o OUT] FILE     render through a synth offline, print sha256
 *
 * options:
 *   -o, --output OUT    au:MANU/SUBT, au:NAME (macOS), sf:PATH, sf:NAME (Linux), midi:NAME (part of the
 *                       name) or a number from `midplay list`
 *   --autoplay / --no-autoplay     start playing when a file is opened (default: the setting, else yes)
 *   --exit-at-end / --stay         at the end of the song quit to the shell / stop and stay
 *   --ascii             draw with ASCII only
 *   --null-audio        (testing) drive the synth from a timer and discard the sound
 *   --version
 *
 * Settings changed on the settings screen (c) are kept in ~/.config/midplay/config ($MIDPLAY_CONFIG);
 * command-line options apply to this run only.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "browser.h"
#include "config.h"
#include "outputs.h"
#include "player.h"
#include "settings.h"
#include "smf.h"
#include "term.h"
#include "view.h"

#define MIDPLAY_VERSION "0.1.0"

enum { SCR_BROWSER, SCR_PLAYER, SCR_SETTINGS };

typedef struct {
    config cfg;                  /* as saved */
    config run;                  /* in force: cfg with the command line on top */
    int null_audio;
    output_info out;
    backend *b;
    song s;
    int have_song;
    player p;
    viewstate v;
    playui ui;
    browser br;
    settings_ui st;
    int screen, back;
    int quit;
    sbuf sb;
} app;

static void usage(FILE *f)
{
    fprintf(f,
            "usage: midplay [options] [FILE.mid | DIRECTORY]\n"
            "       midplay list\n"
            "       midplay hash [--rate HZ] [-o OUT] FILE.mid\n"
            "options:\n"
#ifdef __APPLE__
            "  -o, --output OUT   au:MANU/SUBT | au:NAME | midi:NAME | N (see `midplay list`)\n"
#else
            "  -o, --output OUT   sf:PATH | sf:NAME | midi:NAME | N (see `midplay list`)\n"
#endif
            "  --autoplay, --no-autoplay   start playing when a file is opened\n"
            "  --exit-at-end, --stay       at the end: quit to the shell / stop and stay\n"
            "  --ascii            ASCII-only drawing\n"
            "  --null-audio       (testing) synth driven by a timer, sound discarded\n"
            "  --version          print the version\n"
            "settings: %s\n",
            config_path());
}

static int cmd_list(void)
{
    output_info all[256];
    size_t n = outputs_list(all, 256), i;
    if (!n) {
        printf("no " SYNTH_NOUN " and no MIDI destinations\n");
        return 1;
    }
    for (i = 0; i < n; i++)
        printf("%3zu. [%-4s] %-44s %s\n", i + 1, all[i].is_synth ? SYNTH_KIND : "MIDI", all[i].name, all[i].spec);
    return 0;
}

static int resolve_output(const char *spec, output_info *o)
{
    if (spec && *spec) {
        if (outputs_find(spec, o) == 0) return 0;
        fprintf(stderr, "midplay: no output matches \"%s\" (see `midplay list`)\n", spec);
        return -1;
    }
    if (outputs_default(o) == 0) return 0;
    fprintf(stderr, "midplay: no " SYNTH_NOUN " and no MIDI destinations\n");
    return -1;
}

static int cmd_hash(int argc, char **argv)
{
    const char *spec = NULL, *path = NULL;
    double rate = 44100.0;
    output_info o;
    song s;
    char err[512], hex[65];
    int i;
    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atof(argv[++i]);
        else if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) && i + 1 < argc) spec = argv[++i];
        else path = argv[i];
    }
    if (!path) { usage(stderr); return 2; }
    if (resolve_output(spec, &o)) return 1;
    if (smf_load(&s, path, err, sizeof err)) { fprintf(stderr, "midplay: %s: %s\n", path, err); return 1; }
    smf_schedule(&s, rate);
    if (output_render_hash(&o, &s, rate, hex, err, sizeof err)) { fprintf(stderr, "midplay: %s\n", err); return 1; }
    printf("%s  %llu frames at %.0f Hz through %s, payload sha256 %s\n", path,
           (unsigned long long)s.end_frame, rate, o.name, hex);
    smf_free(&s);
    return 0;
}

/* ---- the running program ---- */
static void set_status(app *a, const char *msg)
{
    snprintf(a->ui.status, sizeof a->ui.status, "%s", msg);
}

static int start_song(app *a)
{
    player_bind(&a->p, &a->s, a->b);
    player_set_mutes(&a->p, view_mute_mask(&a->ui));
    if (a->b->start(a->b, &a->p)) return -1;
    return 0;
}

static void open_song(app *a, const char *path)
{
    song s;
    char err[512];
    if (smf_load(&s, path, err, sizeof err)) {
        snprintf(a->br.msg, sizeof a->br.msg, "%s", err);
        set_status(a, err);
        return;
    }
    if (a->have_song) {
        a->b->stop(a->b);
        smf_free(&a->s);
    }
    a->b->reset(a->b);
    a->s = s;
    a->have_song = 1;
    memset(&a->ui, 0, sizeof a->ui);
    view_reset(&a->v);
    if (start_song(a)) {
        set_status(a, "cannot start the output");
    } else {
        atomic_store(&a->p.playing, a->run.autoplay);
        if (!a->run.autoplay) set_status(a, "Space: play");
    }
    a->screen = SCR_PLAYER;
    {
        char dir[PATH_MAX], real[PATH_MAX];
        const char *slash = strrchr(path, '/');
        if (slash) {
            snprintf(dir, sizeof dir, "%.*s", (int)(slash - path), path);
            if (realpath(dir, real) && strlen(real) < sizeof a->cfg.last_dir) {
                memcpy(a->cfg.last_dir, real, strlen(real) + 1);
                config_save(&a->cfg);
            }
        }
    }
}

static int switch_output(app *a, const output_info *o)
{
    char err[512];
    uint32_t tick = a->have_song ? player_tick(&a->p) : 0;
    int was_playing = a->have_song && atomic_load(&a->p.playing);
    backend *nb = output_open(o, a->null_audio, err, sizeof err);
    if (!nb) {
        snprintf(a->st.msg, sizeof a->st.msg, "%s", err);
        return -1;
    }
    if (a->b) {
        a->b->stop(a->b);
        a->b->destroy(a->b);
    }
    a->b = nb;
    a->out = *o;
    snprintf(a->st.msg, sizeof a->st.msg, "now playing to %s", o->name);
    if (a->have_song) {
        if (start_song(a)) {
            snprintf(a->st.msg, sizeof a->st.msg, "cannot start %s", o->name);
            return 0;
        }
        if (tick) player_seek(&a->p, tick);
        atomic_store(&a->p.playing, was_playing);
    }
    return 0;
}

static void player_keys(app *a, int key)
{
    player *p = &a->p;
    uint32_t tick = player_tick(p), bar, beat, tk;
    int jump = 0;
    a->ui.status[0] = 0;
    smf_tick_to_mbt(&a->s, tick, &bar, &beat, &tk);
    switch (key) {
    case KEY_UP: a->ui.sel = (a->ui.sel + 15) % 16; break;
    case KEY_DOWN: a->ui.sel = (a->ui.sel + 1) % 16; break;
    case KEY_RIGHT: jump = 1; break;
    case KEY_LEFT: jump = -1; break;
    case '.': jump = 8; break;
    case ',': jump = -8; break;
    case ' ':
        if (atomic_load(&p->finished)) {
            player_seek(p, 0);
            atomic_store(&p->playing, 1);
        } else {
            atomic_store(&p->playing, !atomic_load(&p->playing));
        }
        break;
    case 'm': a->ui.mute ^= 1u << a->ui.sel; player_set_mutes(p, view_mute_mask(&a->ui)); break;
    case 's': a->ui.solo ^= 1u << a->ui.sel; player_set_mutes(p, view_mute_mask(&a->ui)); break;
    case 'u': a->ui.mute = a->ui.solo = 0; player_set_mutes(p, 0); break;
    case 'r': player_seek(p, 0); break;
    case 'o': a->screen = SCR_BROWSER; break;
    default: break;
    }
    if (jump) {
        int64_t target = (int64_t)bar + jump;
        if (jump < 0 && (beat || tk >= a->s.division / 2)) target++;   /* first back to this bar's start */
        if (target < 0) target = 0;
        if (smf_bar_to_tick(&a->s, (uint32_t)target) <= a->s.last_tick) player_seek(p, smf_bar_to_tick(&a->s, (uint32_t)target));
    }
}

static void run(app *a)
{
    int W, H;
    while (!a->quit && !term_quit) {
        int key;
        term_size(&W, &H);
        a->sb.ascii = a->run.ascii;
        sb_begin(&a->sb);
        if (a->screen == SCR_PLAYER && a->have_song) {
            view_draw(&a->sb, &a->p, &a->v, &a->ui, W, H);
        } else if (a->screen == SCR_SETTINGS) {
            settings_draw(&a->sb, &a->st, &a->run, a->out.name, W, H);
        } else {
            const char *playing = NULL;
            if (a->have_song) playing = strrchr(a->s.path, '/') ? strrchr(a->s.path, '/') + 1 : a->s.path;
            browser_draw(&a->sb, &a->br, W, H, a->out.name, playing);
        }
        sb_flush(&a->sb);

        if (a->have_song && atomic_load(&a->p.finished) && a->run.exit_at_end) break;
        key = term_key(33);
        if (key == KEY_NONE) continue;
        if (key == 'q' || key == 'Q') break;
        if (a->screen == SCR_SETTINGS) {
            output_info chosen;
            int r = settings_key(&a->st, &a->cfg, key, &chosen);
            if (r == SET_CLOSE) a->screen = a->back;
            else if (r == SET_OUTPUT && switch_output(a, &chosen) == 0) {
                snprintf(a->run.output, sizeof a->run.output, "%s", chosen.spec);
                snprintf(a->cfg.output, sizeof a->cfg.output, "%s", chosen.spec);
                config_save(&a->cfg);
            }
            else if (r == SET_CHANGED) {
                a->run.autoplay = a->cfg.autoplay;
                a->run.exit_at_end = a->cfg.exit_at_end;
                a->run.ascii = a->cfg.ascii;
                if (config_save(&a->cfg)) snprintf(a->st.msg, sizeof a->st.msg, "could not save");
            }
            continue;
        }
        if (key == 'c') {
            a->back = a->screen;
            settings_enter(&a->st, a->out.spec);
            a->screen = SCR_SETTINGS;
            continue;
        }
        if (a->screen == SCR_PLAYER && a->have_song) {
            player_keys(a, key);
        } else {
            char path[2048];
            int r = browser_key(&a->br, key, H - 4, path, sizeof path);
            if (r == BROWSE_FILE) open_song(a, path);
            else if (r == BROWSE_BACK && a->have_song) a->screen = SCR_PLAYER;
        }
    }
}

int main(int argc, char **argv)
{
    static app a;
    const char *spec = NULL, *path = NULL;
    int i, autoplay = -1, exit_at_end = -1, ascii = 0;
    char err[512];
    struct stat st;

    if (argc > 1 && !strcmp(argv[1], "list")) return cmd_list();
    if (argc > 1 && !strcmp(argv[1], "hash")) return cmd_hash(argc - 2, argv + 2);
    for (i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) && i + 1 < argc) spec = argv[++i];
        else if (!strcmp(argv[i], "--autoplay")) autoplay = 1;
        else if (!strcmp(argv[i], "--no-autoplay")) autoplay = 0;
        else if (!strcmp(argv[i], "--exit-at-end")) exit_at_end = 1;
        else if (!strcmp(argv[i], "--stay")) exit_at_end = 0;
        else if (!strcmp(argv[i], "--ascii")) ascii = 1;
        else if (!strcmp(argv[i], "--null-audio")) a.null_audio = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(stdout); return 0; }
        else if (!strcmp(argv[i], "--version")) { printf("midplay %s\n", MIDPLAY_VERSION); return 0; }
        else if (argv[i][0] == '-') { usage(stderr); return 2; }
        else path = argv[i];
    }
    config_load(&a.cfg);
    a.run = a.cfg;
    if (autoplay >= 0) a.run.autoplay = autoplay;
    if (exit_at_end >= 0) a.run.exit_at_end = exit_at_end;
    if (ascii) a.run.ascii = 1;
    if (spec) snprintf(a.run.output, sizeof a.run.output, "%s", spec);

    if (resolve_output(a.run.output, &a.out)) {
        if (spec || !a.run.output[0]) return 1;
        fprintf(stderr, "midplay: the saved output is not there; using the default\n");
        if (resolve_output(NULL, &a.out)) return 1;
    }
    a.b = output_open(&a.out, a.null_audio, err, sizeof err);
    if (!a.b) {
        fprintf(stderr, "midplay: %s\n", err);
        return 1;
    }
    if (term_init()) {
        fprintf(stderr, "midplay: needs a terminal (or use `midplay hash`)\n");
        return 2;
    }
    view_reset(&a.v);
    a.screen = SCR_BROWSER;
    if (path && stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) {
        const char *slash = strrchr(path, '/');
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - path) : 1, slash ? path : ".");
        if (browser_open(&a.br, dir)) browser_open(&a.br, ".");
        open_song(&a, path);
    } else {
        const char *dir = path ? path : a.cfg.last_dir[0] ? a.cfg.last_dir : ".";
        if (browser_open(&a.br, dir) && browser_open(&a.br, ".")) {
            term_restore();
            fprintf(stderr, "midplay: cannot list %s\n", dir);
            return 1;
        }
    }
    run(&a);
    if (a.have_song) a.b->stop(a.b);
    a.b->destroy(a.b);
    term_restore();
    if (a.have_song) smf_free(&a.s);
    browser_free(&a.br);
    return 0;
}
