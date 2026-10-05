/* names.c -- see names.h */
#include "names.h"

#include <stdio.h>
#include <string.h>

const char *const note_names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

static const char *const gm_names[128] = {
    "Grand Piano", "Bright Piano", "E.Grand Piano", "Honky-tonk", "E.Piano 1", "E.Piano 2", "Harpsichord", "Clavi",
    "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
    "Drawbar Organ", "Perc. Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
    "Nylon Guitar", "Steel Guitar", "Jazz Guitar", "Clean Guitar", "Muted Guitar", "Overdrive Gt", "Distortion Gt", "Gt Harmonics",
    "Acoustic Bass", "Finger Bass", "Pick Bass", "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
    "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Str", "Harp", "Timpani",
    "Strings 1", "Strings 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
    "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet",
    "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Square Lead", "Saw Lead", "Calliope Lead", "Chiff Lead", "Charang", "Voice Lead", "Fifths Lead", "Bass+Lead",
    "New Age Pad", "Warm Pad", "Polysynth Pad", "Choir Pad", "Bowed Pad", "Metallic Pad", "Halo Pad", "Sweep Pad",
    "FX Rain", "FX Soundtrack", "FX Crystal", "FX Atmosphere", "FX Brightness", "FX Goblins", "FX Echoes", "FX Sci-fi",
    "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
    "Gt Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone", "Helicopter", "Applause", "Gunshot",
};

void voice_name(char *out, size_t n, uint8_t msb, uint8_t prog)
{
    static const struct { uint8_t prog; const char *name; } kits[] = {
        { 0, "Standard Kit" }, { 1, "Standard Kit 2" }, { 8, "Room Kit" }, { 16, "Rock Kit" }, { 24, "Electro Kit" },
        { 25, "Analog Kit" }, { 32, "Jazz Kit" }, { 40, "Brush Kit" }, { 48, "Classic Kit" },
    };
    size_t i;
    if (msb == 126) {
        snprintf(out, n, "SFX Kit %u", prog + 1u);
        return;
    }
    if (msb == 127) {
        for (i = 0; i < sizeof kits / sizeof kits[0]; i++)
            if (kits[i].prog == prog) {
                snprintf(out, n, "%s", kits[i].name);
                return;
            }
        snprintf(out, n, "Drum Kit %u", prog + 1u);
        return;
    }
    snprintf(out, n, "%s", gm_names[prog & 127]);
}

const char *cc_name(int cc)
{
    switch (cc) {
    case 0: return "Bank MSB";     case 1: return "Modulation";  case 5: return "Porta Time";
    case 6: return "Data Entry";   case 7: return "Volume";      case 10: return "Pan";
    case 11: return "Expression";  case 32: return "Bank LSB";   case 38: return "Data LSB";
    case 64: return "Hold";        case 65: return "Portamento"; case 66: return "Sostenuto";
    case 67: return "Soft";        case 71: return "Harmonic";   case 72: return "Release";
    case 73: return "Attack";      case 74: return "Brightness"; case 84: return "Porta Ctrl";
    case 91: return "Reverb";      case 93: return "Chorus";     case 94: return "Variation";
    case 98: return "NRPN LSB";    case 99: return "NRPN MSB";   case 100: return "RPN LSB";
    case 101: return "RPN MSB";    case 120: return "Sound Off"; case 121: return "Reset Ctrl";
    case 123: return "Notes Off";  case 126: return "Mono";      case 127: return "Poly";
    default: return NULL;
    }
}

int is_xg_on(const item *it)
{
    static const uint8_t m[] = { 0x4c, 0x00, 0x00, 0x7e, 0x00, 0xf7 };
    return it->len == 9 && it->data[0] == 0xf0 && it->data[1] == 0x43 && (it->data[2] & 0xf0) == 0x10 &&
           !memcmp(it->data + 3, m, 6);
}

int is_gm_on(const item *it)
{
    return it->len == 6 && it->data[0] == 0xf0 && it->data[1] == 0x7e && it->data[3] == 0x09 && it->data[4] == 0x01;
}

int is_gs_reset(const item *it)
{
    static const uint8_t m[] = { 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
    return it->len == 11 && it->data[0] == 0xf0 && it->data[1] == 0x41 && !memcmp(it->data + 3, m, 8);
}

void describe(char *out, size_t n, const item *it)
{
    if (it->kind == K_TEMPO) { snprintf(out, n, "Tempo       %.2f", 60000000.0 / it->value); return; }
    if (it->kind == K_TSIG) { snprintf(out, n, "Beat        %u/%u", it->value >> 8, 1u << (it->value & 0xff)); return; }
    if (it->kind == K_TEXT) {
        snprintf(out, n, "%-11s %s",
                 it->meta == 5 ? "Lyric" : it->meta == 6 ? "Marker" : it->meta == 3 ? "Track Name" : "Text",
                 (char *)it->data);
        return;
    }
    if (it->data[0] < 0x80 || it->data[0] >= 0xf0) {
        const char *name = is_xg_on(it) ? "XG System On" : is_gm_on(it) ? "GM System On" : is_gs_reset(it) ? "GS Reset" : NULL;
        if (name) {
            snprintf(out, n, "SysEx       %s", name);
        } else if (it->len >= 8 && it->data[1] == 0x43 && (it->data[2] & 0xf0) == 0x10 && it->data[3] == 0x4c) {
            snprintf(out, n, "XG Param    %02X %02X %02X = %02X", it->data[4], it->data[5], it->data[6], it->data[7]);
        } else {
            size_t i, k = (size_t)snprintf(out, n, "SysEx      ");
            for (i = 0; i < it->len && k + 4 < n; i++) k += (size_t)snprintf(out + k, n - k, " %02X", it->data[i]);
        }
        return;
    }
    {
        uint8_t a = it->len > 1 ? it->data[1] : 0, b = it->len > 2 ? it->data[2] : 0;
        char nn[8];
        const char *cn;
        snprintf(nn, sizeof nn, "%s%d", note_names[a % 12], a / 12 - 1);
        switch (it->data[0] & 0xf0) {
        case 0x80: snprintf(out, n, "Note Off    %s", nn); break;
        case 0x90:
            if (b) snprintf(out, n, "Note        %-4s v%-3u gate %d", nn, b, it->gate);
            else snprintf(out, n, "Note Off    %s", nn);
            break;
        case 0xa0: snprintf(out, n, "Poly AT     %s %u", nn, b); break;
        case 0xb0:
            cn = cc_name(a);
            if (cn) snprintf(out, n, "CC %-3u      %-11s %u", a, cn, b);
            else snprintf(out, n, "CC %-3u      %u", a, b);
            break;
        case 0xc0: snprintf(out, n, "Program     %u", a + 1u); break;
        case 0xd0: snprintf(out, n, "Channel AT  %u", a); break;
        case 0xe0: snprintf(out, n, "Pitch Bend  %+d", (int)((b << 7) | a) - 8192); break;
        default: snprintf(out, n, "?"); break;
        }
    }
}
