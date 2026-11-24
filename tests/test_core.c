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
   mix(mc_rad_table, MC_RAD_LOW + 1601); mix(mc_drive, 256); mix(mc_comp, 256);
   mix(mc_eq_width, MC_EQ_FREQS); mix(mc_eq_cut, 128); mix(mc_eq_dry, 128); mix(mc_eq_ap, 128);
   const uint64_t tables = hash;
   uint32_t seed = 1;
   int32_t ctl[MC_CONTROLS] = {0, 127 << 16, 0, 0, 64 << 16, 64 << 16, 64 << 16};
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
      mono_core_block(&m, ctl, 0, 0, l, r);
      mix(l, MC_FRAMES); mix(r, MC_FRAMES);
   }
   const uint64_t audio = hash;

   /* key tracking: notes changing every 5 blocks, the switches through all four settings */
   hash = 1469598103934665603ULL;
   mono_core_reset(&m);
   int32_t word = (60 << 11) / 12, kctl[MC_CONTROLS] = {0, 64 << 16, 0, 0, 64 << 16, 64 << 16, 64 << 16};
   McPitch glide;
   mc_pitch_reset(&glide, word);
   for (int block = 0; block < 4000; ++block) {
      int32_t l[MC_FRAMES], r[MC_FRAMES];
      if (block % 7 == 6) {
         seed = seed * 1664525u + 1013904223u;
         kctl[(seed >> 8) % MC_CONTROLS] = (int32_t)(seed >> 9) % 0x7f0001;
      }
      if (block % 5 == 4) {
         seed = seed * 1664525u + 1013904223u;
         word = (int32_t)((seed >> 9) % 128 << 11) / 12;
      }
      const int32_t keys = (block / 250 & 1 ? MC_LP_KEY : 0) | (block / 500 & 1 ? MC_HP_KEY : 0);
      for (int i = 0; i < MC_FRAMES; ++i) {
         seed = seed * 1664525u + 1013904223u; l[i] = (int32_t)seed >> 10;
         seed = seed * 1664525u + 1013904223u; r[i] = (int32_t)seed >> 10;
      }
      mono_core_block(&m, kctl, mc_pitch(&glide, word), keys, l, r);
      mix(l, MC_FRAMES); mix(r, MC_FRAMES);
   }
   const uint64_t want_tables = 0x6cb9437eaa4f9343ULL, want_audio = 0xc4fa4862dc063c05ULL, want_keys = 0x392842be21a80112ULL;
   printf("tables %016llx audio %016llx keys %016llx\n", (unsigned long long)tables, (unsigned long long)audio, (unsigned long long)hash);
   if (tables != want_tables || audio != want_audio || hash != want_keys) { puts("FAIL: output differs from the firmware-verified build"); return 1; }
   puts("PASS core tables, audio and key tracking match the firmware-verified build");
   return 0;
}
