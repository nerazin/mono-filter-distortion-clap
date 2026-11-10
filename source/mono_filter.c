#include <clap/clap.h>
#include <stdint.h>
/* Native filter reconstruction: see DSP section below. */
static void *filter_engine_create(double sample_rate);
static void filter_engine_destroy(void *engine);
static void filter_engine_reset(void *engine);
static uint32_t filter_engine_latency(const void *engine);
static void filter_engine_process(void *engine, const double input[2], const double controls[8], double output[2]);
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Mono Filter & Distortion, an independent response-matched reconstruction.
 * The DSP was calibrated against OS 1.32B running in Monomodule. No firmware
 * or emulator is distributed or needed. SPDX-License-Identifier: GPL-3.0-only */

#define NPARAM 8

enum { BASE, WDTH, HPQ, LPQ, DIST, INPUT, OUTPUT, MIX };
static const char *const names[NPARAM] = {"BASE", "WIDTH", "HP Q", "LP Q", "DIST", "Input", "Output", "Mix"};
static const double defaults[NPARAM] = {0, 127, 0, 0, 0, 0, 0, 100};
static const double minimums[NPARAM] = {0, 0, 0, 0, -64, -24, -24, 0};
static const double maximums[NPARAM] = {127, 127, 127, 127, 63, 24, 24, 100};

static const char *const features[] = {
   CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_FILTER, CLAP_PLUGIN_FEATURE_DISTORTION,
   CLAP_PLUGIN_FEATURE_STEREO, NULL
};
static const clap_plugin_descriptor_t descriptor = {
   .clap_version = CLAP_VERSION_INIT,
   .id = "works.reconstruction.monofilterdist",
   .name = "Mono Filter Distortion",
   .vendor = "Independent DSP",
   .url = "", .manual_url = "", .support_url = "",
   .version = "0.2.0",
   .description = "Stereo filter and distortion inspired by the Monomachine",
   .features = features
};

typedef struct {
   clap_plugin_t plugin;
   const clap_host_t *host;
   _Atomic uint64_t base_bits[NPARAM];
   double modulation[NPARAM];
   void *engine;
   uint32_t latency;
} FilterPlugin;

static uint64_t double_bits(double x) {
   uint64_t u;
   memcpy(&u, &x, sizeof u);
   return u;
}
static double bits_double(uint64_t u) {
   double x;
   memcpy(&x, &u, sizeof x);
   return x;
}
static double base_value(const FilterPlugin *s, unsigned id) {
   return bits_double(atomic_load_explicit(&s->base_bits[id], memory_order_relaxed));
}
static void set_base(FilterPlugin *s, unsigned id, double x) {
   atomic_store_explicit(&s->base_bits[id], double_bits(x), memory_order_relaxed);
}
static double clamp(double x, double lo, double hi) {
   return x < lo ? lo : x > hi ? hi : x;
}
static double effective(const FilterPlugin *s, unsigned id) {
   return clamp(base_value(s, id) + s->modulation[id], minimums[id], maximums[id]);
}

static bool plug_init(const clap_plugin_t *p) {
   (void)p;
   return true;
}
static void plug_destroy(const clap_plugin_t *p) {
   FilterPlugin *s = p->plugin_data;
   filter_engine_destroy(s->engine);
   free(s);
}
static bool plug_activate(const clap_plugin_t *p, double rate, uint32_t min, uint32_t max) {
   (void)min;
   (void)max;
   FilterPlugin *s = p->plugin_data;
   if (s->engine) return false;
   s->engine = filter_engine_create(rate);
   if (!s->engine) return false;
   const uint32_t previous = s->latency;
   s->latency = filter_engine_latency(s->engine);
   if (s->latency != previous && s->host && s->host->get_extension) {
      const clap_host_latency_t *host_latency = s->host->get_extension(s->host, CLAP_EXT_LATENCY);
      if (host_latency && host_latency->changed) host_latency->changed(s->host);
   }
   return true;
}
static void plug_deactivate(const clap_plugin_t *p) {
   FilterPlugin *s = p->plugin_data;
   filter_engine_destroy(s->engine);
   s->engine = NULL;
}
static bool plug_start(const clap_plugin_t *p) {
   (void)p;
   return true;
}
static void plug_stop(const clap_plugin_t *p) {
   (void)p;
}
static void plug_reset(const clap_plugin_t *p) {
   FilterPlugin *s = p->plugin_data;
   filter_engine_reset(s->engine);
}
static void plug_on_main(const clap_plugin_t *p) {
   (void)p;
}

static void event(FilterPlugin *s, const clap_event_header_t *h) {
   if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
   if (h->type == CLAP_EVENT_PARAM_VALUE && h->size >= sizeof(clap_event_param_value_t)) {
      const clap_event_param_value_t *e = (const void *)h;
      if (e->param_id < NPARAM && e->note_id == -1 && e->port_index == -1 &&
          e->channel == -1 && e->key == -1 && isfinite(e->value))
         set_base(s, e->param_id, clamp(e->value, minimums[e->param_id], maximums[e->param_id]));
   } else if (h->type == CLAP_EVENT_PARAM_MOD && h->size >= sizeof(clap_event_param_mod_t)) {
      const clap_event_param_mod_t *e = (const void *)h;
      if (e->param_id < NPARAM && e->note_id == -1 && e->port_index == -1 &&
          e->channel == -1 && e->key == -1 && isfinite(e->amount))
         s->modulation[e->param_id] = clamp(e->amount, -127, 127);
   }
}

static clap_process_status plug_process(const clap_plugin_t *p, const clap_process_t *pr) {
   FilterPlugin *s = p->plugin_data;
   if (!s->engine || !pr || pr->audio_inputs_count < 1 || pr->audio_outputs_count < 1 ||
       !pr->audio_inputs || !pr->audio_outputs) return CLAP_PROCESS_ERROR;
   const clap_audio_buffer_t *in = &pr->audio_inputs[0];
   clap_audio_buffer_t *out = &pr->audio_outputs[0];
   if (in->channel_count < 2 || out->channel_count < 2 ||
       ((!in->data32 || !out->data32) && (!in->data64 || !out->data64))) return CLAP_PROCESS_ERROR;
   const clap_input_events_t *events = pr->in_events;
   const uint32_t count = events && events->size && events->get ? events->size(events) : 0;
   uint32_t next = 0;
   for (uint32_t frame = 0; frame < pr->frames_count; ++frame) {
      while (next < count) {
         const clap_event_header_t *header = events->get(events, next);
         if (header && header->time > frame) break;
         event(s, header);
         ++next;
      }
      double input[2], output[2], controls[NPARAM];
      if (in->data32) {
         input[0] = in->data32[0][frame];
         input[1] = in->data32[1][frame];
      } else {
         input[0] = in->data64[0][frame];
         input[1] = in->data64[1][frame];
      }
      for (unsigned i = 0; i < NPARAM; ++i) controls[i] = effective(s, i);
      filter_engine_process(s->engine, input, controls, output);
      if (out->data32) {
         out->data32[0][frame] = (float)output[0];
         out->data32[1][frame] = (float)output[1];
      } else {
         out->data64[0][frame] = output[0];
         out->data64[1][frame] = output[1];
      }
   }
   out->constant_mask = 0;
   return CLAP_PROCESS_CONTINUE;
}

static uint32_t ports_count(const clap_plugin_t *p, bool input) {
   (void)p;
   (void)input;
   return 1;
}
static bool ports_get(const clap_plugin_t *p, uint32_t i, bool input, clap_audio_port_info_t *info) {
   (void)p;
   if (i || !info) return false;
   memset(info, 0, sizeof *info);
   info->id = 0;
   snprintf(info->name, sizeof info->name, "%s", input ? "Stereo In":"Stereo Out");
   info->flags = CLAP_AUDIO_PORT_IS_MAIN|CLAP_AUDIO_PORT_SUPPORTS_64BITS|CLAP_AUDIO_PORT_REQUIRES_COMMON_SAMPLE_SIZE;
   info->channel_count = 2;
   info->port_type = CLAP_PORT_STEREO;
   info->in_place_pair = 0;
   return true;
}
static const clap_plugin_audio_ports_t ports = {ports_count, ports_get};

static uint32_t params_count(const clap_plugin_t *p) {
   (void)p;
   return NPARAM;
}
static bool params_info(const clap_plugin_t *p, uint32_t i, clap_param_info_t *info) {
   (void)p;
   if (i >= NPARAM || !info) return false;
   memset(info, 0, sizeof *info);
   info->id = i;
   info->flags = CLAP_PARAM_IS_AUTOMATABLE|CLAP_PARAM_IS_MODULATABLE|CLAP_PARAM_REQUIRES_PROCESS;
   snprintf(info->name, sizeof info->name, "%s", names[i]);
   info->min_value = minimums[i];
   info->max_value = maximums[i];
   info->default_value = defaults[i];
   return true;
}
static bool params_get(const clap_plugin_t *p, clap_id id, double *v) {
   if (id >= NPARAM || !v) return false;
   *v=base_value(p->plugin_data,id); return true;
}
static bool params_to_text(const clap_plugin_t *p, clap_id id, double v, char *out, uint32_t cap) {
   (void)p;
   if (id >= NPARAM || !out || !cap || !isfinite(v)) return false;
   if (id == INPUT || id == OUTPUT) snprintf(out, cap, "%+.1f dB", v);
   else if (id == MIX) snprintf(out, cap, "%.1f %%", v);
   else if (id == DIST) snprintf(out, cap, "%+.1f", v);
   else snprintf(out, cap, "%.1f", v);
   return true;
}
static bool params_from_text(const clap_plugin_t *p, clap_id id, const char *text, double *v) {
   (void)p;
   if (id >= NPARAM || !text || !v) return false;
   char *end = NULL;
   double x = strtod(text, &end);
   if (end == text || !isfinite(x)) return false;
   *v=clamp(x,minimums[id],maximums[id]); return true;
}
static void params_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out) {
   (void)out;
   if (!in || !in->size || !in->get) return;
   uint32_t n = in->size(in);
   for (uint32_t i = 0; i < n; ++i) event(p->plugin_data, in->get(in, i));
}
static const clap_plugin_params_t params = {params_count, params_info, params_get, params_to_text, params_from_text, params_flush};

static bool stream_write(const clap_ostream_t *s, const void *buf, uint64_t n) {
   const uint8_t *p = buf;
   while (n) {
      int64_t done = s->write(s, p, n);
      if (done <= 0 || (uint64_t)done > n) return false;
      p += done;
      n -= (uint64_t)done;
   }
   return true;
}
static bool stream_read(const clap_istream_t *s, void *buf, uint64_t n) {
   uint8_t *p = buf;
   while (n) {
      int64_t done = s->read(s, p, n);
      if (done <= 0 || (uint64_t)done > n) return false;
      p += done;
      n -= (uint64_t)done;
   }
   return true;
}
static bool state_save(const clap_plugin_t *p, const clap_ostream_t *out) {
   if (!out || !out->write) return false;
   FilterPlugin *s = p->plugin_data;
   const char magic[8] = {'M', 'F', 'I', 'L', 'T', 0, 0, 1};
   if (!stream_write(out, magic, sizeof magic)) return false;
   for (unsigned i = 0; i < NPARAM; ++i) {
      uint64_t u = double_bits(base_value(s, i));
      uint8_t bytes[8];
      for (unsigned j = 0; j < 8; ++j) bytes[j] = (uint8_t)(u>>(8*j));
      if (!stream_write(out, bytes, 8)) return false;
   }
   return true;
}
static bool state_load(const clap_plugin_t *p, const clap_istream_t *in) {
   if (!in || !in->read) return false;
   FilterPlugin *s = p->plugin_data;
   char magic[8];
   double v[NPARAM];
   const char expected[8] = {'M', 'F', 'I', 'L', 'T', 0, 0, 1};
   if (!stream_read(in, magic, 8) || memcmp(magic, expected, 8)) return false;
   for (unsigned i = 0; i < NPARAM; ++i) {
      uint8_t bytes[8];
      uint64_t u = 0;
      if (!stream_read(in, bytes, 8)) return false;
      for (unsigned j = 0; j < 8; ++j) u|=(uint64_t)bytes[j]<<(8*j);
      v[i] = bits_double(u);
      if (!isfinite(v[i]) || v[i] < minimums[i] || v[i] > maximums[i]) return false;
   }
   for (unsigned i = 0; i < NPARAM; ++i) set_base(s, i, v[i]);
   if (s->host && s->host->get_extension) {
      const clap_host_params_t *hp = s->host->get_extension(s->host, CLAP_EXT_PARAMS);
      if (hp && hp->rescan) hp->rescan(s->host, CLAP_PARAM_RESCAN_VALUES);
   }
   return true;
}
static const clap_plugin_state_t state = {state_save, state_load};

static uint32_t remote_count(const clap_plugin_t *p) {
   (void)p;
   return 1;
}
static bool remote_get(const clap_plugin_t *p, uint32_t i, clap_remote_controls_page_t *page) {
   (void)p;
   if (i || !page) return false;
   memset(page, 0, sizeof *page);
   page->page_id = 0;
   snprintf(page->section_name, sizeof page->section_name, "Filter & Drive");
   snprintf(page->page_name, sizeof page->page_name, "Main");
   for (unsigned k = 0; k < NPARAM; ++k) page->param_ids[k] = k;
   return true;
}
static const clap_plugin_remote_controls_t remote = {remote_count, remote_get};
static uint32_t tail_get(const clap_plugin_t *p) {
   (void)p;
   return UINT32_MAX;
}
static const clap_plugin_tail_t tail = {tail_get};
static uint32_t latency_get(const clap_plugin_t *p) {
   const FilterPlugin *s = p->plugin_data;
   return s->latency;
}
static const clap_plugin_latency_t latency = {latency_get};

static const void *plug_extension(const clap_plugin_t *p, const char *id) {
   (void)p;
   if (!id) return NULL;
   if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &ports;
   if (!strcmp(id, CLAP_EXT_PARAMS)) return &params;
   if (!strcmp(id, CLAP_EXT_STATE)) return &state;
   if (!strcmp(id, CLAP_EXT_REMOTE_CONTROLS) || !strcmp(id, CLAP_EXT_REMOTE_CONTROLS_COMPAT)) return &remote;
   if (!strcmp(id, CLAP_EXT_TAIL)) return &tail;
   if (!strcmp(id, CLAP_EXT_LATENCY)) return &latency;
   return NULL;
}
static uint32_t factory_count(const clap_plugin_factory_t *f) {
   (void)f;
   return 1;
}
static const clap_plugin_descriptor_t *factory_desc(const clap_plugin_factory_t *f, uint32_t i) {
   (void)f;
   return i ? NULL : &descriptor;
}
static const clap_plugin_t *factory_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id) {
   (void)f;
   if (!host || !id || strcmp(id, descriptor.id) || !clap_version_is_compatible(host->clap_version)) return NULL;
   FilterPlugin *s = calloc(1, sizeof *s);
   if (!s) return NULL;
   s->host = host;
   for (unsigned i = 0; i < NPARAM; ++i) atomic_init(&s->base_bits[i], double_bits(defaults[i]));
   s->plugin = (clap_plugin_t) {
      .desc = &descriptor, .plugin_data = s, .init = plug_init, .destroy = plug_destroy,
      .activate = plug_activate, .deactivate = plug_deactivate, .start_processing = plug_start,
      .stop_processing = plug_stop, .reset = plug_reset, .process = plug_process,
      .get_extension = plug_extension, .on_main_thread = plug_on_main
   };
   return &s->plugin;
}
static const clap_plugin_factory_t factory = {factory_count, factory_desc, factory_create};
static bool entry_init(const char *path) {
   (void)path;
   return true;
}
static void entry_deinit(void) {
}
static const void *entry_factory(const char *id) {
   return id && !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &factory : NULL;
}
CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_factory};

/* -------------------------------------------------------------------------
 * Native DSP, independent of the emulator and firmware.
 *
 * The reference is the Monomodule THRU track with 1.32B, both key-track
 * flags off, filter and amp envelopes held, and the effect/send pages neutral.
 * The model uses two resonant sections and three one-poles, with the drive
 * between the first low-pass and the high-pass. The filter runs at twice the
 * internal sample rate, using interpolation and decimation coefficients
 * identified in the reference trace. The filter sections remain a fitted
 * floating-point model, not the original instruction-level algorithm.
 * Fixed-point arithmetic and nonlinear resonance details remain approximate.
 *
 * The topology, measured drive control points, and initial resonance curves
 * also draw on Sequencist's GPL-3.0-only MONO2K calibration; see README.
 * ------------------------------------------------------------------------- */
#define ENGINE_NATIVE_RATE 44100.0
#define ENGINE_FILTER_RATE 88200.0
#define ENGINE_DSP_LATENCY 8
#define ENGINE_PI 3.14159265358979323846264338327950288
#define ENGINE_HISTORY 4096
#define ENGINE_PHASES 128
#define ENGINE_QUEUE 8192

typedef struct { double x, y, ctl[NPARAM]; } AudioFrame;
typedef struct {
   float *kernel;
   AudioFrame history[ENGINE_HISTORY];
   double ratio;
   int half, taps;
   int64_t last;
   uint64_t output_index;
} RateConverter;

typedef struct { double s1, s2, a1, a2, a3, k; } TwoPole;
typedef struct { double s, g; } OnePole;
typedef struct {
   TwoPole lp, hp;
   OnePole hp1, lp1, lp2;
   double input_history[4], down_history[6], wet_delay[5], dry_delay[8];
} FilterChannel;
typedef struct {
   double rate;
   bool direct, initialized;
   uint32_t latency, queue_start;
   uint64_t host_frames, read_pos, write_pos, native_frames;
   double control[NPARAM], drive, ceiling, input_gain, output_gain, mix, tone_gain;
   FilterChannel channel[2];
   RateConverter in_rate, out_rate;
   struct { double left, right; } queue[ENGINE_QUEUE];
} FilterEngine;

static bool rate_prepare(RateConverter *r, double source, double dest) {
   r->ratio = source / dest;
   r->half = (int)ceil(40.0 * fmax(1.0, r->ratio));
   r->taps = 2 * r->half;
   if (r->taps + 2 >= ENGINE_HISTORY) return false;
   r->kernel = calloc((size_t)(ENGINE_PHASES + 1) * r->taps, sizeof(float));
   if (!r->kernel) return false;
   const double cutoff = 0.95 * fmin(1.0, dest / source);
   for (int p = 0; p <= ENGINE_PHASES; ++p) {
      const double frac = (double)p / ENGINE_PHASES;
      double sum = 0.0;
      for (int k = 0; k < r->taps; ++k) {
         const double d = (double)(k - r->half + 1) - frac;
         const double x = d / r->half;
         const double window = fabs(x) <= 1.0
            ? 0.42 + 0.5 * cos(ENGINE_PI*x) + 0.08 * cos(2.0*ENGINE_PI*x) : 0.0;
         const double u = ENGINE_PI * cutoff * d;
         const double sinc = fabs(u) < 1e-14 ? 1.0 : sin(u) / u;
         const double v = cutoff * sinc * window;
         r->kernel[(size_t)p * r->taps + k] = (float)v;
         sum += v;
      }
      if (sum != 0.0)
         for (int k = 0; k < r->taps; ++k)
            r->kernel[(size_t)p * r->taps + k] /= (float)sum;
   }
   return true;
}
static void rate_reset(RateConverter *r) {
   memset(r->history, 0, sizeof r->history);
   r->last = -1;
   r->output_index = 0;
}
static void rate_push(RateConverter *r, const AudioFrame *f) {
   r->history[(size_t)(++r->last) & (ENGINE_HISTORY - 1)] = *f;
}
static bool rate_ready(const RateConverter *r) {
   return (int64_t)floor((double)r->output_index * r->ratio) + r->half <= r->last;
}
static AudioFrame rate_pop(RateConverter *r) {
   const double pos = (double)(r->output_index++) * r->ratio;
   const int64_t base = (int64_t)floor(pos);
   const double pp = (pos - (double)base) * ENGINE_PHASES;
   const int phase = (int)fmin(ENGINE_PHASES - 1, (int)pp);
   const double blend = pp - phase;
   const float *a = r->kernel + (size_t)phase * r->taps;
   const float *b = a + r->taps;
   AudioFrame result = {0};
   for (int k = 0; k < r->taps; ++k) {
      const int64_t index = base - r->half + 1 + k;
      if (index < 0) continue;
      const AudioFrame *f = &r->history[(size_t)index & (ENGINE_HISTORY - 1)];
      const double weight = a[k] + (b[k] - a[k]) * blend;
      result.x += f->x * weight;
      result.y += f->y * weight;
   }
   if (base >= 0) {
      const AudioFrame *f = &r->history[(size_t)base & (ENGINE_HISTORY - 1)];
      memcpy(result.ctl, f->ctl, sizeof result.ctl);
   }
   return result;
}

typedef struct { double x, y; } ControlPoint;
static double between(const ControlPoint *points, int n, double x, bool logarithmic) {
   if (x <= points[0].x) return points[0].y;
   for (int i = 1; i < n; ++i) {
      if (x <= points[i].x) {
         const double t = (x - points[i-1].x) / (points[i].x - points[i-1].x);
         if (logarithmic) return exp(log(points[i-1].y) + t * log(points[i].y / points[i-1].y));
         return points[i-1].y + t * (points[i].y - points[i-1].y);
      }
   }
   return points[n-1].y;
}
#define CURVE(a,x,log) between((a), (int)(sizeof(a)/sizeof((a)[0])), (x), (log))

/* The low- and high-Q response varies with cutoff in the hardware. These
 * curves are a compromise, calibrated using separate low-level sweeps. */
/* LP section frequency, resonance Q, one-pole frequency and gain, fitted
 * from the existing 1.32B captures. Width 120 is the open-filter region.
 * 48 and 80 resonance positions were held out during calibration. */
static const double lp_table[6][6][4] = {
   { /* cutoff position 40 */
      {87.3926849, 0.711382172, 78.6081699, 0.964233406},
      {62.0226001, 1.72345529, 83.350708, 0.9545352},
      {58.5621992, 5.05613534, 90.2625991, 0.923452619},
      {58.211696, 35, 94.83928, 0.93},
      {58.211696, 187, 94.83928, 0.93},
      {58.211696, 1000, 94.83928, 0.93},
   },
   { /* cutoff position 56 */
      {290.1424, 0.731755261, 430.306882, 0.910895979},
      {240.940504, 1.71375189, 390.107122, 0.908486305},
      {233.808626, 4.8619248, 382.816032, 0.915261881},
      {233.005881, 41.9354562, 387.107853, 0.910789606},
      {233.071209, 354.111327, 467.520514, 0.87565548},
      {233.071209, 1000, 467.520514, 0.87565548},
   },
   { /* cutoff position 72 */
      {1042.91078, 0.802924086, 1873.49062, 0.93021304},
      {952.142922, 1.78309368, 1522.34012, 0.938619415},
      {932.746455, 4.81790154, 1486.19221, 0.959206424},
      {931.625187, 37.5089083, 1525.92596, 0.970207608},
      {932.261293, 303.046345, 1471.51255, 0.989704791},
      {932.261293, 1000, 1471.51255, 0.989704791},
   },
   { /* cutoff position 88 */
      {4451.78276, 0.650816208, 8217.87863, 0.96544568},
      {3895.44435, 1.43964122, 6699.31432, 0.958225486},
      {3749.48392, 3.89255653, 6448.40244, 0.937915607},
      {3729.01562, 29.9801525, 6304.19579, 0.921829981},
      {3729.23943, 231.015765, 6387.84413, 0.911005981},
      {3729.23943, 1000, 6387.84413, 0.911005981},
   },
   { /* cutoff position 104 */
      {20981.1754, 0.509586335, 34271.6727, 0.959461679},
      {16284.6185, 1.21386, 26566.6491, 0.958263176},
      {15188.0404, 2.45794725, 33577.797, 0.949653996},
      {14940.2866, 13.1277503, 36858.6644, 0.928920801},
      {14918.9382, 100, 36836.8845, 0.929},
      {14918.9382, 1000, 36836.8845, 0.929},
   },
   { /* cutoff position 120 */
      {28632.5199, 0.933841914, 30851.4808, 0.949759086},
      {28632.5026, 0.933831477, 30851.5902, 0.949759073},
      {28632.5495, 0.933859815, 30851.293, 0.949759107},
      {28632.5409, 0.933852404, 30851.3706, 0.949759097},
      {28632.5464, 0.933866181, 30851.2296, 0.949759088},
      {28632.5464, 0.933866181, 30851.2296, 0.949759088},
   },
};
/* Fitted high-pass two-pole frequency, Q, one-pole frequency and gain. */
static const double hp_table[4][6][4] = {
   { /* BASE 40 */
      {99.8623489, 0.726875244, 50.3777086, 0.95500742},
      {61.2470717, 1.38707274, 132.1994, 0.96055324},
      {59.8321231, 4.48700739, 164.975754, 0.969364787},
      {58.3496955, 31.0508149, 149.321591, 0.963964792},
      {58.0154768, 190, 143.89408, 0.95},
      {58.0154768, 1200, 143.89408, 0.95},
   },
   { /* BASE 56 */
      {367.343929, 0.751704739, 212.068303, 0.949045943},
      {258.431289, 1.71148942, 605.13825, 0.965949299},
      {239.249862, 4.79592201, 741.327086, 0.982719873},
      {233.442476, 39.1395526, 749.152969, 0.985618681},
      {233.10841, 250, 667.14528, 0.96},
      {233.10841, 1200, 667.14528, 0.96},
   },
   { /* BASE 72 */
      {1107.57304, 0.806741712, 1750.7025, 0.959772788},
      {998.937048, 1.81845162, 2661.4769, 0.979658783},
      {948.383037, 4.76605956, 3009.20547, 0.999357933},
      {933.578056, 38.4563093, 3053.10628, 1.00517534},
      {932.263753, 250.570278, 2664.38134, 0.983943908},
      {932.263753, 1200, 2664.38134, 0.983943908},
   },
   { /* BASE 88 */
      {5269.51837, 0.754674189, 4745.96617, 0.925369153},
      {4105.31387, 1.71004294, 10411.3623, 1.02934724},
      {3811.33068, 4.32400161, 11925.7073, 1.09445633},
      {3734.67826, 33.0646774, 11660.6512, 1.10117205},
      {3729.49886, 223.107804, 8673.82017, 0.928307265},
      {3729.49886, 1200, 8673.82017, 0.928307265},
   },
};
static const ControlPoint hp_q[] = {
   {0,.75},{8,1.1},{16,1.9},{24,3.1},{32,5.1},{40,8.5},{48,14},
   {56,23},{64,38},{72,55},{80,85},{88,140},{96,190},{104,340},
   {112,486},{120,800},{127,1200}
};
static const ControlPoint hp_freq[] = {
   {0,1.17},{8,1.08},{16,1.02},{24,.98},{32,.95},{40,.93},{48,.91},{64,.895},{127,.895}
};
static const ControlPoint hp_one[] = {
   {0,1.42},{8,1.9},{16,2.35},{24,2.6},{32,2.75},{48,2.8},{64,2.65},{127,2.65}
};
static const ControlPoint dist_gain[] = {{64,0},{72,0},{76,1.95},{80,4.58},{84,7.78},{88,10.55}};
static const ControlPoint dist_ceil[] = {{0,0},{64,0},{68,-1.85},{72,-5.79},{76,-8.06},{80,-9.03},{127,-9.03}};

static double filter_freq(double hz) { return clamp(hz, 0.25, 43800.0); }
static void two_pole_set(TwoPole *f, double hz, double q) {
   const double g = tan(ENGINE_PI * filter_freq(hz) / ENGINE_FILTER_RATE);
   f->k = 1.0 / q;
   f->a1 = 1.0 / (1.0 + g * (g + f->k));
   f->a2 = g * f->a1;
   f->a3 = g * f->a2;
}
static void one_pole_set(OnePole *f, double hz) {
   const double g = tan(ENGINE_PI * filter_freq(hz) / ENGINE_FILTER_RATE);
   f->g = g / (1.0 + g);
}
/* Interpolation in log frequency and log Q keeps filter motion smooth. */
static void lp_parameters(double position, double resonance, double out[4]) {
   static const double qs[6] = {0, 16, 32, 64, 96, 127};
   const double p = clamp(position, 40.0, 120.0);
   const double q = clamp(resonance, 0.0, 127.0);
   const int i = (int)fmin(4.0, floor((p - 40.0) / 16.0));
   const double wp = (p - (40.0 + 16.0 * i)) / 16.0;
   int j = 0;
   while (j < 4 && q > qs[j + 1]) ++j;
   const double wq = (q - qs[j]) / (qs[j+1] - qs[j]);
   for (int k = 0; k < 4; ++k) {
      const double a = log(lp_table[i][j][k]) * (1-wq) + log(lp_table[i][j+1][k]) * wq;
      const double b = log(lp_table[i+1][j][k]) * (1-wq) + log(lp_table[i+1][j+1][k]) * wq;
      out[k] = exp(a * (1-wp) + b * wp);
   }
   if (position < 40.0) {
      const double factor = exp2((position - 40.0) / 8.0);
      out[0] *= factor;
      out[2] *= factor;
   }
}
static void hp_parameters(double position, double resonance, double out[4]) {
   static const double qs[6] = {0, 16, 32, 64, 96, 127};
   const double p = clamp(position, 40.0, 88.0);
   const double q = clamp(resonance, 0.0, 127.0);
   const int i = (int)fmin(2.0, floor((p - 40.0) / 16.0));
   const double wp = (p - (40.0 + 16.0 * i)) / 16.0;
   int j = 0;
   while (j < 4 && q > qs[j + 1]) ++j;
   const double wq = (q - qs[j]) / (qs[j+1] - qs[j]);
   for (int k = 0; k < 4; ++k) {
      const double a = log(hp_table[i][j][k]) * (1-wq) + log(hp_table[i][j+1][k]) * wq;
      const double b = log(hp_table[i+1][j][k]) * (1-wq) + log(hp_table[i+1][j+1][k]) * wq;
      out[k] = exp(a * (1-wp) + b * wp);
   }
   if (position < 40.0 || position > 88.0) {
      const double d = position < 40.0 ? position - 40.0 : position - 88.0;
      const double factor = exp2(d / 8.0);
      out[0] *= factor;
      out[2] *= factor;
   }
}
static void coefficients(FilterEngine *e) {
   const double base = e->control[BASE], width = e->control[WDTH];
   const double hq = e->control[HPQ], lq = e->control[LPQ];
   double lp[4], hp[4];
   lp_parameters(base + width, lq, lp);
   hp_parameters(base, hq, hp);
   const double mix_fit = .5 * clamp((base - 24.0)/16.0, 0.0, 1.0);
   const double corner = 8.1758 * exp2((base - 16.0) / 8.0);
   const double nominal[4] = {
      corner * CURVE(hp_freq, hq, false), CURVE(hp_q, hq, true),
      corner * CURVE(hp_one, hq, false), .94976
   };
   for (int k = 0; k < 4; ++k)
      hp[k] = exp(log(nominal[k]) * (1-mix_fit) + log(hp[k]) * mix_fit);
   for (int c = 0; c < 2; ++c) {
      FilterChannel *v = &e->channel[c];
      two_pole_set(&v->lp, lp[0], lp[1]);
      two_pole_set(&v->hp, hp[0], hp[1]);
      one_pole_set(&v->hp1, hp[2]);
      one_pole_set(&v->lp1, lp[2]);
      one_pole_set(&v->lp2, lp[2]);
   }
   e->tone_gain = lp[3] * hp[3] / .94976;
   const double raw = e->control[DIST] + 64.0;
   const double db = raw < 64 ? -1.35 * (64 - raw) + .34 :
                     raw >= 96 ? 15.51 + (raw - 96) * .370 :
                     raw >= 88 ? 10.55 + (raw - 88) * .62 : CURVE(dist_gain, raw, false);
   e->drive = pow(10.0, db / 20.0);
   e->ceiling = .54 * (1.0 + .033 * clamp((raw - 96.0) / 31.0, 0.0, 1.0))
                * pow(10.0, CURVE(dist_ceil, raw, false) / 20.0);
   e->input_gain = pow(10.0, e->control[INPUT] / 20.0);
   e->output_gain = pow(10.0, e->control[OUTPUT] / 20.0);
   e->mix = e->control[MIX] / 100.0;
}
static double two_pole_tick(TwoPole *f, double x, bool high, double limit) {
   const double v3 = x - f->s2;
   const double v1 = f->a1 * f->s1 + f->a2 * v3;
   const double v2 = f->s2 + f->a2 * f->s1 + f->a3 * v3;
   f->s1 = clamp(2 * v1 - f->s1, -limit, limit);
   f->s2 = clamp(2 * v2 - f->s2, -limit, limit);
   if (fabs(f->s1) < 1e-25) f->s1 = 0;
   if (fabs(f->s2) < 1e-25) f->s2 = 0;
   return high ? x - f->k * v1 - v2 : v2;
}
static double one_pole_tick(OnePole *f, double x) {
   const double v = (x - f->s) * f->g;
   const double y = v + f->s;
   f->s = y + v;
   if (fabs(f->s) < 1e-25) f->s = 0;
   return y;
}
static void queue_push(FilterEngine *e, double l, double r) {
   if (e->write_pos - e->read_pos >= ENGINE_QUEUE) return;
   const size_t i = (size_t)(e->write_pos++) & (ENGINE_QUEUE - 1);
   e->queue[i].left = l;
   e->queue[i].right = r;
}
static void native_frame(FilterEngine *e, const AudioFrame *frame) {
   if (!e->initialized) {
      memcpy(e->control, frame->ctl, sizeof e->control);
      coefficients(e);
      e->initialized = true;
   }
   if ((e->native_frames++ & 15u) == 0) {
      bool moved = false;
      for (int i = 0; i < NPARAM; ++i) {
         const double diff = frame->ctl[i] - e->control[i];
         if (fabs(diff) > 1e-7) { e->control[i] += diff * .00903; moved = true; }
      }
      if (moved) coefficients(e);
   }
   const double dry[2] = {frame->x, frame->y};
   double out[2];
   static const double down[6] = {
      -0.06779587268829346, 0.1165628433227539, 0.45123302936553955,
       0.45123302936553955, 0.1165628433227539, -0.06779587268829346
   };
   for (int c = 0; c < 2; ++c) {
      FilterChannel *v = &e->channel[c];
      memmove(v->input_history, v->input_history+1, 3*sizeof(double));
      v->input_history[3] = dry[c];
      const double *h = v->input_history;
      const double over[2] = {
         h[1],
         -0.0846942663192749 * (h[0] + h[3])
         + 0.5805034637451172 * (h[1] + h[2])
      };
      for (int i = 0; i < 2; ++i) {
         const double driven = clamp(over[i] * e->input_gain * e->drive, -64, 64);
         double x = two_pole_tick(&v->lp, driven, false, 16.0);
         x = clamp(x, -e->ceiling, e->ceiling);
         x = two_pole_tick(&v->hp, x, true, e->ceiling * 2.0);
         x -= one_pole_tick(&v->hp1, x);
         x = one_pole_tick(&v->lp2, one_pole_tick(&v->lp1, x));
         memmove(v->down_history, v->down_history+1, 5*sizeof(double));
         v->down_history[5] = x;
      }
      double wet = 0;
      for (int i = 0; i < 6; ++i) wet += down[i] * v->down_history[5-i];
      const double delayed_wet = v->wet_delay[0];
      memmove(v->wet_delay, v->wet_delay+1, 4*sizeof(double));
      v->wet_delay[4] = wet * e->tone_gain;
      const double delayed_dry = v->dry_delay[0];
      memmove(v->dry_delay, v->dry_delay+1, 7*sizeof(double));
      v->dry_delay[7] = dry[c];
      out[c] = ((1.0 - e->mix) * delayed_dry + e->mix * delayed_wet) * e->output_gain;
      if (!isfinite(out[c])) out[c] = 0;
   }
   if (e->direct) queue_push(e, out[0], out[1]);
   else {
      AudioFrame v = {0}; v.x = out[0]; v.y = out[1];
      rate_push(&e->out_rate, &v);
      while (rate_ready(&e->out_rate)) {
         const AudioFrame result = rate_pop(&e->out_rate);
         queue_push(e, result.x, result.y);
      }
   }
}
static void filter_engine_reset(void *engine) {
   FilterEngine *e = engine;
   if (!e) return;
   e->initialized = false;
   e->native_frames = e->host_frames = e->read_pos = e->write_pos = 0;
   memset(e->channel, 0, sizeof e->channel);
   rate_reset(&e->in_rate);
   rate_reset(&e->out_rate);
}
static void *filter_engine_create(double rate) {
   if (!isfinite(rate) || rate < 1000 || rate > 768000) return NULL;
   FilterEngine *e = calloc(1, sizeof *e);
   if (!e) return NULL;
   e->rate = rate;
   e->direct = rate == ENGINE_NATIVE_RATE;
   e->latency = ENGINE_DSP_LATENCY;
   if (!e->direct) {
      if (!rate_prepare(&e->in_rate, rate, ENGINE_NATIVE_RATE) ||
          !rate_prepare(&e->out_rate, ENGINE_NATIVE_RATE, rate)) {
         free(e->in_rate.kernel); free(e->out_rate.kernel); free(e); return NULL;
      }
      e->queue_start = (uint32_t)(e->in_rate.half + ceil(e->out_rate.half * rate / ENGINE_NATIVE_RATE) + 2);
      e->latency = e->queue_start + (uint32_t)llround(ENGINE_DSP_LATENCY * rate / ENGINE_NATIVE_RATE);
   }
   filter_engine_reset(e);
   return e;
}
static void filter_engine_destroy(void *engine) {
   FilterEngine *e = engine;
   if (!e) return;
   free(e->in_rate.kernel); free(e->out_rate.kernel); free(e);
}
static uint32_t filter_engine_latency(const void *engine) {
   const FilterEngine *e = engine;
   return e ? e->latency : 0;
}
static void filter_engine_process(void *engine, const double input[2], const double controls[8], double out[2]) {
   FilterEngine *e = engine;
   AudioFrame f = {0};
   f.x = isfinite(input[0]) ? input[0] : 0;
   f.y = isfinite(input[1]) ? input[1] : 0;
   memcpy(f.ctl, controls, sizeof f.ctl);
   if (e->direct) native_frame(e, &f);
   else {
      rate_push(&e->in_rate, &f);
      while (rate_ready(&e->in_rate)) {
         AudioFrame native = rate_pop(&e->in_rate);
         native_frame(e, &native);
      }
   }
   out[0] = out[1] = 0;
   if (e->host_frames >= e->queue_start && e->read_pos != e->write_pos) {
      const size_t i = (size_t)(e->read_pos++) & (ENGINE_QUEUE - 1);
      out[0] = e->queue[i].left;
      out[1] = e->queue[i].right;
   }
   ++e->host_frames;
}
