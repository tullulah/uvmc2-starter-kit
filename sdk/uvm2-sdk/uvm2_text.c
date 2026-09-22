/*
 * uvm2_text.c — SYS_PRINT_TEXT (svc #16) for the halted bus.
 *
 * Geometry is copied deliberately from the IDE emulator's handler
 * (Rp2350System.ts, the vpy_print_text trap) so a string lands in the same
 * place on screen as it does in the simulator: same 4x6 stroke font, same
 * half-unit scaling, same baseline adjustment, same advance.  Text that drifts
 * between the emulator and hardware is far more expensive to debug than the
 * few lines of duplication this costs.
 *
 * Text is the most expensive thing a VPy frame can do here, and knowingly so:
 * each glyph re-centres the beam before drawing.  That per-glyph zero is
 * load-bearing — chaining glyphs without it leaves later ones unlit, because
 * the beam needs the relight.  At the default scale a glyph costs roughly as
 * much as two vectors, so a 10-character string is a fifth of a 50 Hz frame.
 */

#include "uvm2_bus.h"
#include "uvm2_draw.h"
#include "uvm2_text.h"
#include "uvm2_font.h"

/* Strokes for one character, folding lowercase onto uppercase as the font
 * covers ASCII 32..90 only.  Returns NULL when the glyph has no strokes
 * (space, and anything outside the range). */
static const uint8_t *glyph(int c, int *count)
{
    int idx;

    if (c >= 'a' && c <= 'z') c -= 32;
    if (c < UVM2_FONT_FIRST || c > UVM2_FONT_LAST) { *count = 0; return 0; }

    idx    = c - UVM2_FONT_FIRST;
    *count = uvm2_font_length[idx];
    return *count ? &uvm2_font_data[uvm2_font_offset[idx]] : 0;
}

void uvm2_print_text(int x, int y, const char *str, int scale, int intensity)
{
    int cur_x = x;
    /* y addresses the top of the glyph box; strokes count upward from the
     * baseline, so shift down by the box height. */
    int adj_y = y - ((6 * scale) >> 1);

    if (scale <= 0) scale = 3;
    if (intensity <= 0) intensity = 100;

    for (int i = 0; i < 256; i++) {
        int n;
        const uint8_t *st;
        unsigned char ch = (unsigned char)str[i];

        /* Vectrex strings end with NUL or the BIOS' 0x80 terminator. */
        if (ch == 0 || ch == 0x80) break;

        st = glyph(ch, &n);
        if (st) {
            int bx = 0, by = 0;   /* beam, relative to centre after the reset */

            uvm2_draw_reset();
            uvm2_draw_intensity(intensity);

            for (int s = 0; s < n; s += 3) {
                int tx = cur_x + ((st[s + 1] * scale) >> 1);
                int ty = adj_y + ((st[s + 2] * scale) >> 1);
                if (st[s] == 1) uvm2_draw_move(tx - bx, ty - by);
                else            uvm2_draw_delta(tx - bx, ty - by);
                bx = tx;
                by = ty;
            }
        }
        cur_x += (7 * scale) >> 1;
    }
}
