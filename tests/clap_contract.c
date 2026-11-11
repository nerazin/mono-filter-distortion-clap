#include <clap/clap.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c,m) do { if(!(c)){fprintf(stderr,"FAIL %s, line %d\n",m,__LINE__);exit(1);} }while(0)
typedef struct { clap_input_events_t api; clap_event_param_value_t values[8]; const clap_event_header_t *items[8]; unsigned n; } Events;
static uint32_t n_events(const clap_input_events_t *p){return ((Events*)p->ctx)->n;}
static const clap_event_header_t *get_event(const clap_input_events_t *p,uint32_t i){return ((Events*)p->ctx)->items[i];}
static void set_all(const clap_plugin_t *p,const double values[8]){
   Events e={0};e.api.ctx=&e;e.api.size=n_events;e.api.get=get_event;e.n=8;
   for(unsigned k=0;k<8;k++){clap_event_param_value_t *v=&e.values[k];v->header.size=sizeof *v;v->header.space_id=CLAP_CORE_EVENT_SPACE_ID;v->header.type=CLAP_EVENT_PARAM_VALUE;v->param_id=k;v->note_id=v->port_index=v->channel=v->key=-1;v->value=values[k];e.items[k]=&v->header;}
   ((const clap_plugin_params_t*)p->get_extension(p,CLAP_EXT_PARAMS))->flush(p,&e.api,NULL);
}
typedef struct{unsigned char data[512];size_t pos,size;} Stream;
static int64_t stream_write(const clap_ostream_t *s,const void *p,uint64_t n){Stream *v=s->ctx;if(n>3)n=3;if(v->pos+n>sizeof v->data)return -1;memcpy(v->data+v->pos,p,n);v->pos+=n;if(v->size<v->pos)v->size=v->pos;return n;}
static int64_t stream_read(const clap_istream_t *s,void *p,uint64_t n){Stream *v=s->ctx;if(n>4)n=4;if(n>v->size-v->pos)n=v->size-v->pos;memcpy(p,v->data+v->pos,n);v->pos+=n;return n;}
static unsigned latency_changed_count=0;
static void latency_changed(const clap_host_t *p){(void)p;latency_changed_count++;}
static const clap_host_latency_t host_latency={latency_changed};
static const void *host_ext(const clap_host_t *p,const char *id){(void)p;return id && !strcmp(id,CLAP_EXT_LATENCY)?&host_latency:NULL;}
static void host_cb(const clap_host_t *p){(void)p;}
static clap_host_t host={.clap_version=CLAP_VERSION_INIT,.name="Regression host",.vendor="Test",.url="",.version="1",.get_extension=host_ext,.request_restart=host_cb,.request_process=host_cb,.request_callback=host_cb};
typedef struct{void *library;const clap_plugin_entry_t *entry;const clap_plugin_t *plugin;} Loaded;
static Loaded load(const char *path){
   Loaded v={0};v.library=dlopen(path,RTLD_NOW|RTLD_LOCAL);CHECK(v.library,dlerror());v.entry=dlsym(v.library,"clap_entry");CHECK(v.entry && v.entry->init(path),"entry");
   const clap_plugin_factory_t *f=v.entry->get_factory(CLAP_PLUGIN_FACTORY_ID);CHECK(f && f->get_plugin_count(f)==1,"factory");
   const clap_plugin_descriptor_t *d=f->get_plugin_descriptor(f,0);CHECK(d,"descriptor");v.plugin=f->create_plugin(f,&host,d->id);CHECK(v.plugin && v.plugin->init(v.plugin),"init");return v;
}
static void close_plugin(Loaded *v){v->plugin->destroy(v->plugin);v->entry->deinit();dlclose(v->library);}
int main(int argc,char **argv){
   CHECK(argc==3,"usage: clap_contract older.clap current.clap");Loaded old=load(argv[1]), now=load(argv[2]);
   CHECK(!strcmp(old.plugin->desc->id,now.plugin->desc->id),"plugin ID compatibility");CHECK(!strcmp(now.plugin->desc->version,"0.3.0"),"version");
   const clap_plugin_params_t *params=now.plugin->get_extension(now.plugin,CLAP_EXT_PARAMS);CHECK(params && params->count(now.plugin)==8,"8 parameters");
   for(unsigned k=0;k<8;k++){clap_param_info_t info;CHECK(params->get_info(now.plugin,k,&info),"param info");CHECK(info.id==k && (info.flags&CLAP_PARAM_IS_AUTOMATABLE) && (info.flags&CLAP_PARAM_IS_MODULATABLE),"parameter identity and automation");}
   const double values[8]={80.25,16.5,0,127,-19,12,-12,64};set_all(old.plugin,values);
   const clap_plugin_state_t *oldstate=old.plugin->get_extension(old.plugin,CLAP_EXT_STATE),*newstate=now.plugin->get_extension(now.plugin,CLAP_EXT_STATE);
   CHECK(oldstate && newstate,"state extension");Stream s={0};clap_ostream_t os={.ctx=&s,.write=stream_write};clap_istream_t is={.ctx=&s,.read=stream_read};
   CHECK(oldstate->save(old.plugin,&os) && s.size==72,"old state save");s.pos=0;CHECK(newstate->load(now.plugin,&is),"new state load");
   for(unsigned k=0;k<8;k++){double value;CHECK(params->get_value(now.plugin,k,&value) && value==values[k],"old state restored");}
   s.data[0]='?';s.pos=0;CHECK(!newstate->load(now.plugin,&is),"invalid state rejected");double v;CHECK(params->get_value(now.plugin,0,&v)&&v==values[0],"transactional state load");
   const clap_plugin_latency_t *latency=now.plugin->get_extension(now.plugin,CLAP_EXT_LATENCY);CHECK(latency,"latency extension");
   const clap_plugin_audio_ports_t *ports=now.plugin->get_extension(now.plugin,CLAP_EXT_AUDIO_PORTS);CHECK(ports && ports->count(now.plugin,true)==1 && ports->count(now.plugin,false)==1,"audio ports");
   const clap_plugin_remote_controls_t *remote=now.plugin->get_extension(now.plugin,CLAP_EXT_REMOTE_CONTROLS);clap_remote_controls_page_t page;
   CHECK(remote && remote->get(now.plugin,0,&page),"remote page");for(unsigned k=0;k<8;k++)CHECK(page.param_ids[k]==k,"remote ids");
   const double dry[8]={40,127,0,0,0,0,0,0};set_all(now.plugin,dry);
   CHECK(now.plugin->activate(now.plugin,44100,1,256),"activate");CHECK(latency->get(now.plugin)==22 && latency_changed_count==1,"reported 22-sample latency");CHECK(now.plugin->start_processing(now.plugin),"start");
   float l[256]={0},r[256]={0},ol[256]={0},orr[256]={0};l[0]=0.25f;r[0]=-0.25f;
   float *ins[2]={l,r},*outs[2]={ol,orr};clap_audio_buffer_t ai={.data32=ins,.channel_count=2},ao={.data32=outs,.channel_count=2};
   clap_process_t proc={.steady_time=-1,.frames_count=256,.audio_inputs=&ai,.audio_outputs=&ao,.audio_inputs_count=1,.audio_outputs_count=1};
   CHECK(now.plugin->process(now.plugin,&proc)==CLAP_PROCESS_CONTINUE,"process");
   for(int i=0;i<22;i++)CHECK(ol[i]==0 && orr[i]==0,"leading latency silence");
   CHECK(fabs(ol[22]-.25)<1e-6 && fabs(orr[22]+.25)<1e-6,"dry impulse alignment");
   now.plugin->stop_processing(now.plugin);now.plugin->deactivate(now.plugin);
   CHECK(now.plugin->activate(now.plugin,48000,1,256),"activate 48k");CHECK(latency->get(now.plugin)==114 && latency_changed_count==2,"updated 48k latency");
   now.plugin->deactivate(now.plugin);close_plugin(&now);close_plugin(&old);puts("PASS CLAP IDs, older state, automation, ports and latency");
}
