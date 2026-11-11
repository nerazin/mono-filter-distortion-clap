/* mono_core.h - Monomachine track filter + distortion, fixed-point core.
 *
 * A native C port of the per-track signal path of Monomachine OS 1.32B
 * (THRU machine input -> EQ -> 2x up -> resonant low-pass -> resonant
 * high-pass -> drive/clip -> two smoothing poles -> 2x down -> two DC
 * blockers), written from the DSP56300 code and checked sample-for-sample
 * against that code running in an emulator. It contains no firmware code or
 * data: every coefficient table is generated from a formula at start-up.
 *
 * The arithmetic follows the DSP: 24-bit words, 56-bit accumulators,
 * truncation and saturation exactly where the original has them. Blocks are
 * 16 stereo frames at 44.1 kHz; controls are the 24-bit block words the
 * Monomachine's main CPU sends (knob value << 16).
 *
 * Plain C99, no dependencies beyond libm. SPDX-License-Identifier: GPL-3.0-only */
#ifndef MONO_CORE_H
#define MONO_CORE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define MC_FRAMES 16          /* frames per block */
#define MC_ONE 0x7fffff       /* largest 24-bit fraction */

/* ---- DSP56300 arithmetic ------------------------------------------------ */

typedef int64_t mc_acc;       /* 56-bit accumulator, kept sign-extended */

static inline mc_acc mc_s56(mc_acc v) { return (mc_acc)((uint64_t)v << 8) >> 8; }
static inline mc_acc mc_w(int32_t x) { return (mc_acc)x * 16777216; }             /* word -> accumulator */
static inline mc_acc mc_mpy(int32_t x, int32_t y) { return (mc_acc)x * y * 2; }   /* signed fractional product */
static inline mc_acc mc_mpysu(int32_t s, uint32_t u) { return (mc_acc)s * (mc_acc)(u & 0xffffff) * 2; }
static inline mc_acc mc_shl(mc_acc a, int n) { return mc_s56((mc_acc)((uint64_t)a << n)); }
static inline int32_t mc_hi(mc_acc a) { return (int32_t)((uint32_t)(a >> 24) << 8) >> 8; }   /* A1, no limiting */
static inline uint32_t mc_lo(mc_acc a) { return (uint32_t)a & 0xffffff; }                    /* A0 */
/* Accumulator -> 24-bit word: truncate, saturate. */
static inline int32_t mc_sat(mc_acc a) {
   const mc_acc v = a >> 24;
   return v > MC_ONE ? MC_ONE : v < -MC_ONE - 1 ? -MC_ONE - 1 : (int32_t)v;
}
/* Accumulator -> 48-bit long word, saturated (what a store to L: memory keeps). */
static inline mc_acc mc_sat48(mc_acc a) {
   const mc_acc top = ((mc_acc)1 << 47) - 1;
   return a > top ? top : a < -top - 1 ? -top - 1 : a;
}
/* RND: round to 24 bits, ties to even. */
static inline mc_acc mc_rnd(mc_acc a) {
   a += 0x800000;
   if (!(a & 0xffffff)) a &= ~(mc_acc)0x1000000;
   return mc_s56(a & ~(mc_acc)0xffffff);
}
/* 24 DIV steps with carry cleared: the quotient a / s, a 24-bit fraction. */
static int32_t mc_div(mc_acc a, int32_t s) {
   uint64_t d = (uint64_t)a & 0xffffffffffffffULL;
   const uint64_t sv = (uint64_t)mc_w(s);
   unsigned carry = 0;
   for (int i = 0; i < 24; ++i) {
      const int add = (int)((d >> 55) & 1) != ((s >> 23) & 1);
      d = ((d << 1) | carry) & 0xffffffffffffffULL;
      d = (((add ? d + sv : d - sv)) & 0xffffffff000000ULL) | (d & 0xffffff);
      carry = !((d >> 55) & 1);
   }
   return (int32_t)(d & 0xffffff);
}

/* ---- coefficient tables ------------------------------------------------- */

/* All frequency tables share one axis: 128 steps per octave, step 1024 =
 * 932.33 Hz, evaluated at the 88.2 kHz rate of the oversampled section. */
#define MC_FREQS 1728
#define MC_RAD_LOW 1774       /* the pole-radius table reaches this far below 0 */

static int32_t mc_cos[MC_FREQS], mc_sin[MC_FREQS], mc_pole[MC_FREQS];
static int32_t mc_rad_table[MC_RAD_LOW + 1601];
static int32_t mc_drive[256], mc_comp[256];
static int32_t mc_eq[4];
static int mc_tables_ready;

static int32_t mc_fix(double v) { const double r = rint(v * 8388608.0); return r > MC_ONE ? MC_ONE : (int32_t)r; }
static double mc_omega(int step) {
   return 2.0 * 3.14159265358979323846 * 440.0 * pow(2.0, 13.0 / 12.0 + (step - 1024) / 128.0) / 88200.0;
}
static inline int32_t mc_rad(int step) { return mc_rad_table[step + MC_RAD_LOW]; }

static void mc_tables(void) {
   if (mc_tables_ready) return;
   for (int i = 0; i < MC_FREQS; ++i) {
      const double w = mc_omega(i), c = 1.0 - cos(w), m = sqrt(2.0) - 1.0;
      mc_cos[i] = mc_fix(cos(w));
      mc_sin[i] = (int32_t)rint(sin(w) * MC_ONE);
      /* one-pole coefficient that puts two cascaded poles 3 dB down at w */
      mc_pole[i] = mc_fix((sqrt(c * c + 2.0 * c * m) - c) / m);
   }
   mc_cos[1396] += 1;   /* the original table rounds this one entry the other way */
   for (int r = -MC_RAD_LOW; r <= 1600; ++r) mc_rad_table[r + MC_RAD_LOW] = mc_fix(exp(-mc_omega(r) / 1.1));
   for (int n = 0; n < 256; ++n) {
      /* input drive: 1.35 dB per DIST step below centre, 1/1.1 at the top */
      mc_drive[n] = (int32_t)rint(exp(-0.09531018589 - 0.0388485254 * (255 - n)) * 8388608.0);
      /* output compensation for the post-filter gain p: 1/p up to p = 2,
       * 1/sqrt(2p) up to p = 4, flat beyond */
      const double p = 1.0 + 60.6867447 * (n / 256.0) * (n / 256.0);
      double e = 1.0 / p;
      if (1.0 / sqrt(2.0 * p) > e) e = 1.0 / sqrt(2.0 * p);
      if (e < sqrt(0.125)) e = sqrt(0.125);
      mc_comp[n] = mc_fix(e);
   }
   /* the formula is one LSB off the original in these eleven entries */
   static const signed char fix[][2] = {{2,-1},{4,1},{6,-1},{8,-1},{11,1},{12,1},{19,1},{20,1},{21,1},{29,-1},{56,1}};
   for (unsigned i = 0; i < sizeof fix / sizeof fix[0]; ++i) mc_comp[fix[i][0]] += fix[i][1];

   /* The track EQ sits in front of the filter and cannot be switched off. It
    * is held at the EFX page defaults (EQF 64, EQG 64 = "0" on the display):
    * a second-order all-pass at 1.62 kHz mixed with the input, which also
    * sets the internal headroom (-30.6 dB). 0x03c5e3 is the EQF width term,
    * 0x00fdf8 / -0x000d63 the two EQG mix gains at those positions. */
   const int32_t width = 0x03c5e3, cosine = mc_cos[1126];
   const int32_t q = mc_div(mc_w(0x010000), mc_sat((mc_w(width) + mc_w(MC_ONE)) >> 1));
   const int32_t k = mc_sat(mc_shl(mc_mpy(q, mc_sat(mc_w(MC_ONE) - mc_w(width))), 6));
   mc_eq[0] = mc_sat((-mc_mpy(cosine, k) - mc_w(cosine)) >> 1);
   mc_eq[1] = k;
   mc_eq[2] = 0x00fdf8;
   mc_eq[3] = -0x000d63;
   mc_tables_ready = 1;
}

/* ---- state -------------------------------------------------------------- */

typedef struct {
   int32_t eq_in[2], eq_ap[2];   /* EQ all-pass: last two inputs and outputs */
   int32_t up[3];                /* interpolator history */
   mc_acc lp_a, lp_b;            /* low-pass resonator */
   int32_t hp_in[2];             /* high-pass: its input, two samples back */
   mc_acc hp_a, hp_b;            /* high-pass resonator */
   mc_acc smooth[2];             /* the two poles after the clipper */
   int32_t down[5];              /* decimator history */
   int32_t dc_in[2];             /* DC blockers: previous input */
   mc_acc dc[2];
} McChannel;

typedef struct { int32_t step, shifted, a, b, gain; } McResonator;   /* last block's values */

typedef struct {
   McChannel ch[2];
   McResonator lp, hp;
   int32_t smooth_f, smooth_g;   /* last block's smoothing-pole coefficient and input gain */
   int32_t dc_t, dc_c;           /* last block's DC-blocker coefficients */
} MonoCore;

typedef struct { int32_t gain[16], a[16], b[16]; } McRamp;   /* one entry per input frame */

static void mono_core_reset(MonoCore *m) { mc_tables(); memset(m, 0, sizeof *m); }

/* ---- per-block coefficients --------------------------------------------- */

static void mc_ramp(int32_t *dst, int n, int32_t from, int32_t delta, int32_t step) {
   mc_acc a = mc_w(from);
   for (int k = 0; k < n; ++k) {
      dst[k] = mc_sat(a);
      a += mc_mpy(delta, step);
   }
}

/* Pole pair at radius r, angle w: returns the feedback terms r*cos(w) - 1 and
 * r*sin(w), and the input gain (1 - 2r*cos(w) + r^2) / (8r*sin(w)). */
static int32_t mc_pole_pair(int32_t cosine, int32_t sine, int32_t radius, int32_t *fa, int32_t *fb) {
   const mc_acc a = mc_mpy(cosine, radius), b = mc_mpy(radius, sine);
   *fa = mc_sat(a - mc_w(0x800000));
   *fb = mc_sat(b);
   const mc_acc d = (mc_mpy(radius, radius) >> 1) - a + mc_w(0x400000);
   const int32_t q = mc_div(mc_w(0x800), *fb);
   return mc_sat(mc_shl((mc_mpysu(q, mc_lo(d)) >> 24) + mc_mpy(q, mc_hi(d)), 10));
}

/* Resonator coefficients for one block. `target` is the cutoff in table
 * steps, `q_word` the resonance knob, `track` how fast resonance is backed
 * off above step 1599, `drive` an extra input gain (negative: none). The
 * block starts from last block's values, passes through the half-way point
 * after eight frames and ends on the new target. */
static void mc_resonator(McResonator *s, int32_t target, int32_t q_word, int32_t track, int32_t drive, McRamp *r) {
   mc_acc a = mc_w(target) - mc_w(0x5bf);
   if (a < 0) a = 0;
   const int32_t over1 = mc_sat(a);
   a -= mc_w(0x80);
   if (a < 0) a = 0;
   const int32_t over2 = mc_sat(a);
   const int32_t step = target > 0x63f ? 0x63f : target < 0 ? 0 : target;
   const int32_t damp = mc_sat(mc_shl(mc_mpy(-525, step) + mc_mpy(-6554, over1) + mc_mpy(track, over2), 24));
   a = mc_w(q_word) + mc_mpy(damp, q_word);
   if (a < 0) a = 0;
   /* resonance and cutoff share one axis: more resonance = a lower radius index */
   mc_acc b = mc_w(step) + mc_mpy(-1774, mc_sat(a));
   const int32_t shifted = mc_hi(b);
   a = (mc_w(s->step) + mc_w(step)) >> 1;
   b = (b + mc_w(s->shifted)) >> 1;
   const int32_t mid = mc_hi(a), mid_shifted = mc_hi(b);
   const uint32_t fa = mc_lo(a), fb = mc_lo(b);
   s->step = step;
   s->shifted = shifted;

   const int32_t cosine = mc_sat(mc_w(mc_cos[mid]) - mc_mpysu(mc_cos[mid], fa) + mc_mpysu(mc_cos[mid + 1], fa));
   const int32_t sine = mc_sat(mc_w(mc_sin[mid]) - mc_mpysu(mc_sin[mid], fa) + mc_mpysu(mc_sin[mid + 1], fa));
   const mc_acc limit = mc_w(0x7fffa4);
   a = mc_w(mc_rad(mid_shifted)) + mc_mpysu(mc_rad(mid_shifted + 1), fb) - mc_mpysu(mc_rad(mid_shifted), fb);
   if (a > limit) a = limit;
   int32_t mid_a, mid_b, end_a, end_b;
   int32_t mid_gain = mc_pole_pair(cosine, sine, mc_sat(a), &mid_a, &mid_b);
   a = mc_w(mc_rad(shifted));
   if (a > limit) a = limit;
   int32_t end_gain = mc_pole_pair(mc_cos[step], mc_sin[step], mc_sat(a), &end_a, &end_b);
   if (drive >= 0) {
      mid_gain = mc_sat(mc_mpy(drive, mid_gain));
      end_gain = mc_sat(mc_mpy(drive, end_gain));
   }
   const int32_t eighth = 0x100000;
   mc_ramp(r->a, 8, s->a, mid_a - s->a, eighth);
   mc_ramp(r->a + 8, 8, mid_a, end_a - mid_a, eighth);
   mc_ramp(r->b, 8, s->b, mid_b - s->b, eighth);
   mc_ramp(r->b + 8, 8, mid_b, end_b - mid_b, eighth);
   mc_ramp(r->gain, 8, s->gain, mid_gain - s->gain, eighth);
   mc_ramp(r->gain + 8, 8, mid_gain, end_gain - mid_gain, eighth);
   s->a = end_a;
   s->b = end_b;
   s->gain = end_gain;
}

/* ---- one channel, one block --------------------------------------------- */

typedef struct {
   McRamp lp, hp;
   int32_t clip_gain;                 /* gain into the clipper */
   int32_t smooth_f[16], smooth_g[16];
   int32_t dc_t[16], dc_c[16];
} McBlock;

static void mc_channel(McChannel *s, const McBlock *k, int32_t *io) {
   /* EQ: all-pass mixed with the input (see mc_tables) */
   int32_t x[18], ap[18], e[19];
   x[0] = s->eq_in[0]; x[1] = s->eq_in[1];
   ap[0] = s->eq_ap[0]; ap[1] = s->eq_ap[1];
   memcpy(x + 2, io, 16 * sizeof *io);
   memcpy(e, s->up, sizeof s->up);
   for (int i = 0; i < 16; ++i) {
      const mc_acc a = mc_w(x[i]) + 2 * mc_mpy(mc_eq[0], x[i + 1]) + mc_mpy(mc_eq[1], x[i + 2])
                     - mc_mpy(ap[i], mc_eq[1]) - 2 * mc_mpy(mc_eq[0], ap[i + 1]);
      ap[i + 2] = mc_sat(mc_s56(a));
      e[3 + i] = mc_sat(mc_shl(mc_mpy(x[i + 2], mc_eq[2]) + mc_mpy(mc_eq[3], ap[i + 2]), 2));
   }
   s->eq_in[0] = x[16]; s->eq_in[1] = x[17];
   s->eq_ap[0] = ap[16]; s->eq_ap[1] = ap[17];

   /* 2x up: the input, two frames late, with 4-tap half-band values between */
   int32_t up[32];
   for (int i = 0; i < 16; ++i) {
      up[2 * i] = e[i + 1];
      up[2 * i + 1] = mc_sat(mc_mpy(-0x0ad743, e[i]) + mc_mpy(0x4a4df0, e[i + 1])
                           + mc_mpy(0x4a4df0, e[i + 2]) + mc_mpy(-0x0ad743, e[i + 3]));
   }
   memcpy(s->up, e + 16, sizeof s->up);

   /* low-pass: coupled-form resonator, driven through its second state. The
    * feedback reads the states as saturated 24-bit words, which is what
    * bounds a self-oscillating filter. */
   int32_t lp[34];
   lp[0] = s->hp_in[0]; lp[1] = s->hp_in[1];
   mc_acc a = s->lp_a, b = s->lp_b;
   for (int n = 0; n < 32; ++n) {
      const int j = n >> 1;
      const int32_t a24 = mc_sat(a), b24 = mc_sat(b);
      lp[2 + n] = a24;
      const mc_acc in = mc_shl((b >> 4) + mc_mpy(up[n], k->lp.gain[j]), 4);
      b = mc_s56(in + mc_mpy(k->lp.a[j], b24) - mc_mpy(k->lp.b[j], a24));
      a = mc_s56(a + mc_mpy(a24, k->lp.a[j]) + mc_mpy(b24, k->lp.b[j]));
   }
   s->lp_a = mc_sat48(a); s->lp_b = mc_sat48(b);
   s->hp_in[0] = lp[32]; s->hp_in[1] = lp[33];

   /* high-pass: the same resonator run as a low-pass and subtracted from the
    * input delayed to match, then the gain into the clipper and the clip.
    * The clip inverts the signal. */
   int32_t clip[32];
   a = s->hp_a; b = s->hp_b;
   for (int n = 0; n < 32; ++n) {
      const int j = n >> 1;
      const int32_t a24 = mc_sat(a), b24 = mc_sat(b);
      const int32_t hp = mc_sat(mc_shl(a + mc_mpy(0x100000, lp[n]), 7));
      b = mc_s56(b - mc_mpy(k->hp.gain[j], lp[2 + n]) + mc_mpy(k->hp.a[j], b24) - mc_mpy(k->hp.b[j], a24));
      a = mc_sat48(a) + mc_mpy(a24, k->hp.a[j]) + mc_mpy(b24, k->hp.b[j]);
      const mc_acc v = -2 * (mc_acc)mc_sat(mc_shl(mc_mpy(k->clip_gain, hp), 8));
      clip[n] = v > MC_ONE ? MC_ONE : v < -MC_ONE - 1 ? -MC_ONE - 1 : (int32_t)v;
   }
   s->hp_a = mc_sat48(a); s->hp_b = mc_sat48(b);

   /* two one-pole low-passes; each outputs its state from before the update */
   int32_t d[38];
   memcpy(d, s->down, sizeof s->down);
   mc_acc p = s->smooth[0], q = s->smooth[1];
   for (int n = 0; n < 32; ++n) {
      const int j = n >> 1;
      const int32_t p24 = mc_sat(p), q24 = mc_sat(q);
      d[5 + n] = q24;
      q = mc_s56(q + mc_mpy(k->smooth_f[j], p24) - mc_mpy(q24, k->smooth_f[j]));
      p = mc_s56(p + mc_mpy(k->smooth_g[j], clip[n]) - mc_mpy(p24, k->smooth_f[j]));
   }
   s->smooth[0] = mc_sat48(p); s->smooth[1] = mc_sat48(q);

   /* 2x down: 6-tap half-band, then two DC blockers */
   memcpy(s->down, d + 32, sizeof s->down);
   for (int i = 0; i < 16; ++i) {
      int32_t v = mc_sat(mc_mpy(-0x08ad89, d[2 * i]) + mc_mpy(0x0eeb88, d[2 * i + 1]) + mc_mpy(0x39c201, d[2 * i + 2])
                       + mc_mpy(0x39c201, d[2 * i + 3]) + mc_mpy(0x0eeb88, d[2 * i + 4]) + mc_mpy(-0x08ad89, d[2 * i + 5]));
      for (int stage = 0; stage < 2; ++stage) {
         const int32_t state = mc_sat(s->dc[stage]);
         s->dc[stage] = mc_s56(s->dc[stage] - mc_mpy(s->dc_in[stage], k->dc_c[i]) - mc_mpy(k->dc_t[i], state)
                               + mc_mpy(v, k->dc_c[i]));
         s->dc_in[stage] = v;
         v = mc_sat(s->dc[stage]);
      }
      io[i] = v;
   }
   s->dc[0] = mc_sat48(s->dc[0]); s->dc[1] = mc_sat48(s->dc[1]);
}

/* ---- public entry ------------------------------------------------------- */

enum { MC_BASE, MC_WIDTH, MC_HPQ, MC_LPQ, MC_DIST, MC_CONTROLS };

/* Process one block of 16 frames in place. ctl[] holds the five block words
 * (0 .. 0x7fffff; a knob at v sends v << 16, DIST centre is 64 << 16). Audio
 * is 24-bit signed. The output is the track signal after the filter section,
 * inverted, about 7 dB below the input at DIST centre. */
static void mono_core_block(MonoCore *m, const int32_t ctl[MC_CONTROLS], int32_t left[MC_FRAMES], int32_t right[MC_FRAMES]) {
   McBlock k;
   /* BASE places the high-pass, BASE + WIDTH the low-pass, 16 steps per unit */
   mc_acc a = mc_rnd(mc_mpy(0x800, ctl[MC_BASE]) - mc_w(0x80));
   if (a >= mc_w(0x700)) a = mc_w(0x700);
   const int32_t hp_target = mc_hi(a);
   a += mc_mpy(0x800, ctl[MC_WIDTH]);
   if (a < 0) a = 0;
   const int32_t lp_target = mc_hi(a);

   /* DIST below centre only turns the input down; above centre the input
    * gain stays at its maximum and the gain into the clipper rises. */
   a = mc_shl(mc_w(ctl[MC_DIST]), 1);
   const int32_t drive = mc_drive[mc_sat(a) >> 15];
   a -= mc_w(0x800000);
   if (a < 0) a = 0;
   const int32_t over = mc_sat(a);
   const int32_t headroom = 0x200000;   /* set per machine; this is THRU's */
   const int32_t post = mc_sat(mc_mpy(0x7deccd, mc_sat(mc_mpy(over, over))) + mc_w(0x021333));
   const int32_t comp = mc_sat(mc_mpy(mc_comp[over >> 15], headroom));
   k.clip_gain = mc_sat(mc_mpy(mc_div(mc_w(0x080000), headroom), post));

   mc_resonator(&m->lp, lp_target, ctl[MC_LPQ], -22938, drive, &k.lp);
   mc_resonator(&m->hp, hp_target, ctl[MC_HPQ], -9830, -1, &k.hp);

   const int32_t sixteenth = 0x080000;
   const int32_t f = mc_pole[lp_target > 0x6a3 ? 0x6a3 : lp_target];
   const mc_acc g = mc_mpy(comp, f);
   mc_ramp(k.smooth_f, 16, m->smooth_f, f - m->smooth_f, sixteenth);
   mc_ramp(k.smooth_g, 16, m->smooth_g, mc_sat(g - mc_w(m->smooth_g)), sixteenth);
   m->smooth_f = f;
   m->smooth_g = mc_sat(g);

   const int32_t t = mc_pole[(hp_target > 0x63f ? 0x63f : hp_target < 0 ? 0 : hp_target) + 128];
   a = (2 * mc_w(MC_ONE) - mc_w(t)) >> 1;
   mc_ramp(k.dc_c, 16, m->dc_c, mc_sat(a - mc_w(m->dc_c)), sixteenth);
   mc_ramp(k.dc_t, 16, m->dc_t, t - m->dc_t, sixteenth);
   m->dc_c = mc_sat(a);
   m->dc_t = t;

   mc_channel(&m->ch[0], &k, left);
   mc_channel(&m->ch[1], &k, right);
}

#endif
