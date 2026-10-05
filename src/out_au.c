/* out_au.c -- an Audio Unit instrument ('aumu') played on the default output device.
 *
 * The device's render callback is the clock.  The instrument is rendered in pieces that never cross a
 * 128-frame boundary and each block's events are sent at its start (offset 0), so `midplay hash` gives
 * the same result on every run.
 * Paused, the instrument is not run at all, so playing resumes seamlessly.
 */
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "outputs.h"
#include "sha256.h"

#define BLOCK 128u

typedef struct {
    backend base;
    AudioUnit inst, out;
    player *p;
    int null_audio;
    pthread_t th;
    _Atomic int quit;
    mach_timebase_info_data_t tb;
} au_backend;

static void fourcc(char out[5], UInt32 v)
{
    int i;
    for (i = 0; i < 4; i++) {
        char c = (char)((v >> (24 - 8 * i)) & 0xff);
        out[i] = c >= 0x20 && c < 0x7f ? c : '?';
    }
    out[4] = 0;
}

static void cfstr(CFStringRef s, char *out, size_t n)
{
    out[0] = 0;
    if (s && !CFStringGetCString(s, out, (CFIndex)n, kCFStringEncodingUTF8)) out[0] = 0;
}

size_t synth_list(output_info *out, size_t cap)
{
    AudioComponentDescription d = { kAudioUnitType_MusicDevice, 0, 0, 0, 0 };
    AudioComponent c = NULL;
    size_t n = 0;
    while (n < cap && (c = AudioComponentFindNext(c, &d)) != NULL) {
        AudioComponentDescription cd;
        CFStringRef name = NULL;
        char m[5], st[5];
        if (AudioComponentGetDescription(c, &cd) != noErr) continue;
        AudioComponentCopyName(c, &name);
        cfstr(name, out[n].name, sizeof out[n].name);
        if (name) CFRelease(name);
        fourcc(m, cd.componentManufacturer);
        fourcc(st, cd.componentSubType);
        out[n].is_synth = 1;
        snprintf(out[n].spec, sizeof out[n].spec, "au:%s/%s", m, st);
        if (!out[n].name[0]) snprintf(out[n].name, sizeof out[n].name, "%s", out[n].spec);
        n++;
    }
    return n;
}

static UInt32 code(const char *s)
{
    return ((UInt32)(uint8_t)s[0] << 24) | ((UInt32)(uint8_t)s[1] << 16) | ((UInt32)(uint8_t)s[2] << 8) | (uint8_t)s[3];
}

static void set_format(AudioStreamBasicDescription *f, double rate)
{
    memset(f, 0, sizeof *f);
    f->mSampleRate = rate;
    f->mFormatID = kAudioFormatLinearPCM;
    f->mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    f->mBytesPerPacket = f->mBytesPerFrame = 4;
    f->mFramesPerPacket = 1;
    f->mChannelsPerFrame = 2;
    f->mBitsPerChannel = 32;
}

static int open_instrument(const output_info *o, double rate, AudioUnit *inst, char *err, size_t errlen)
{
    AudioComponentDescription d = { kAudioUnitType_MusicDevice, 0, 0, 0, 0 };
    AudioComponent c;
    AudioStreamBasicDescription f;
    OSStatus rc;
    const char *slash = strchr(o->spec + 3, '/');
    if (strncmp(o->spec, "au:", 3) || !slash || slash - (o->spec + 3) != 4 || strlen(slash + 1) != 4) {
        snprintf(err, errlen, "bad Audio Unit spec %s", o->spec);
        return -1;
    }
    d.componentManufacturer = code(o->spec + 3);
    d.componentSubType = code(slash + 1);
    c = AudioComponentFindNext(NULL, &d);
    if (!c || (rc = AudioComponentInstanceNew(c, inst)) != noErr) {
        snprintf(err, errlen, "cannot open %s", o->name);
        return -1;
    }
    set_format(&f, rate);
    if ((rc = AudioUnitSetProperty(*inst, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f)) != noErr ||
        (rc = AudioUnitInitialize(*inst)) != noErr) {
        snprintf(err, errlen, "%s does not start at %.0f Hz stereo Float32 (%d)", o->name, rate, (int)rc);
        AudioComponentInstanceDispose(*inst);
        *inst = NULL;
        return -1;
    }
    return 0;
}

static void render_inst(AudioUnit inst, uint64_t pos, float *l, float *r, uint32_t n)
{
    struct { UInt32 n; AudioBuffer b[2]; } abl;
    AudioUnitRenderActionFlags fl = 0;
    AudioTimeStamp ts;
    memset(&ts, 0, sizeof ts);
    ts.mSampleTime = (Float64)pos;
    ts.mFlags = kAudioTimeStampSampleTimeValid;
    abl.n = 2;
    abl.b[0].mNumberChannels = abl.b[1].mNumberChannels = 1;
    abl.b[0].mDataByteSize = abl.b[1].mDataByteSize = n * 4u;
    abl.b[0].mData = l;
    abl.b[1].mData = r;
    if (AudioUnitRender(inst, &fl, &ts, 0, n, (AudioBufferList *)&abl) != noErr) {
        memset(l, 0, n * 4u);
        memset(r, 0, n * 4u);
    }
}

static void render(au_backend *a, float *l, float *r, uint32_t n)
{
    player *p = a->p;
    uint32_t done = 0, i;
    int mode = player_cycle(p);
    if (mode == CYCLE_SEEK) {            /* keep the unit's MIDI queue draining while the main thread replays */
        while (done < n) {
            uint32_t k = n - done < BLOCK ? n - done : BLOCK;
            render_inst(a->inst, p->pos, l + done, r + done, k);
            done += k;
        }
        memset(l, 0, n * 4u);
        memset(r, 0, n * 4u);
        return;
    }
    if (mode == CYCLE_PAUSE) {
        memset(l, 0, n * 4u);
        memset(r, 0, n * 4u);
        return;
    }
    while (done < n) {
        uint32_t k = BLOCK - (uint32_t)(p->pos % BLOCK);
        if (k > n - done) k = n - done;
        if (p->pos % BLOCK == 0) player_dispatch(p);
        render_inst(a->inst, p->pos, l + done, r + done, k);
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

static void timed_render(au_backend *a, float *l, float *r, uint32_t n)
{
    uint64_t t0 = mach_absolute_time(), dt;
    render(a, l, r, n);
    dt = (mach_absolute_time() - t0) * a->tb.numer / a->tb.denom;
    player_max_float(&a->p->load, (float)((double)dt * 1e-9 / ((double)n / a->base.rate)));
}

static OSStatus out_cb(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                       UInt32 n, AudioBufferList *io)
{
    (void)flags; (void)ts; (void)bus;
    if (io->mNumberBuffers < 2) return kAudioUnitErr_InvalidParameter;
    timed_render(ref, (float *)io->mBuffers[0].mData, (float *)io->mBuffers[1].mData, n);
    return noErr;
}

static void *null_thread(void *arg)
{
    au_backend *a = arg;
    enum { N = 512 };
    float l[N], r[N];
    uint64_t period = (uint64_t)((double)N / a->base.rate * 1e9 * a->tb.denom / a->tb.numer);
    uint64_t next = mach_absolute_time();
    while (!atomic_load(&a->quit)) {
        timed_render(a, l, r, N);
        next += period;
        mach_wait_until(next);
    }
    return NULL;
}

static int au_send(backend *b, const uint8_t *m, uint32_t len)
{
    au_backend *a = (au_backend *)b;
    OSStatus rc;
    if (m[0] == 0xf0) rc = MusicDeviceSysEx(a->inst, m, len);
    else rc = MusicDeviceMIDIEvent(a->inst, m[0], len > 1 ? m[1] : 0, len > 2 ? m[2] : 0, 0);
    if (rc == noErr) return SEND_OK;
    return rc == kAudioUnitErr_CannotDoInCurrentContext ? SEND_BUSY : SEND_FAIL;
}

static void au_reset(backend *b)
{
    AudioUnitReset(((au_backend *)b)->inst, kAudioUnitScope_Global, 0);
}

static int au_start(backend *b, player *p)
{
    au_backend *a = (au_backend *)b;
    a->p = p;
    atomic_store(&a->quit, 0);
    if (a->null_audio) return pthread_create(&a->th, NULL, null_thread, a) ? -1 : 0;
    return AudioOutputUnitStart(a->out) == noErr ? 0 : -1;
}

static void au_stop(backend *b)
{
    au_backend *a = (au_backend *)b;
    if (!a->p) return;
    if (a->null_audio) {
        atomic_store(&a->quit, 1);
        pthread_join(a->th, NULL);
    } else {
        AudioOutputUnitStop(a->out);
    }
    a->p = NULL;
}

static void au_destroy(backend *b)
{
    au_backend *a = (au_backend *)b;
    au_stop(b);
    if (a->out) {
        AudioUnitUninitialize(a->out);
        AudioComponentInstanceDispose(a->out);
    }
    if (a->inst) {
        AudioUnitUninitialize(a->inst);
        AudioComponentInstanceDispose(a->inst);
    }
    free(a);
}

backend *synth_open(const output_info *o, int null_audio, char *err, size_t errlen)
{
    au_backend *a = calloc(1, sizeof *a);
    double rate = 44100.0;
    if (!a) return NULL;
    mach_timebase_info(&a->tb);
    a->null_audio = null_audio;
    if (!null_audio) {
        AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
        AudioComponent c = AudioComponentFindNext(NULL, &d);
        AudioStreamBasicDescription dev, f;
        AURenderCallbackStruct cb = { out_cb, a };
        UInt32 sz = sizeof dev;
        if (!c || AudioComponentInstanceNew(c, &a->out) != noErr) {
            snprintf(err, errlen, "no default audio output device");
            free(a);
            return NULL;
        }
        if (AudioUnitGetProperty(a->out, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &dev, &sz) == noErr &&
            dev.mSampleRate >= 1000.0)
            rate = dev.mSampleRate;
        set_format(&f, rate);
        if (AudioUnitSetProperty(a->out, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) != noErr ||
            AudioUnitSetProperty(a->out, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) != noErr ||
            AudioUnitInitialize(a->out) != noErr) {
            snprintf(err, errlen, "the audio output device does not take stereo Float32");
            AudioComponentInstanceDispose(a->out);
            free(a);
            return NULL;
        }
    }
    if (open_instrument(o, rate, &a->inst, err, errlen)) {
        if (a->out) {
            AudioUnitUninitialize(a->out);
            AudioComponentInstanceDispose(a->out);
        }
        free(a);
        return NULL;
    }
    snprintf(a->base.name, sizeof a->base.name, "%s", o->name);
    snprintf(a->base.spec, sizeof a->base.spec, "%s", o->spec);
    a->base.is_synth = 1;
    a->base.rate = rate;
    a->base.quantum = BLOCK;
    a->base.send = au_send;
    a->base.reset = au_reset;
    a->base.start = au_start;
    a->base.stop = au_stop;
    a->base.destroy = au_destroy;
    return &a->base;
}

int synth_render_hash(const output_info *o, song *s, double rate, char hex[65], char *err, size_t errlen)
{
    au_backend a;
    player p;
    uint64_t total, done = 0;
    float *inter, l[4096], r[4096];
    uint8_t dg[32];
    int i;
    memset(&a, 0, sizeof a);
    memset(&p, 0, sizeof p);
    if (open_instrument(o, rate, &a.inst, err, errlen)) return -1;
    a.base.rate = rate;
    a.base.quantum = BLOCK;
    a.base.send = au_send;
    a.p = &p;
    player_bind(&p, s, &a.base);
    atomic_store(&p.playing, 1);
    total = s->end_frame;
    inter = malloc((size_t)total * 2 * sizeof(float));
    if (!inter) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    while (done < total) {
        uint32_t n = total - done < 4096 ? (uint32_t)(total - done) : 4096, k;
        render(&a, l, r, n);
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
    AudioUnitUninitialize(a.inst);
    AudioComponentInstanceDispose(a.inst);
    return 0;
}
