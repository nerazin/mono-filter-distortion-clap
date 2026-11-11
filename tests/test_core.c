/* Regression test for mono_core.h that needs no firmware.
 *
 * The core was checked sample-for-sample against Monomachine OS 1.32B running
 * in an emulator (see the README). This test pins that verified behaviour:
 * it hashes the coefficient tables and the output for a fixed stimulus with
 * moving controls, and fails if either changes on any compiler or platform. */
#include <stdio.h>
#include "../source/mono_core.h"

static uint64_t hash = 1469598103934665603ULL;
static void mix(const int32_t *v, int n) {
   for (int i = 0; i < n; ++i)
      for (int b = 0; b < 4; ++b) hash = (hash ^ (((uint32_t)v[i] >> (8 * b)) & 0xff)) * 1099511628211ULL;
}
int main(void) {
   MonoCore m;
   mono_core_reset(&m);
   mix(mc_cos, MC_FREQS); mix(mc_sin, MC_FREQS); mix(mc_pole, MC_FREQS);
   mix(mc_rad_table, MC_RAD_LOW + 1601); mix(mc_drive, 256); mix(mc_comp, 256); mix(mc_eq, 4);
   const uint64_t tables = hash;
   uint32_t seed = 1;
   int32_t ctl[MC_CONTROLS] = {0, 127 << 16, 0, 0, 64 << 16};
   for (int block = 0; block < 4000; ++block) {
      int32_t l[MC_FRAMES], r[MC_FRAMES];
      if (block % 7 == 6) {                               /* move one control */
         seed = seed * 1664525u + 1013904223u;
         ctl[(seed >> 8) % MC_CONTROLS] = (int32_t)(seed >> 9) % 0x7f0001;
      }
      const int shift = 8 + (block / 500) % 8;            /* full scale down to -42 dB */
      for (int i = 0; i < MC_FRAMES; ++i) {
         seed = seed * 1664525u + 1013904223u; l[i] = (int32_t)seed >> shift;
         seed = seed * 1664525u + 1013904223u; r[i] = (int32_t)seed >> shift;
      }
      mono_core_block(&m, ctl, l, r);
      mix(l, MC_FRAMES); mix(r, MC_FRAMES);
   }
   const uint64_t want_tables = 0x7c16be6b8271c1f7ULL, want_audio = 0x3ee94d3e5ec6fa28ULL;
   printf("tables %016llx audio %016llx\n", (unsigned long long)tables, (unsigned long long)hash);
   if (tables != want_tables || hash != want_audio) { puts("FAIL: output differs from the firmware-verified build"); return 1; }
   puts("PASS core tables and audio match the firmware-verified build");
   return 0;
}
