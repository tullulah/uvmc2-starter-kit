/* aae_romload_gi.c — the wrapper that loads onto GI[], AAE's "region pointers". It lives
 * apart from the core (aae_romload.c) because the hand-written ports load onto their own
 * arrays and do not link AAE's globals; putting it all together would force globals.h into
 * places it has no business being.
 */
#include "aae_romload.h"
#include "globals.h"          /* GI[] */

int aae_rom_load(const aae_rom_op *ops, int n)
{
    return aae_rom_load_bases(ops, n, GI);
}
