/* Minimal offline CLAP host used for regression and firmware comparisons.
 * Input: headerless frames of 10 doubles: stereo input then 8 raw controls.
 * Output: headerless interleaved stereo doubles, including declared latency.
 */
#include <clap/clap.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(c, m) do { if (!(c)) { fprintf(stderr, "%s (line %d)\n", m, __LINE__); exit(1); } } while (0)
#define MAX_CHUNK 2048
#define MAX_EVENTS (8 * MAX_CHUNK)

typedef struct { double input[2], control[8]; } Frame;
typedef struct { clap_input_events_t api; clap_event_param_value_t values[MAX_EVENTS]; const clap_event_header_t *items[MAX_EVENTS]; uint32_t count; } Events;
static uint32_t event_size(const clap_input_events_t *ev) { return ((Events *)ev->ctx)->count; }
static const clap_event_header_t *event_get(const clap_input_events_t *ev, uint32_t i) { return ((Events *)ev->ctx)->items[i]; }
static void event_add(Events *ev, unsigned id, double value, unsigned time) {
   REQUIRE(ev->count < MAX_EVENTS, "too many events");
   clap_event_param_value_t *p = &ev->values[ev->count];
   memset(p, 0, sizeof *p);
   p->header.size = sizeof *p; p->header.time = time;
   p->header.space_id = CLAP_CORE_EVENT_SPACE_ID; p->header.type = CLAP_EVENT_PARAM_VALUE;
   p->param_id = id; p->note_id = p->port_index = p->channel = p->key = -1; p->value = value;
   ev->items[ev->count++] = &p->header;
}
static const void *host_ext(const clap_host_t *host, const char *id) { (void)host; (void)id; return NULL; }
static void host_cb(const clap_host_t *host) { (void)host; }
static clap_host_t host = {.clap_version=CLAP_VERSION_INIT, .name="Regression host", .vendor="Test", .url="", .version="1", .get_extension=host_ext, .request_restart=host_cb, .request_process=host_cb, .request_callback=host_cb};

int main(int argc, char **argv) {
   REQUIRE(argc == 7 || argc == 8, "usage: clap_render plugin input output sample_rate chunk flush_extra [32|64]");
   const double rate = atof(argv[4]);
   const uint32_t chunk = (uint32_t)atoi(argv[5]);
   const uint32_t extra = (uint32_t)atoi(argv[6]);
   const int bits = argc == 8 ? atoi(argv[7]) : 64;
   REQUIRE(bits == 32 || bits == 64, "invalid sample format");
   REQUIRE(chunk > 0 && chunk <= MAX_CHUNK, "invalid chunk size");
   FILE *f = fopen(argv[2], "rb"); REQUIRE(f, "input open");
   REQUIRE(fseek(f, 0, SEEK_END) == 0, "seek input");
   long bytes = ftell(f); REQUIRE(bytes > 0 && bytes % (long)sizeof(Frame) == 0, "input size");
   rewind(f); const size_t frames = (size_t)bytes / sizeof(Frame);
   Frame *input = malloc(frames * sizeof *input); REQUIRE(input, "allocate input");
   REQUIRE(fread(input, sizeof *input, frames, f) == frames, "input read"); fclose(f);
   FILE *out = fopen(argv[3], "wb"); REQUIRE(out, "output open");
   void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); REQUIRE(lib, dlerror());
   const clap_plugin_entry_t *entry = dlsym(lib, "clap_entry"); REQUIRE(entry && entry->init(argv[1]), "entry init");
   const clap_plugin_factory_t *factory = entry->get_factory(CLAP_PLUGIN_FACTORY_ID); REQUIRE(factory, "factory");
   const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, 0); REQUIRE(desc, "descriptor");
   const clap_plugin_t *plug = factory->create_plugin(factory, &host, desc->id); REQUIRE(plug && plug->init(plug), "plugin init");
   const clap_plugin_params_t *params = plug->get_extension(plug, CLAP_EXT_PARAMS); REQUIRE(params, "parameters");
   const clap_plugin_latency_t *latency = plug->get_extension(plug, CLAP_EXT_LATENCY); REQUIRE(latency, "latency extension");
   Events *ev = calloc(1, sizeof *ev); REQUIRE(ev, "allocate events");
   ev->api.ctx=ev; ev->api.size=event_size; ev->api.get=event_get;
   for (unsigned k=0;k<8;k++) event_add(ev,k,input[0].control[k],0);
   params->flush(plug,&ev->api,NULL); ev->count=0;
   REQUIRE(plug->activate(plug,rate,1,chunk) && plug->start_processing(plug), "activate");
   const uint32_t delay=latency->get(plug);
   double *l=calloc(chunk,sizeof *l),*r=calloc(chunk,sizeof *r),*ol=calloc(chunk,sizeof *ol),*orr=calloc(chunk,sizeof *orr);
   float *lf=calloc(chunk,sizeof *lf),*rf=calloc(chunk,sizeof *rf),*olf=calloc(chunk,sizeof *olf),*orrf=calloc(chunk,sizeof *orrf);
   REQUIRE(l && r && ol && orr && lf && rf && olf && orrf, "allocate audio");
   double *in_channels[2]={l,r}, *out_channels[2]={ol,orr};
   float *in_float[2]={lf,rf}, *out_float[2]={olf,orrf};
   clap_audio_buffer_t ai={.data64=in_channels,.channel_count=2}, ao={.data64=out_channels,.channel_count=2};
   if(bits==32){ai.data64=NULL;ao.data64=NULL;ai.data32=in_float;ao.data32=out_float;}
   double previous[8]; for(int k=0;k<8;k++) previous[k]=input[0].control[k];
   const size_t total=frames+(extra ? delay+extra : 0);
   for(size_t pos=0;pos<total;) {
      uint32_t n=(uint32_t)((total-pos)<chunk ? (total-pos) : chunk);ev->count=0;
      for(uint32_t i=0;i<n;i++) {
         const size_t index=pos+i; l[i]=r[i]=0;
         if(index<frames) {
            l[i]=input[index].input[0];r[i]=input[index].input[1];
            for(unsigned k=0;k<8;k++) if(input[index].control[k]!=previous[k]) {
               event_add(ev,k,input[index].control[k],i);previous[k]=input[index].control[k];
            }
         }
         lf[i]=(float)l[i];rf[i]=(float)r[i];
      }
      clap_process_t process={.steady_time=(int64_t)pos,.frames_count=n,.audio_inputs=&ai,.audio_outputs=&ao,.audio_inputs_count=1,.audio_outputs_count=1,.in_events=&ev->api};
      REQUIRE(plug->process(plug,&process)==CLAP_PROCESS_CONTINUE,"processing failed");
      for(uint32_t i=0;i<n;i++){double sample[2]={bits==32 ? olf[i] : ol[i],bits==32 ? orrf[i] : orr[i]};REQUIRE(isfinite(sample[0]) && isfinite(sample[1]),"non-finite output");REQUIRE(fwrite(sample,sizeof sample,1,out)==1,"write output");}
      pos+=n;
   }
   printf("version=%s rate=%.1f latency=%u frames=%zu chunk=%u precision=%d\n",desc->version,rate,delay,frames,chunk,bits);
   plug->stop_processing(plug);plug->deactivate(plug);plug->destroy(plug);entry->deinit();
   dlclose(lib);fclose(out);free(input);free(ev);free(l);free(r);free(ol);free(orr);free(lf);free(rf);free(olf);free(orrf);return 0;
}
