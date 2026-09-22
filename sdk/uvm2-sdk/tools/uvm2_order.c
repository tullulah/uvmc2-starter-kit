/* How much does ORDERING BY SLOPE save? It replays a real frame through the REAL
 * uvm2_draw.c against a simulated bus, in several orders, and counts the commands.
 *
 * y_can_skip() compares the ramp's Y VELOCITY, not the position: when two consecutive
 * operations share vy, three writes AND THEIR THREE DELAYS are saved — 6 commands out of
 * 15. The motivating scene is parallel girders, horizontal rungs and vertical uprights,
 * i.e. a handful of slopes repeated hundreds of times.
 *
 *   cc -O2 -DUVM2_HOST -I<sdk> -o order tools/uvm2_order.c uvm2_draw.c <cabi>.a eh.c
 *   ./order dump.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "uvm2_bus.h"
#include "uvm2_draw.h"

uvm2_stats_t uvm2_stats;
static uint32_t g_cmd, g_cyc;
uint32_t uvm2_exec(const uint8_t *c, uint32_t n){ (void)c; g_cmd += n; g_cyc += n; return n; }
void     uvm2_bus_delay(uint32_t c){ g_cyc += c; }
void     uvm2_via_write(uint32_t r, uint32_t d){ (void)r; (void)d; g_cmd++; g_cyc++; }
uint8_t  uvm2_via_read(uint32_t r){ (void)r; return 0; }

#define MAXS 2048
static int sx0[MAXS], sy0[MAXS], sx1[MAXS], sy1[MAXS], sb[MAXS], sk[MAXS], n;

/* the key: the slope in the DAC's scale, which is what y_can_skip compares */
static int slope_key(int i){
    int dx = sx1[i]-sx0[i], dy = sy1[i]-sy0[i];
    int m = (dx<0?-dx:dx) > (dy<0?-dy:dy) ? (dx<0?-dx:dx) : (dy<0?-dy:dy);
    return m ? (dy * 127) / m : 0;
}
static int cmp(const void *a, const void *b){
    int i = *(const int*)a, j = *(const int*)b;
    return sk[i] != sk[j] ? sk[i]-sk[j] : sx0[i]-sx0[j];
}

static void replay(const int *ord, const char *q){
    g_cmd = g_cyc = 0;
    uvm2_draw_init(); uvm2_frame_begin();
    int bx = 0, by = 0, bri = -1, nmov = 0, nvy = 0, vyprev = 0x7fffffff;
    for (int k = 0; k < n; k++){
        int i = ord[k];
        if (!sb[i]) continue;
        if (sb[i] != bri){ uvm2_draw_intensity(sb[i]); bri = sb[i]; }
        if (sx0[i] != bx || sy0[i] != by){ uvm2_draw_move(sx0[i]-bx, sy0[i]-by); nmov++; }
        { int dx=sx1[i]-sx0[i], dy=sy1[i]-sy0[i];
          int mm=(dx<0?-dx:dx)>(dy<0?-dy:dy)?(dx<0?-dx:dx):(dy<0?-dy:dy);
          int vy = mm ? (dy*127)/mm : 0;
          if (vy == vyprev) nvy++; vyprev = vy; }
        uvm2_draw_delta(sx1[i]-sx0[i], sy1[i]-sy0[i]);
        bx = sx1[i]; by = sy1[i];
    }
    uvm2_frame_end();
    printf("  %-24s %5u commands  %4d jumps  %4d with the same slope\n", q, uvm2_stats.commands, nmov, nvy);
}

/* nearest neighbour: the one starting closest to where the beam ended, at either of its
 * two endpoints. It is what flush_frame does in the cartridge SDK. */
static void nearest(int *o){
    char *used = calloc(n,1);
    int bx = 0, by = 0, m = 0;
    for (int k = 0; k < n; k++){
        int best = -1; long bd = 0; int rev = 0;
        for (int i = 0; i < n; i++){
            if (used[i] || !sb[i]) continue;
            long d0 = (long)(sx0[i]-bx)*(sx0[i]-bx) + (long)(sy0[i]-by)*(sy0[i]-by);
            long d1 = (long)(sx1[i]-bx)*(sx1[i]-bx) + (long)(sy1[i]-by)*(sy1[i]-by);
            if (best < 0 || d0 < bd){ bd = d0; best = i; rev = 0; }
            if (d1 < bd){ bd = d1; best = i; rev = 1; }
        }
        if (best < 0) break;
        used[best] = 1;
        if (rev){ int t;
            t=sx0[best]; sx0[best]=sx1[best]; sx1[best]=t;
            t=sy0[best]; sy0[best]=sy1[best]; sy1[best]=t; }
        o[m++] = best; bx = sx1[best]; by = sy1[best];
    }
    while (m < n) o[m++] = 0;
    free(used);
}

/* GREEDY, WITH A PREFERENCE FOR SLOPE. The 6-command saving is triggered by y_can_skip,
 * which compares the Y VELOCITY — that is, the slope — not the position. And a jump costs
 * 15, so the slope cannot be chased at the cost of moving. Priority:
 *   1. starts where the beam ended AND with the same slope   (9 commands, no jump)
 *   2. starts where the beam ended                           (15, no jump)
 *   3. the same slope, as close as possible                  (15 + a short jump)
 *   4. the closest                                           (15 + jump)
 */
static void greedy(int *o){
    char *used = calloc(n,1);
    int bx = 0, by = 0, bk = 0x7fffffff, m = 0;
    for (int k = 0; k < n; k++){
        int best = -1, rev = 0, best_rank = 9; long bd = 0;
        for (int i = 0; i < n; i++){
            if (used[i] || !sb[i]) continue;
            for (int r = 0; r < 2; r++){
                int px = r ? sx1[i] : sx0[i], py = r ? sy1[i] : sy0[i];
                int ky = r ? -sk[i] : sk[i];
                int glued = (px == bx && py == by);
                int rank = glued ? (ky == bk ? 0 : 1) : (ky == bk ? 2 : 3);
                long d = (long)(px-bx)*(px-bx) + (long)(py-by)*(py-by);
                if (rank < best_rank || (rank == best_rank && d < bd)){
                    best_rank = rank; bd = d; best = i; rev = r;
                }
            }
        }
        if (best < 0) break;
        used[best] = 1;
        if (rev){ int t;
            t=sx0[best]; sx0[best]=sx1[best]; sx1[best]=t;
            t=sy0[best]; sy0[best]=sy1[best]; sy1[best]=t; sk[best] = -sk[best]; }
        o[m++] = best; bx = sx1[best]; by = sy1[best]; bk = sk[best];
    }
    while (m < n) o[m++] = 0;
    free(used);
}

/* ORDER OBJECTS, NOT SEGMENTS. Reordering loose segments breaks the chains and ADDS jumps
 * (measured: 5307 -> 5838). What flush_frame does in the SDK is order STROKES — chains of
 * contiguous segments — leaving their interior untouched. Here they are grouped first and
 * ordered afterwards, which is the only way reordering can win. */
static void by_strokes(int *o){
    /* 1. group: a stroke is a chain of segments where one's end is the next one's start,
     *    in the order the game emitted them. */
    int ini[MAXS], end[MAXS], nt = 0;
    for (int i = 0; i < n; ){
        if (!sb[i]) { i++; continue; }
        int j = i;
        while (j+1 < n && sb[j+1] && sx1[j] == sx0[j+1] && sy1[j] == sy0[j+1]) j++;
        ini[nt] = i; end[nt] = j; nt++;
        i = j + 1;
    }
    /* 2. order the strokes by proximity, each in whichever direction starts closer */
    char *used = calloc(nt,1);
    int bx = 0, by = 0, m = 0;
    for (int k = 0; k < nt; k++){
        int best = -1, rev = 0; long bd = 0;
        for (int t = 0; t < nt; t++){
            if (used[t]) continue;
            long d0 = (long)(sx0[ini[t]]-bx)*(sx0[ini[t]]-bx) + (long)(sy0[ini[t]]-by)*(sy0[ini[t]]-by);
            long d1 = (long)(sx1[end[t]]-bx)*(sx1[end[t]]-bx) + (long)(sy1[end[t]]-by)*(sy1[end[t]]-by);
            if (best < 0 || d0 < bd){ bd = d0; best = t; rev = 0; }
            if (d1 < bd){ bd = d1; best = t; rev = 1; }
        }
        if (best < 0) break;
        used[best] = 1;
        if (!rev){ for (int i = ini[best]; i <= end[best]; i++) o[m++] = i;
                   bx = sx1[end[best]]; by = sy1[end[best]]; }
        else {     for (int i = end[best]; i >= ini[best]; i--){
                       int t2;
                       t2=sx0[i]; sx0[i]=sx1[i]; sx1[i]=t2;
                       t2=sy0[i]; sy0[i]=sy1[i]; sy1[i]=t2;
                       o[m++] = i; }
                   bx = sx1[end[best]]; by = sy1[end[best]]; }
    }
    printf("  (%d strokes from %d segments)\n", nt, n);
    while (m < n) o[m++] = 0;
    free(used);
}

int main(int argc, char **argv){
    FILE *f = fopen(argv[1], "r"); if (!f) return 2;
    char l[256];
    while (fgets(l, sizeof l, f) && n < MAXS){
        int a,b,c,d,e;
        if (sscanf(l, "%d %d %d %d %d", &a,&b,&c,&d,&e) == 5){
            sx0[n]=a/127; sy0[n]=b/127; sx1[n]=c/127; sy1[n]=d/127; sb[n]=e; n++;
        }
    }
    fclose(f);
    int *o1 = malloc(n*sizeof(int)), *o2 = malloc(n*sizeof(int));
    for (int i = 0; i < n; i++){ o1[i]=i; sk[i]=slope_key(i); }
    memcpy(o2, o1, n*sizeof(int));
    qsort(o2, n, sizeof(int), cmp);
    printf("%d segments\n", n);
    replay(o1, "current order");
    replay(o2, "sorted by slope");
    int *o3 = malloc(n*sizeof(int)); nearest(o3);
    replay(o3, "nearest neighbour");
    int *o4 = malloc(n*sizeof(int));
    for (int i = 0; i < n; i++) sk[i] = slope_key(i);
    greedy(o4);
    replay(o4, "greedy + slope");
    int *o5 = malloc(n*sizeof(int));
    by_strokes(o5);
    replay(o5, "by STROKES (objects)");
    return 0;
}
