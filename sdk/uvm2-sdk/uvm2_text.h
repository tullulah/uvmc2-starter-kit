/*
 * uvm2_text.h — stroke-font text on the halted bus (SYS_PRINT_TEXT).
 */
#ifndef UVM2_TEXT_H
#define UVM2_TEXT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Draw `str` with its top-left at (x, y) in VPy units.
 * `scale` is the half-unit size the VPy stub resolves (TEXT_SIZE, default 3 →
 * x1.5); `intensity` is 0..127.  Stops at NUL or the Vectrex 0x80 terminator,
 * and after 256 characters. */
void uvm2_print_text(int x, int y, const char *str, int scale, int intensity);

#ifdef __cplusplus
}
#endif

#endif /* UVM2_TEXT_H */
