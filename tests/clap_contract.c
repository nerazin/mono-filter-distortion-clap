#include <clap/clap.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c,m) do { if(!(c)){fprintf(stderr,"FAIL %s, line %d\n",m,__LINE__);exit(1);} }while(0)
typedef struct { clap_input_events_t api; clap_event_param_value_t values[10]; const clap_event_header_t *items[10]; unsigned n; } Events;
static uint32_t n_events(const clap_input_events_t *p){return ((Events*)p->ctx)->n;}
static const clap_event_header_t *get_event(const clap_input_events_t *p,uint32_t i){return ((Events*)p->ctx)->items[i];}
static void set_all(const clap_plugin_t *p,const double *values,unsigned n){
   Events e={0};e.api.ctx=&e;e.api.size=n_events;e.api.get=get_event;e.n=n;
   for(unsigned k=0;k<n;k++){clap_event_param_value_t *v=&e.values[k];v->header.size=sizeof *v;v->header.space_id=CLAP_CORE_EVENT_SPACE_ID;v->header.type=CLAP_EVENT_PARAM_VALUE;v->param_id=k;v->note_id=v->port_index=v->channel=v->key=-1;v->value=values[k];e.items[k]=&v->header;}
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
#define MOD_FRAMES 4096
static uint32_t one_size(const clap_input_events_t *p){(void)p;return 1;}
static const clap_event_header_t *one_get(const clap_input_events_t *p,uint32_t i){(void)i;return p->ctx;}
/* id >= 10 renders with no event at all */
static void render_mod(const clap_plugin_t *p,double rate,clap_id id,double amount,double *out){
   clap_event_param_mod_t m={.header={.size=sizeof m,.space_id=CLAP_CORE_EVENT_SPACE_ID,.type=CLAP_EVENT_PARAM_MOD},.param_id=id,.note_id=-1,.port_index=-1,.channel=-1,.key=-1,.amount=amount};
   clap_input_events_t ev={.ctx=&m,.size=one_size,.get=one_get};
   double in[2][MOD_FRAMES];for(int i=0;i<MOD_FRAMES;i++){in[0][i]=.05*sin(i*.031)+.03*sin(i*.37);in[1][i]=.04*sin(i*.047)+.03*sin(i*.29);}
   double *ins[2]={in[0],in[1]},*outs[2]={out,out+MOD_FRAMES};clap_audio_buffer_t ai={.data64=ins,.channel_count=2},ao={.data64=outs,.channel_count=2};
   clap_process_t pr={.steady_time=-1,.frames_count=MOD_FRAMES,.audio_inputs=&ai,.audio_outputs=&ao,.audio_inputs_count=1,.audio_outputs_count=1,.in_events=id<10?&ev:NULL};
   CHECK(p->activate(p,rate,1,MOD_FRAMES)&&p->start_processing(p),"activate modulation render");CHECK(p->process(p,&pr)==CLAP_PROCESS_CONTINUE,"process modulation render");
   p->stop_processing(p);p->deactivate(p);
   if(id<10){m.amount=0;((const clap_plugin_params_t*)p->get_extension(p,CLAP_EXT_PARAMS))->flush(p,&ev,NULL);}
}
/* The parameter at `from` plus `amount` must sound exactly like the parameter at `to`; zero modulation must change nothing. */
static void check_mod(const clap_plugin_t *p,double rate,double *v,unsigned id,double to,double from,double amount){
   static double want[2*MOD_FRAMES],plain[2*MOD_FRAMES],got[2*MOD_FRAMES];const double home=v[id];double most=0;
   v[id]=to;set_all(p,v,10);render_mod(p,rate,10,0,want);
   v[id]=from;set_all(p,v,10);render_mod(p,rate,10,0,plain);
   for(int i=0;i<2*MOD_FRAMES;i++)most=fmax(most,fabs(want[i]-plain[i]));CHECK(most>1e-4,"test values must sound different");
   render_mod(p,rate,id,0,got);CHECK(!memcmp(got,plain,sizeof got),"zero modulation changes nothing");
   render_mod(p,rate,id,amount,got);CHECK(!memcmp(got,want,sizeof got),"modulation reaches the parameter in plain units, clamped");
   v[id]=home;set_all(p,v,10);
}
static void check_modulation(const clap_plugin_t *p){
   static const double rates[3]={44100,48000,96000},lo[3]={-24,-24,0},hi[3]={24,24,100},mid[3]={0,0,50},to[3]={3,-3,40},by[3]={5,-7,30};
   double v[10]={60,30,0,0,0,0,0,100,64,0};
   for(unsigned r=0;r<3;r++)for(unsigned k=0;k<3;k++){
      check_mod(p,rates[r],v,5+k,to[k],to[k]-by[k],by[k]);
      check_mod(p,rates[r],v,5+k,lo[k],mid[k],-127);check_mod(p,rates[r],v,5+k,hi[k],mid[k],127);
   }
}
int main(int argc,char **argv){
   CHECK(argc==3,"usage: clap_contract older.clap current.clap");Loaded old=load(argv[1]), now=load(argv[2]);
   CHECK(!strcmp(old.plugin->desc->id,now.plugin->desc->id),"plugin ID compatibility");CHECK(!strcmp(now.plugin->desc->version,"0.4.0"),"version");
   const clap_plugin_params_t *params=now.plugin->get_extension(now.plugin,CLAP_EXT_PARAMS);CHECK(params && params->count(now.plugin)==10,"10 parameters");
   for(unsigned k=0;k<10;k++){clap_param_info_t info;CHECK(params->get_info(now.plugin,k,&info),"param info");CHECK(info.id==k && (info.flags&CLAP_PARAM_IS_AUTOMATABLE) && (info.flags&CLAP_PARAM_IS_MODULATABLE),"parameter identity and automation");}
   const double values[8]={80.25,16.5,0,127,-19,12,-12,64};set_all(old.plugin,values,8);
   const clap_plugin_state_t *oldstate=old.plugin->get_extension(old.plugin,CLAP_EXT_STATE),*newstate=now.plugin->get_extension(now.plugin,CLAP_EXT_STATE);
   CHECK(oldstate && newstate,"state extension");Stream s={0};clap_ostream_t os={.ctx=&s,.write=stream_write};clap_istream_t is={.ctx=&s,.read=stream_read};
   CHECK(oldstate->save(old.plugin,&os) && s.size==72,"old state save");s.pos=0;CHECK(newstate->load(now.plugin,&is),"new state load");
   for(unsigned k=0;k<8;k++){double value;CHECK(params->get_value(now.plugin,k,&value) && value==values[k],"old state restored");}
   {double f,g;CHECK(params->get_value(now.plugin,8,&f) && f==64 && params->get_value(now.plugin,9,&g) && g==0,"old state leaves EQF/EQG at defaults");}
   {const double all[10]={1,2,3,4,5,6,7,8,9.5,-10.25};set_all(now.plugin,all,10);Stream t={0};clap_ostream_t o2={.ctx=&t,.write=stream_write};clap_istream_t i2={.ctx=&t,.read=stream_read};
    CHECK(newstate->save(now.plugin,&o2) && t.size==88,"state save");set_all(now.plugin,values,8);t.pos=0;CHECK(newstate->load(now.plugin,&i2),"state load");
    for(unsigned k=0;k<10;k++){double value;CHECK(params->get_value(now.plugin,k,&value) && value==all[k],"state round trip");}
    double back[10];memcpy(back,values,sizeof values);back[8]=64;back[9]=0;set_all(now.plugin,back,10);}
   s.data[0]='?';s.pos=0;CHECK(!newstate->load(now.plugin,&is),"invalid state rejected");double v;CHECK(params->get_value(now.plugin,0,&v)&&v==values[0],"transactional state load");
   const clap_plugin_latency_t *latency=now.plugin->get_extension(now.plugin,CLAP_EXT_LATENCY);CHECK(latency,"latency extension");
   const clap_plugin_audio_ports_t *ports=now.plugin->get_extension(now.plugin,CLAP_EXT_AUDIO_PORTS);CHECK(ports && ports->count(now.plugin,true)==1 && ports->count(now.plugin,false)==1,"audio ports");
   const clap_plugin_remote_controls_t *remote=now.plugin->get_extension(now.plugin,CLAP_EXT_REMOTE_CONTROLS);clap_remote_controls_page_t page;
   CHECK(remote && remote->count(now.plugin)==2 && remote->get(now.plugin,0,&page),"remote page");for(unsigned k=0;k<8;k++)CHECK(page.param_ids[k]==k,"remote ids");
   CHECK(remote->get(now.plugin,1,&page) && page.param_ids[0]==8 && page.param_ids[1]==9 && page.param_ids[2]==CLAP_INVALID_ID,"EQ remote page");
   const double dry[8]={40,127,0,0,0,0,0,0};set_all(now.plugin,dry,8);
   CHECK(now.plugin->activate(now.plugin,44100,1,256),"activate");CHECK(latency->get(now.plugin)==22 && latency_changed_count==1,"reported 22-sample latency");CHECK(now.plugin->start_processing(now.plugin),"start");
   float l[256]={0},r[256]={0},ol[256]={0},orr[256]={0};l[0]=0.25f;r[0]=-0.25f;
   float *ins[2]={l,r},*outs[2]={ol,orr};clap_audio_buffer_t ai={.data32=ins,.channel_count=2},ao={.data32=outs,.channel_count=2};
   clap_process_t proc={.steady_time=-1,.frames_count=256,.audio_inputs=&ai,.audio_outputs=&ao,.audio_inputs_count=1,.audio_outputs_count=1};
   CHECK(now.plugin->process(now.plugin,&proc)==CLAP_PROCESS_CONTINUE,"process");
   for(int i=0;i<22;i++)CHECK(ol[i]==0 && orr[i]==0,"leading latency silence");
   CHECK(fabs(ol[22]-.25)<1e-6 && fabs(orr[22]+.25)<1e-6,"dry impulse alignment");
   now.plugin->stop_processing(now.plugin);now.plugin->deactivate(now.plugin);
   CHECK(now.plugin->activate(now.plugin,48000,1,256),"activate 48k");CHECK(latency->get(now.plugin)==114 && latency_changed_count==2,"updated 48k latency");
   now.plugin->deactivate(now.plugin);check_modulation(now.plugin);close_plugin(&now);close_plugin(&old);puts("PASS CLAP IDs, older state, automation, ports, latency and Input/Output/Mix modulation");
}
