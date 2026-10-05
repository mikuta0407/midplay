/* out_fluid.c -- FluidSynth with a SoundFont, played on the default audio device.  The Linux
 * counterpart of out_au.c.
 *
 * The audio driver's callback is the clock.  The synth is rendered in pieces that never cross a 64-frame
 * boundary (FluidSynth's own block) and each block's events are sent at its start, so `midplay hash`
 * gives the same result on every run.  Paused, the synth is not run at all, so playing resumes seamlessly.
 *
 * SoundFonts (*.sf2, *.sf3) are looked for in $MIDPLAY_SOUNDFONTS (directories or files, separated by
 * ':'), $XDG_DATA_HOME/soundfonts (~/.local/share/soundfonts), /usr/local/share/soundfonts,
 * /usr/share/soundfonts, /usr/share/sounds/sf2 and /usr/share/sounds/sf3.  The audio driver is
 * $MIDPLAY_AUDIO_DRIVER if set (any of FluidSynth's: pipewire, pulseaudio, alsa, jack, ...), else the
 * first of pipewire, pulseaudio and alsa that opens.  The sample rate is 44100 Hz, or $MIDPLAY_RATE.
 */
#include <dirent.h>
#include <fcntl.h>
#include <fluidsynth.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "outputs.h"
#include "sha256.h"

#define BLOCK 64u

typedef struct {
    backend base;
    fluid_settings_t *settings;
    fluid_synth_t *synth;
    fluid_audio_driver_t *drv;
    pthread_mutex_t lock;        /* held by the audio callback while it renders */
    player *p;
    int null_audio;
    pthread_t th;
    _Atomic int quit;
} fluid_backend;

/* FluidSynth logs to stderr, which would scribble over the screen: keep the last error for messages */
static char last_error[256];

static void on_log(int level, const char *msg, void *data)
{
    (void)level; (void)data;
    snprintf(last_error, sizeof last_error, "%s", msg);
}

/* and some of the libraries under it (libinstpatch, PipeWire) write to stderr directly: point it at
 * /dev/null while they are being set up */
static int hush(void)
{
    int saved, nul;
    fflush(stderr);
    if ((saved = dup(STDERR_FILENO)) < 0) return -1;
    if ((nul = open("/dev/null", O_WRONLY)) >= 0) {
        dup2(nul, STDERR_FILENO);
        close(nul);
    }
    return saved;
}

static void unhush(int saved)
{
    if (saved < 0) return;
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
}

static void quiet(void)
{
    fluid_set_log_function(FLUID_PANIC, on_log, NULL);
    fluid_set_log_function(FLUID_ERR, on_log, NULL);
    fluid_set_log_function(FLUID_WARN, NULL, NULL);
    fluid_set_log_function(FLUID_INFO, NULL, NULL);
    fluid_set_log_function(FLUID_DBG, NULL, NULL);
}

/* ---- finding SoundFonts ---- */
static int is_soundfont(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && (!strcasecmp(name + n - 4, ".sf2") || !strcasecmp(name + n - 4, ".sf3"));
}

static void add_file(output_info *out, size_t cap, size_t *n, const char *path)
{
    char real[PATH_MAX];
    const char *base;
    struct stat st;
    size_t i;
    if (*n >= cap || !realpath(path, real) || stat(real, &st) || !S_ISREG(st.st_mode)) return;
    for (i = 0; i < *n; i++)                   /* default-GM.sf2 and friends are links to another one */
        if (!strcmp(out[i].spec + 3, real)) return;
    base = strrchr(real, '/') ? strrchr(real, '/') + 1 : real;
    out[*n].is_synth = 1;
    snprintf(out[*n].name, sizeof out[*n].name, "FluidSynth: %s", base);
    snprintf(out[*n].spec, sizeof out[*n].spec, "sf:%s", real);
    (*n)++;
}

static int by_name(const struct dirent **a, const struct dirent **b) { return strcmp((*a)->d_name, (*b)->d_name); }

static void add_path(output_info *out, size_t cap, size_t *n, const char *path)
{
    struct dirent **ents;
    struct stat st;
    int k, m;
    if (stat(path, &st)) return;
    if (!S_ISDIR(st.st_mode)) {
        add_file(out, cap, n, path);
        return;
    }
    if ((m = scandir(path, &ents, NULL, by_name)) < 0) return;
    for (k = 0; k < m; k++) {
        char full[PATH_MAX];
        if (is_soundfont(ents[k]->d_name)) {
            snprintf(full, sizeof full, "%s/%s", path, ents[k]->d_name);
            add_file(out, cap, n, full);
        }
        free(ents[k]);
    }
    free(ents);
}

size_t synth_list(output_info *out, size_t cap)
{
    static const char *const sys[] = { "/usr/local/share/soundfonts", "/usr/share/soundfonts", "/usr/share/sounds/sf2",
                                       "/usr/share/sounds/sf3" };
    const char *env = getenv("MIDPLAY_SOUNDFONTS"), *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    char buf[PATH_MAX];
    size_t n = 0, i;
    if (env && *env) {
        char *list = strdup(env), *save = NULL, *tok;
        for (tok = list ? strtok_r(list, ":", &save) : NULL; tok; tok = strtok_r(NULL, ":", &save))
            add_path(out, cap, &n, tok);
        free(list);
    }
    if (xdg && *xdg) snprintf(buf, sizeof buf, "%s/soundfonts", xdg);
    else snprintf(buf, sizeof buf, "%s/.local/share/soundfonts", home ? home : "");
    add_path(out, cap, &n, buf);
    for (i = 0; i < sizeof sys / sizeof sys[0]; i++) add_path(out, cap, &n, sys[i]);
    return n;
}

int synth_from_path(const char *path, output_info *found)
{
    size_t n = 0;
    add_file(found, 1, &n, path);
    return n ? 0 : -1;
}

/* ---- the synth ---- */
static int open_synth(const output_info *o, double rate, fluid_backend *f, char *err, size_t errlen)
{
    int saved;
    quiet();
    if (strncmp(o->spec, "sf:", 3)) {
        snprintf(err, errlen, "bad SoundFont spec %s", o->spec);
        return -1;
    }
    f->settings = new_fluid_settings();
    if (!f->settings) {
        snprintf(err, errlen, "cannot start FluidSynth");
        return -1;
    }
    fluid_settings_setnum(f->settings, "synth.sample-rate", rate);
    fluid_settings_setint(f->settings, "synth.cpu-cores", 1);
    saved = hush();
    f->synth = new_fluid_synth(f->settings);
    unhush(saved);
    if (!f->synth || fluid_synth_sfload(f->synth, o->spec + 3, 1) == FLUID_FAILED) {
        snprintf(err, errlen, "cannot load %s%s%s", o->spec + 3, last_error[0] ? ": " : "", last_error);
        if (f->synth) delete_fluid_synth(f->synth);
        delete_fluid_settings(f->settings);
        f->synth = NULL;
        f->settings = NULL;
        return -1;
    }
    return 0;
}

static void close_synth(fluid_backend *f)
{
    if (f->synth) delete_fluid_synth(f->synth);
    if (f->settings) delete_fluid_settings(f->settings);
    f->synth = NULL;
    f->settings = NULL;
}

static void render(fluid_backend *f, float *l, float *r, uint32_t n)
{
    player *p = f->p;
    uint32_t done = 0, i;
    int mode = player_cycle(p);
    if (mode != CYCLE_PLAY) {            /* the API is synchronous: nothing to drain while seeking */
        memset(l, 0, n * 4u);
        memset(r, 0, n * 4u);
        return;
    }
    while (done < n) {
        uint32_t k = BLOCK - (uint32_t)(p->pos % BLOCK);
        if (k > n - done) k = n - done;
        if (p->pos % BLOCK == 0) player_dispatch(p);
        fluid_synth_write_float(f->synth, (int)k, l + done, 0, 1, r + done, 0, 1);
        p->pos += k;
        done += k;
    }
    player_after(p);
    {
        float pl = 0, pr = 0;
        for (i = 0; i < n; i++) {
            float x = l[i] < 0 ? -l[i] : l[i], y = r[i] < 0 ? -r[i] : r[i];
            if (x > pl) pl = x;
            if (y > pr) pr = y;
        }
        player_max_float(&p->peak_l, pl);
        player_max_float(&p->peak_r, pr);
    }
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void timed_render(fluid_backend *f, float *l, float *r, uint32_t n)
{
    uint64_t t0 = now_ns();
    render(f, l, r, n);
    player_max_float(&f->p->load, (float)((double)(now_ns() - t0) * 1e-9 / ((double)n / f->base.rate)));
}

static int audio_cb(void *data, int len, int nfx, float *fx[], int nout, float *out[])
{
    fluid_backend *f = data;
    (void)nfx; (void)fx;
    if (nout < 2 || len <= 0) return FLUID_OK;
    pthread_mutex_lock(&f->lock);        /* only fluid_start/fluid_stop contend for it */
    if (f->p) {
        timed_render(f, out[0], out[1], (uint32_t)len);
    } else {                             /* the driver runs from creation, before a song and between songs */
        memset(out[0], 0, (size_t)len * 4u);
        memset(out[1], 0, (size_t)len * 4u);
    }
    pthread_mutex_unlock(&f->lock);
    return FLUID_OK;
}

static void *null_thread(void *arg)
{
    fluid_backend *f = arg;
    enum { N = 512 };
    float l[N], r[N];
    uint64_t period = (uint64_t)((double)N / f->base.rate * 1e9), next = now_ns();
    while (!atomic_load(&f->quit)) {
        struct timespec ts;
        timed_render(f, l, r, N);
        next += period;
        ts.tv_sec = (time_t)(next / 1000000000ull);
        ts.tv_nsec = (long)(next % 1000000000ull);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    }
    return NULL;
}

static int fluid_send(backend *b, const uint8_t *m, uint32_t len)
{
    fluid_synth_t *s = ((fluid_backend *)b)->synth;
    int ch = m[0] & 15, d1 = len > 1 ? m[1] & 127 : 0, d2 = len > 2 ? m[2] & 127 : 0, rc = FLUID_OK;
    switch (m[0] & 0xf0) {
    case 0x80: fluid_synth_noteoff(s, ch, d1); break;    /* fails when the note is not sounding: fine */
    case 0x90: if (d2) rc = fluid_synth_noteon(s, ch, d1, d2); else fluid_synth_noteoff(s, ch, d1); break;
    case 0xa0: rc = fluid_synth_key_pressure(s, ch, d1, d2); break;
    case 0xb0: rc = fluid_synth_cc(s, ch, d1, d2); break;
    case 0xc0: fluid_synth_program_change(s, ch, d1); break;   /* fails when the preset is missing */
    case 0xd0: rc = fluid_synth_channel_pressure(s, ch, d1); break;
    case 0xe0: rc = fluid_synth_pitch_bend(s, ch, d1 | d2 << 7); break;
    case 0xf0:
        if (m[0] == 0xf0 && len > 2) {   /* FluidSynth wants the body without F0 ... F7 */
            uint32_t body = len - 1 - (m[len - 1] == 0xf7);
            if (fluid_synth_sysex(s, (const char *)m + 1, (int)body, NULL, NULL, NULL, 0) == FLUID_FAILED) rc = FLUID_FAILED;
        }
        break;
    }
    return rc == FLUID_FAILED ? SEND_FAIL : SEND_OK;
}

static void fluid_reset(backend *b)
{
    fluid_synth_system_reset(((fluid_backend *)b)->synth);
}

static int fluid_start(backend *b, player *p)
{
    fluid_backend *f = (fluid_backend *)b;
    atomic_store(&f->quit, 0);
    pthread_mutex_lock(&f->lock);
    f->p = p;
    pthread_mutex_unlock(&f->lock);
    if (f->null_audio && pthread_create(&f->th, NULL, null_thread, f)) {
        f->p = NULL;
        return -1;
    }
    return 0;
}

static void fluid_stop(backend *b)
{
    fluid_backend *f = (fluid_backend *)b;
    if (!f->p) return;
    if (f->null_audio) {
        atomic_store(&f->quit, 1);
        pthread_join(f->th, NULL);
    }
    pthread_mutex_lock(&f->lock);
    f->p = NULL;
    pthread_mutex_unlock(&f->lock);
    fluid_synth_all_sounds_off(f->synth, -1);
}

static void fluid_destroy(backend *b)
{
    fluid_backend *f = (fluid_backend *)b;
    fluid_stop(b);
    if (f->drv) delete_fluid_audio_driver(f->drv);
    close_synth(f);
    pthread_mutex_destroy(&f->lock);
    free(f);
}

static fluid_audio_driver_t *open_driver(fluid_backend *f, char *used, size_t usedlen)
{
    static const char *const prefer[] = { "pipewire", "pulseaudio", "alsa" };
    const char *env = getenv("MIDPLAY_AUDIO_DRIVER");
    fluid_audio_driver_t *d = NULL;
    size_t i;
    int saved = hush();
    if (env && *env) {
        fluid_settings_setstr(f->settings, "audio.driver", env);
        snprintf(used, usedlen, "%s", env);
        d = new_fluid_audio_driver2(f->settings, audio_cb, f);
    }
    for (i = 0; !d && !(env && *env) && i < sizeof prefer / sizeof prefer[0]; i++) {
        if (fluid_settings_setstr(f->settings, "audio.driver", prefer[i]) != FLUID_OK) continue;   /* not built in */
        if ((d = new_fluid_audio_driver2(f->settings, audio_cb, f)) != NULL) snprintf(used, usedlen, "%s", prefer[i]);
    }
    unhush(saved);
    return d;
}

backend *synth_open(const output_info *o, int null_audio, char *err, size_t errlen)
{
    fluid_backend *f = calloc(1, sizeof *f);
    double rate = 44100.0;
    char driver[64] = "";
    const char *env = getenv("MIDPLAY_RATE");
    if (!f) return NULL;
    if (env && atof(env) >= 8000.0) rate = atof(env);
    f->null_audio = null_audio;
    pthread_mutex_init(&f->lock, NULL);
    if (open_synth(o, rate, f, err, errlen)) {
        pthread_mutex_destroy(&f->lock);
        free(f);
        return NULL;
    }
    if (!null_audio && !(f->drv = open_driver(f, driver, sizeof driver))) {
        snprintf(err, errlen, "no audio output (tried %s)%s%s", driver[0] ? driver : "pipewire, pulseaudio, alsa",
                 last_error[0] ? ": " : "", last_error);
        close_synth(f);
        pthread_mutex_destroy(&f->lock);
        free(f);
        return NULL;
    }
    fluid_settings_getnum(f->settings, "synth.sample-rate", &rate);
    snprintf(f->base.name, sizeof f->base.name, "%s", o->name);
    snprintf(f->base.spec, sizeof f->base.spec, "%s", o->spec);
    f->base.is_synth = 1;
    f->base.rate = rate;
    f->base.quantum = BLOCK;
    f->base.send = fluid_send;
    f->base.reset = fluid_reset;
    f->base.start = fluid_start;
    f->base.stop = fluid_stop;
    f->base.destroy = fluid_destroy;
    return &f->base;
}

int synth_render_hash(const output_info *o, song *s, double rate, char hex[65], char *err, size_t errlen)
{
    fluid_backend f;
    player p;
    uint64_t total, done = 0;
    float *inter, l[4096], r[4096];
    uint8_t dg[32];
    int i;
    memset(&f, 0, sizeof f);
    memset(&p, 0, sizeof p);
    if (open_synth(o, rate, &f, err, errlen)) return -1;
    f.base.rate = rate;
    f.base.quantum = BLOCK;
    f.base.send = fluid_send;
    f.p = &p;
    player_bind(&p, s, &f.base);
    atomic_store(&p.playing, 1);
    total = s->end_frame;
    inter = malloc((size_t)total * 2 * sizeof(float));
    if (!inter) {
        snprintf(err, errlen, "out of memory");
        close_synth(&f);
        return -1;
    }
    while (done < total) {
        uint32_t n = total - done < 4096 ? (uint32_t)(total - done) : 4096, k;
        render(&f, l, r, n);
        for (k = 0; k < n; k++) {
            inter[(done + k) * 2] = l[k];
            inter[(done + k) * 2 + 1] = r[k];
        }
        done += n;
    }
    sha256((const uint8_t *)inter, (size_t)total * 2 * sizeof(float), dg);
    for (i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", dg[i]);
    free(inter);
    if (atomic_load(&p.late) || atomic_load(&p.dropped) || atomic_load(&p.skipped))
        fprintf(stderr, "note: late %u, dropped %u, skipped %u events\n", atomic_load(&p.late),
                atomic_load(&p.dropped), atomic_load(&p.skipped));
    close_synth(&f);
    return 0;
}
