/* settings.c -- see settings.h */
#include "settings.h"

#include <stdio.h>
#include <string.h>

enum { ITEM_OUTPUT, ITEM_AUTOPLAY, ITEM_END, ITEM_ASCII, ITEMS };

void settings_enter(settings_ui *s, const char *current_spec)
{
    size_t i;
    s->sel = 0;
    s->picking = 0;
    s->msg[0] = 0;
    s->nouts = outputs_list(s->outs, 256);
    s->osel = 0;
    for (i = 0; i < s->nouts; i++)
        if (!strcmp(s->outs[i].spec, current_spec)) s->osel = (int)i;
}

int settings_key(settings_ui *s, config *cfg, int key, output_info *chosen)
{
    if (s->picking) {
        switch (key) {
        case KEY_UP: case 'k': if (s->osel > 0) s->osel--; break;
        case KEY_DOWN: case 'j': if (s->osel + 1 < (int)s->nouts) s->osel++; break;
        case KEY_ESC: case KEY_LEFT: case KEY_BACKSPACE: s->picking = 0; break;
        case KEY_ENTER: case KEY_RIGHT: case ' ':
            if (!s->nouts) break;
            *chosen = s->outs[s->osel];         /* saved by the caller once it has opened */
            s->picking = 0;
            return SET_OUTPUT;
        default: break;
        }
        return SET_NONE;
    }
    switch (key) {
    case KEY_UP: case 'k': if (s->sel > 0) s->sel--; break;
    case KEY_DOWN: case 'j': if (s->sel + 1 < ITEMS) s->sel++; break;
    case KEY_ESC: case 'c': case KEY_BACKSPACE: return SET_CLOSE;
    case KEY_ENTER: case KEY_RIGHT: case KEY_LEFT: case ' ':
        switch (s->sel) {
        case ITEM_OUTPUT:
            s->nouts = outputs_list(s->outs, 256);   /* devices come and go: list them again */
            s->picking = 1;
            break;
        case ITEM_AUTOPLAY: cfg->autoplay = !cfg->autoplay; return SET_CHANGED;
        case ITEM_END: cfg->exit_at_end = !cfg->exit_at_end; return SET_CHANGED;
        case ITEM_ASCII: cfg->ascii = !cfg->ascii; return SET_CHANGED;
        default: break;
        }
        break;
    default: break;
    }
    return SET_NONE;
}

static void row(sbuf *sb, int sel, const char *label, const char *value, int W)
{
    char buf[1024];
    sb_attr(sb, sel ? ATTR(231, 25, 0, 1, 0) : A_NONE);
    snprintf(buf, sizeof buf, "%s %-26s %s", sel ? " >" : "  ", label, value);
    sb_text(sb, buf, W, sel);
    sb_eol(sb);
}

void settings_draw(sbuf *sb, settings_ui *s, const config *cfg, const char *current_name, int W, int H)
{
    int i, used = 0;
    char buf[1024];
    sb_attr(sb, ATTR(16, 75, 0, 1, 0));
    sb_str(sb, " midplay ");
    sb_attr(sb, ATTR(252, -1, 0, 1, 0));
    sb_str(sb, " Settings");
    sb_eol(sb);
    sb_eol(sb);
    row(sb, !s->picking && s->sel == ITEM_OUTPUT, "Output", current_name, W);
    row(sb, !s->picking && s->sel == ITEM_AUTOPLAY, "Start playing on open", cfg->autoplay ? "yes" : "no (press Space)", W);
    row(sb, !s->picking && s->sel == ITEM_END, "At the end of the song", cfg->exit_at_end ? "stop and quit to the shell" : "stop and stay", W);
    row(sb, !s->picking && s->sel == ITEM_ASCII, "ASCII-only drawing", cfg->ascii ? "yes" : "no", W);
    used = 6;
    sb_eol(sb);
    used++;
    if (s->picking) {
        sb_attr(sb, ATTR(245, -1, 0, 0, 0));
        sb_str(sb, "  Choose an output (Audio Units play on the default audio device):");
        sb_eol(sb);
        used++;
        for (i = 0; i < (int)s->nouts && used < H - 2; i++, used++) {
            snprintf(buf, sizeof buf, "%3d. [%s] %-40s %s", i + 1, s->outs[i].is_au ? "AU  " : "MIDI", s->outs[i].name,
                     s->outs[i].spec);
            sb_attr(sb, i == s->osel ? ATTR(231, 25, 0, 1, 0) : A_NONE);
            sb_str(sb, i == s->osel ? " > " : "   ");
            sb_text(sb, buf, W - 3, i == s->osel);
            sb_eol(sb);
        }
        if (!s->nouts) {
            sb_str(sb, "   (no Audio Unit instruments and no MIDI destinations found)");
            sb_eol(sb);
            used++;
        }
    }
    for (; used < H - 2; used++) sb_eol(sb);
    sb_attr(sb, ATTR(229, -1, 0, 0, 0));
    snprintf(buf, sizeof buf, " saved in %s%s%s", config_path(), s->msg[0] ? "   " : "", s->msg);
    sb_text(sb, buf, W, 0);
    sb_eol(sb);
    sb_attr(sb, ATTR(16, 250, 0, 0, 0));
    sb_text(sb, s->picking ? " Up Down choose  Enter use it  Esc back" : " Up Down choose  Enter change  Esc/c close", W, 1);
}
