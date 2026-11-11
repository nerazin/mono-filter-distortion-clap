#include <clap/clap.h>
#include <stdint.h>
/* The DSP is in mono_core.h; the engine section below wraps it for a host. */
static void *filter_engine_create(double sample_rate);
static void filter_engine_destroy(void *engine);
static void filter_engine_reset(void *engine);
static uint32_t filter_engine_latency(const void *engine);
static void filter_engine_process(void *engine, const double input[2], const double controls[8],
                                  const double modulation[8], double output[2]);
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Mono Filter & Distortion: the Monomachine's track filter and distortion as
 * a CLAP effect. The DSP is a native fixed-point port of OS 1.32B's signal
 * path, checked sample-for-sample against that firmware running in Monomodule.
 * No firmware or emulator is distributed or needed.
 * SPDX-License-Identifier: GPL-3.0-only */

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
   .version = "0.3.0",
   .description = "The Monomachine track filter and distortion, ported from its DSP code",
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
      for (unsigned i = 0; i < NPARAM; ++i) controls[i] = base_value(s, i);
      filter_engine_process(s->engine, input, controls, s->modulation, output);
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
 * Engine: mono_core.h wrapped for a host.
 *
 * The core is the Monomachine's own arithmetic: 16-frame blocks of 24-bit
 * audio at 44.1 kHz, controlled by the 24-bit words its main CPU sends. This
 * section supplies what surrounds it on the hardware and in a DAW:
 *
 *  - the main CPU's handling of a knob: every 384 frames the word moves a
 *    quarter of the way to the knob's value; modulation is added on top,
 *    unsmoothed, every 48 frames (as the LFOs are);
 *  - float <-> 24-bit conversion, level and polarity (the core inverts and
 *    sits 7 dB down; the track's VOL and LEVEL would follow on the hardware);
 *  - a dry path delayed to match, Mix, and Input/Output gain;
 *  - sample-rate conversion when the host does not run at 44.1 kHz.
 * ------------------------------------------------------------------------- */
#include "mono_core.h"

#define ENGINE_NATIVE_RATE 44100.0
#define ENGINE_CORE_DELAY 6                      /* frames through the open filter */
#define ENGINE_LATENCY (MC_FRAMES + ENGINE_CORE_DELAY)
#define ENGINE_MAKEUP (-1.0 / 0.4455)            /* undo the core's inversion and level */
#define ENGINE_PI 3.14159265358979323846264338327950288
#define ENGINE_HISTORY 4096
#define ENGINE_PHASES 128
#define ENGINE_QUEUE 8192

typedef struct { double x, y, ctl[NPARAM], mod[NPARAM]; } AudioFrame;
typedef struct {
   float *kernel;
   AudioFrame history[ENGINE_HISTORY];
   double ratio;
   int half, taps;
   int64_t last;
   uint64_t output_index;
} RateConverter;

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
      memcpy(result.mod, f->mod, sizeof result.mod);
   }
   return result;
}

typedef struct {
   double rate;
   bool direct, settled;
   uint32_t latency, queue_start;
   uint64_t host_frames, read_pos, write_pos;
   MonoCore core;
   uint32_t blocks;                   /* blocks run; three make one control frame */
   int32_t knob[MC_CONTROLS];         /* knob words after the main CPU's slew */
   int32_t word[MC_CONTROLS];         /* words sent with the current block */
   int pos, dry_pos;
   int32_t in[2][MC_FRAMES];
   double wet[2][MC_FRAMES], dry[2][ENGINE_LATENCY];
   double input_gain, output_gain, mix;
   RateConverter in_rate, out_rate;
   struct { double left, right; } queue[ENGINE_QUEUE];
} FilterEngine;

/* Start of a control frame (three blocks). BASE, WIDTH, HP Q, LP Q and DIST
 * are the core's five controls, in that order. */
static void control_frame(FilterEngine *e, const AudioFrame *f) {
   const uint32_t frame = e->blocks / 3;
   for (int i = 0; i < MC_CONTROLS; ++i) {
      const double value = f->ctl[i] + (i == DIST ? 64.0 : 0.0);
      const int32_t target = (int32_t)lrint(clamp(value, 0.0, 127.0) * 65536.0);
      if (!e->settled) e->knob[i] = target;
      else if ((frame & 7) == 1) e->knob[i] = (3 * e->knob[i] + target) >> 2;
      const double mod = clamp(f->mod[i], -127.0, 127.0) * 65536.0;
      e->word[i] = mod == 0.0 ? e->knob[i] : (int32_t)lrint(clamp(e->knob[i] + mod, 0.0, (double)0x7effff));
   }
}
static void queue_push(FilterEngine *e, double l, double r) {
   if (e->write_pos - e->read_pos >= ENGINE_QUEUE) return;
   const size_t i = (size_t)(e->write_pos++) & (ENGINE_QUEUE - 1);
   e->queue[i].left = l;
   e->queue[i].right = r;
}
static void native_frame(FilterEngine *e, const AudioFrame *f) {
   if (e->pos == 0) {
      if (e->blocks % 3 == 0) control_frame(e, f);
      ++e->blocks;
   }
   /* Input, Output and Mix are not Monomachine controls: plain smoothing */
   const double target[3] = {pow(10.0, f->ctl[INPUT] / 20.0), pow(10.0, f->ctl[OUTPUT] / 20.0), f->ctl[MIX] / 100.0};
   double *const gain[3] = {&e->input_gain, &e->output_gain, &e->mix};
   for (int i = 0; i < 3; ++i) *gain[i] = e->settled ? *gain[i] + (target[i] - *gain[i]) * 0.01 : target[i];
   e->settled = true;

   const double in[2] = {f->x, f->y};
   double out[2];
   for (int c = 0; c < 2; ++c) {
      e->in[c][e->pos] = (int32_t)lrint(clamp(in[c] * e->input_gain, -1.0, 0.99999988) * 8388608.0);
      const double dry = e->dry[c][e->dry_pos];
      e->dry[c][e->dry_pos] = in[c];
      out[c] = ((1.0 - e->mix) * dry + e->mix * e->wet[c][e->pos]) * e->output_gain;
   }
   e->dry_pos = (e->dry_pos + 1) % ENGINE_LATENCY;
   if (++e->pos == MC_FRAMES) {
      e->pos = 0;
      mono_core_block(&e->core, e->word, e->in[0], e->in[1]);
      for (int c = 0; c < 2; ++c)
         for (int i = 0; i < MC_FRAMES; ++i) e->wet[c][i] = e->in[c][i] * (ENGINE_MAKEUP / 8388608.0);
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
   e->settled = false;
   e->host_frames = e->read_pos = e->write_pos = 0;
   e->blocks = 0;
   e->pos = e->dry_pos = 0;
   mono_core_reset(&e->core);
   memset(e->in, 0, sizeof e->in);
   memset(e->wet, 0, sizeof e->wet);
   memset(e->dry, 0, sizeof e->dry);
   rate_reset(&e->in_rate);
   rate_reset(&e->out_rate);
}
static void *filter_engine_create(double rate) {
   if (!isfinite(rate) || rate < 1000 || rate > 768000) return NULL;
   FilterEngine *e = calloc(1, sizeof *e);
   if (!e) return NULL;
   e->rate = rate;
   e->direct = rate == ENGINE_NATIVE_RATE;
   e->latency = ENGINE_LATENCY;
   if (!e->direct) {
      if (!rate_prepare(&e->in_rate, rate, ENGINE_NATIVE_RATE) ||
          !rate_prepare(&e->out_rate, ENGINE_NATIVE_RATE, rate)) {
         free(e->in_rate.kernel); free(e->out_rate.kernel); free(e); return NULL;
      }
      e->queue_start = (uint32_t)(e->in_rate.half + ceil(e->out_rate.half * rate / ENGINE_NATIVE_RATE) + 2);
      e->latency = e->queue_start + (uint32_t)llround(ENGINE_LATENCY * rate / ENGINE_NATIVE_RATE);
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
static void filter_engine_process(void *engine, const double input[2], const double controls[8],
                                  const double modulation[8], double out[2]) {
   FilterEngine *e = engine;
   AudioFrame f = {0};
   f.x = isfinite(input[0]) ? input[0] : 0;
   f.y = isfinite(input[1]) ? input[1] : 0;
   memcpy(f.ctl, controls, sizeof f.ctl);
   memcpy(f.mod, modulation, sizeof f.mod);
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
