/* SpeechDesk native audio helper. Copyright (c) 2026 davoda1. MIT licensed.
 * miniaudio is pinned to 0.11.25; see MINIAUDIO-LICENSE.
 * Capture callbacks only copy to a bounded ring buffer. The main thread owns disk I/O.
 */
#define _POSIX_C_SOURCE 200809L
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_WASAPI
#define MA_ENABLE_DSOUND
#define MA_ENABLE_WINMM
#define MA_ENABLE_PULSEAUDIO
#define MA_ENABLE_ALSA
#define MA_NO_NULL
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_GENERATION
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <locale.h>
#include <math.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

#define AUDIO_RATE 16000u
#define MAX_SECONDS 3600u
#define MAX_FRAMES ((uint64_t)AUDIO_RATE * MAX_SECONDS)
#define RING_FRAMES (AUDIO_RATE * 2u)

typedef struct { FILE *file; uint64_t frames; } wav_writer;
typedef struct {
    ma_pcm_rb ring;
    atomic_int stop;
    atomic_int stopping;
    atomic_int fault;
    atomic_uint peak;
    atomic_uint_fast64_t captured;
    atomic_uint_fast64_t last_callback_ms;
    uint64_t max_frames;
} capture_state;
static capture_state capture;
static volatile sig_atomic_t interrupted = 0;

static uint64_t now_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
#endif
}
static void wait_ms(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec t = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {}
#endif
}
static void on_signal(int sig) { (void)sig; interrupted = 1; }

static void json_string(const char *s) {
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    putchar('"');
    for (; *p; ++p) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}
static int fail(const char *message, int code) {
    printf("{\"type\":\"error\",\"message\":"); json_string(message);
    printf(",\"code\":%d}\n", code); fflush(stdout);
    fprintf(stderr, "SpeechDesk audio: %s (%d)\n", message, code);
    return 1;
}

#ifdef _WIN32
static wchar_t *wide_path(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n)) { free(w); return NULL; }
    return w;
}
static FILE *open_output(const char *path) {
    wchar_t *w = wide_path(path); FILE *f;
    if (!w) return NULL;
    f = _wfopen(w, L"wb"); free(w); return f;
}
static void remove_output(const char *path) {
    wchar_t *w = wide_path(path); if (w) { _wremove(w); free(w); }
}
static ma_result decoder_open(const char *path, ma_decoder_config *cfg, ma_decoder *decoder) {
    wchar_t *w = wide_path(path); ma_result r;
    if (!w) return MA_INVALID_ARGS;
    r = ma_decoder_init_file_w(w, cfg, decoder); free(w); return r;
}
static int same_file(const char *a, const char *b) {
    wchar_t *wa = wide_path(a), *wb = wide_path(b);
    wchar_t pa[32768], pb[32768]; int same = 0;
    if (wa && wb && GetFullPathNameW(wa, 32768, pa, NULL) && GetFullPathNameW(wb, 32768, pb, NULL))
        same = _wcsicmp(pa, pb) == 0;
    if (!same && wa && wb) {
        HANDLE ha = CreateFileW(wa, 0, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        HANDLE hb = CreateFileW(wb, 0, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        BY_HANDLE_FILE_INFORMATION ia, ib;
        if (ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE && GetFileInformationByHandle(ha,&ia) && GetFileInformationByHandle(hb,&ib))
            same = ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber && ia.nFileIndexHigh == ib.nFileIndexHigh && ia.nFileIndexLow == ib.nFileIndexLow;
        if (ha != INVALID_HANDLE_VALUE) CloseHandle(ha);
        if (hb != INVALID_HANDLE_VALUE) CloseHandle(hb);
    }
    free(wa); free(wb); return same;
}
#else
static FILE *open_output(const char *path) { return fopen(path, "wb"); }
static void remove_output(const char *path) { remove(path); }
static ma_result decoder_open(const char *path, ma_decoder_config *cfg, ma_decoder *decoder) {
    return ma_decoder_init_file(path, cfg, decoder);
}
static int same_file(const char *a, const char *b) {
    struct stat sa, sb;
    if (!strcmp(a,b)) return 1;
    return stat(a,&sa)==0 && stat(b,&sb)==0 && sa.st_dev==sb.st_dev && sa.st_ino==sb.st_ino;
}
#endif

static void le16(unsigned char *p, uint16_t n) { p[0]=(unsigned char)n; p[1]=(unsigned char)(n>>8); }
static void le32(unsigned char *p, uint32_t n) { p[0]=(unsigned char)n; p[1]=(unsigned char)(n>>8); p[2]=(unsigned char)(n>>16); p[3]=(unsigned char)(n>>24); }
static int wav_header(wav_writer *w) {
    unsigned char h[44] = {0}; uint32_t bytes = (uint32_t)(w->frames * 2u);
    memcpy(h,"RIFF",4); le32(h+4,36u+bytes); memcpy(h+8,"WAVEfmt ",8);
    le32(h+16,16); le16(h+20,1); le16(h+22,1); le32(h+24,AUDIO_RATE);
    le32(h+28,AUDIO_RATE*2u); le16(h+32,2); le16(h+34,16);
    memcpy(h+36,"data",4); le32(h+40,bytes);
    return fseek(w->file,0,SEEK_SET)==0 && fwrite(h,1,44,w->file)==44;
}
static int wav_open(wav_writer *w, const char *path) {
    memset(w,0,sizeof(*w)); w->file=open_output(path);
    if (!w->file) return 0;
    if (!wav_header(w)) { fclose(w->file); w->file=NULL; return 0; }
    return 1;
}
static int wav_write(wav_writer *w, const int16_t *samples, uint32_t frames) {
    if (w->frames + frames > MAX_FRAMES) return 0;
    if (fwrite(samples,2,frames,w->file)!=frames) return 0;
    w->frames+=frames; return 1;
}
static int wav_close(wav_writer *w) {
    int ok;
    if (!w->file) return 0;
    ok=wav_header(w); if (fflush(w->file)!=0) ok=0;
    if (fclose(w->file)!=0) ok=0;
    w->file=NULL;
    return ok;
}

static const char *backend_name(ma_backend b) {
    switch (b) {
        case ma_backend_wasapi: return "wasapi";
        case ma_backend_dsound: return "dsound";
        case ma_backend_winmm: return "winmm";
        case ma_backend_pulseaudio: return "pulse";
        case ma_backend_alsa: return "alsa";
        default: return "unsupported";
    }
}
static int allowed_backend(ma_backend b) {
#ifdef _WIN32
    return b==ma_backend_wasapi || b==ma_backend_dsound || b==ma_backend_winmm;
#else
    return b==ma_backend_pulseaudio || b==ma_backend_alsa;
#endif
}
static ma_result init_context(ma_context *ctx, const ma_backend *specific) {
#ifdef _WIN32
    const ma_backend backends[]={ma_backend_wasapi,ma_backend_dsound,ma_backend_winmm};
#else
    const ma_backend backends[]={ma_backend_pulseaudio,ma_backend_alsa};
#endif
    ma_context_config cfg=ma_context_config_init();
    return ma_context_init(specific?specific:backends, specific?1u:(ma_uint32)(sizeof(backends)/sizeof(backends[0])), &cfg,ctx);
}
static void canonical_id(ma_device_id *dst, const ma_device_id *src, ma_backend b) {
    size_t i; memset(dst,0,sizeof(*dst));
    switch(b) {
        case ma_backend_wasapi:
            for(i=0;i<63 && src->wasapi[i];++i) dst->wasapi[i]=src->wasapi[i];
            break;
        case ma_backend_dsound: memcpy(dst->dsound,src->dsound,16); break;
        case ma_backend_winmm: dst->winmm=src->winmm; break;
        case ma_backend_pulseaudio:
            for(i=0;i<255 && src->pulse[i];++i) dst->pulse[i]=src->pulse[i];
            break;
        case ma_backend_alsa:
            for(i=0;i<255 && src->alsa[i];++i) dst->alsa[i]=src->alsa[i];
            break;
        default: break;
    }
}
static void print_device_id(const ma_device_id *id, ma_backend b) {
    ma_device_id clean; size_t i; const unsigned char *bytes=(const unsigned char *)&clean;
    canonical_id(&clean,id,b); printf("\"%s:",backend_name(b));
    for(i=0;i<sizeof(clean);++i) printf("%02x",bytes[i]);
    putchar('"');
}
static int hexval(char c) {
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}
static int parse_device_id(const char *text, ma_device_id *id, ma_backend *backend) {
    const char *colon=strchr(text,':'); size_t n,i; unsigned char *bytes=(unsigned char *)id;
    const ma_backend all[]={ma_backend_wasapi,ma_backend_dsound,ma_backend_winmm,ma_backend_pulseaudio,ma_backend_alsa};
    if(!colon || strlen(colon+1)!=sizeof(*id)*2u)return 0;
    n=(size_t)(colon-text); *backend=ma_backend_null;
    for(i=0;i<sizeof(all)/sizeof(all[0]);++i) if(strlen(backend_name(all[i]))==n && !strncmp(text,backend_name(all[i]),n)) *backend=all[i];
    if(!allowed_backend(*backend))return 0;
    for(i=0;i<sizeof(*id);++i) { int hi=hexval(colon[1+i*2]),lo=hexval(colon[2+i*2]);if(hi<0||lo<0)return 0;bytes[i]=(unsigned char)((hi<<4)|lo); }
    if(*backend==ma_backend_wasapi && id->wasapi[63]!=0)return 0;
    if(*backend==ma_backend_pulseaudio && id->pulse[255]!=0)return 0;
    if(*backend==ma_backend_alsa && (id->alsa[255]!=0 || !strcmp(id->alsa,"null")))return 0;
    return 1;
}
static int devices_command(void) {
    ma_context ctx; ma_device_info *list=NULL; ma_uint32 count=0,i; int printed=0; ma_result r;
    r=init_context(&ctx,NULL); if(r!=MA_SUCCESS)return fail("No supported audio backend is available.",r);
    r=ma_context_get_devices(&ctx,NULL,NULL,&list,&count);
    if(r!=MA_SUCCESS) { ma_context_uninit(&ctx);return fail("Could not enumerate microphone devices.",r); }
    printf("{\"devices\":[");
    for(i=0;i<count;++i) {
        if(ctx.backend==ma_backend_alsa && !strcmp(list[i].id.alsa,"null"))continue;
        if(printed++)putchar(',');
        printf("{\"id\":");print_device_id(&list[i].id,ctx.backend);
        printf(",\"name\":");json_string(list[i].name);printf(",\"isDefault\":%s}",list[i].isDefault?"true":"false");
    }
    printf("]}\n");fflush(stdout);ma_context_uninit(&ctx);return 0;
}

static void data_callback(ma_device *device, void *out, const void *in, ma_uint32 count) {
    capture_state *s=(capture_state *)device->pUserData; uint64_t total; ma_uint32 remain,offset=0; unsigned peak=0,i;
    const int16_t *samples=(const int16_t *)in; (void)out;
    if(atomic_load(&s->stopping)||atomic_load(&s->fault)||atomic_load(&s->stop))return;
    if(!in) { atomic_store(&s->fault,1);return; }
    atomic_store(&s->last_callback_ms,now_ms()); total=atomic_load(&s->captured);
    if(total>=s->max_frames){atomic_store(&s->stop,1);return;}
    if((uint64_t)count>s->max_frames-total)count=(ma_uint32)(s->max_frames-total);
    for(i=0;i<count;++i) { int v=samples[i];unsigned a=(unsigned)(v<0?-v:v);if(a>peak)peak=a; }
    { unsigned old=atomic_load(&s->peak);while(peak>old && !atomic_compare_exchange_weak(&s->peak,&old,peak)) {} }
    remain=count;
    while(remain) {
        void *dest=NULL; ma_uint32 n=remain;
        if(ma_pcm_rb_acquire_write(&s->ring,&n,&dest)!=MA_SUCCESS || n==0) {atomic_store(&s->fault,2);return;}
        memcpy(dest,samples+offset,(size_t)n*2u);ma_pcm_rb_commit_write(&s->ring,n);remain-=n;offset+=n;
    }
    atomic_store(&s->captured,total+count);if(total+count>=s->max_frames)atomic_store(&s->stop,1);
}
static void notification_callback(const ma_device_notification *n) {
    capture_state *s=(capture_state *)n->pDevice->pUserData;
    if(!atomic_load(&s->stopping) && (n->type==ma_device_notification_type_stopped || n->type==ma_device_notification_type_interruption_began))
        atomic_store(&s->fault,3);
}
#ifdef _WIN32
static DWORD WINAPI stdin_worker(LPVOID unused) {
#else
static void *stdin_worker(void *unused) {
#endif
    /* Never hold stdin's FILE lock in this detached worker. When the duration
     * cap stops capture, the parent may deliberately keep stdin open. fgets()
     * would then keep a stdio lock while exit() tries to close standard streams,
     * hanging an otherwise finalized recording. OS reads do not take that lock.
     */
    char line[64]; size_t length=0; int overflow=0; (void)unused;
    for(;;) {
        char c;
#ifdef _WIN32
        DWORD count=0;
        if(!ReadFile(GetStdHandle(STD_INPUT_HANDLE),&c,1,&count,NULL) || count==0)break;
#else
        ssize_t count=read(STDIN_FILENO,&c,1);
        if(count<0 && errno==EINTR)continue;
        if(count<=0)break;
#endif
        if(c=='\n') {
            if(!overflow && length==4 && !memcmp(line,"stop",4))break;
            length=0;overflow=0;
        } else if(c!='\r') {
            if(length<sizeof(line))line[length++]=c;
            else overflow=1;
        }
    }
    atomic_store(&capture.stop,1);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
static int start_stdin_worker(void) {
#ifdef _WIN32
    HANDLE t=CreateThread(NULL,0,stdin_worker,NULL,0,NULL);if(!t)return 0;CloseHandle(t);return 1;
#else
    pthread_t t;if(pthread_create(&t,NULL,stdin_worker,NULL)!=0)return 0;pthread_detach(t);return 1;
#endif
}
static int drain_ring(wav_writer *writer) {
    ma_uint32 n;
    while((n=ma_pcm_rb_available_read(&capture.ring))>0) {
        void *data=NULL;
        if(ma_pcm_rb_acquire_read(&capture.ring,&n,&data)!=MA_SUCCESS || n==0)return 0;
        if(!wav_write(writer,(const int16_t *)data,n))return 0;
        ma_pcm_rb_commit_read(&capture.ring,n);
    }
    return 1;
}
static int record_command(const char *output, const char *device_text, double max_seconds) {
    ma_context ctx; ma_device device; ma_device_config cfg; ma_device_id id; ma_backend backend; ma_result r;
    wav_writer writer; uint64_t next_level; int fault=0,ok=1;
    if(device_text && !parse_device_id(device_text,&id,&backend))return fail("The selected microphone ID is invalid or belongs to another platform.",-1);
    r=init_context(&ctx,device_text?&backend:NULL);if(r!=MA_SUCCESS)return fail("No supported audio capture backend is available.",r);
    memset(&capture,0,sizeof(capture)); capture.max_frames=(uint64_t)(max_seconds*AUDIO_RATE);
    atomic_init(&capture.stop,0);atomic_init(&capture.stopping,0);atomic_init(&capture.fault,0);
    atomic_init(&capture.peak,0);atomic_init(&capture.captured,0);atomic_init(&capture.last_callback_ms,now_ms());
    r=ma_pcm_rb_init(ma_format_s16,1,RING_FRAMES,NULL,NULL,&capture.ring);
    if(r!=MA_SUCCESS){ma_context_uninit(&ctx);return fail("Could not allocate the bounded audio buffer.",r);}
    cfg=ma_device_config_init(ma_device_type_capture);cfg.capture.format=ma_format_s16;cfg.capture.channels=1;cfg.sampleRate=AUDIO_RATE;
    cfg.capture.pDeviceID=device_text?&id:NULL;cfg.dataCallback=data_callback;cfg.notificationCallback=notification_callback;
    cfg.pUserData=&capture;cfg.periodSizeInMilliseconds=20;
    r=ma_device_init(&ctx,&cfg,&device);
    if(r!=MA_SUCCESS){ma_pcm_rb_uninit(&capture.ring);ma_context_uninit(&ctx);return fail("Could not open the microphone. Check its connection, permissions, and whether another application is using it.",r);}
    if(!wav_open(&writer,output)){ma_device_uninit(&device);ma_pcm_rb_uninit(&capture.ring);ma_context_uninit(&ctx);return fail("Could not create the recording file.",errno);}
    r=ma_device_start(&device);
    if(r!=MA_SUCCESS){atomic_store(&capture.stopping,1);ma_device_uninit(&device);wav_close(&writer);remove_output(output);ma_pcm_rb_uninit(&capture.ring);ma_context_uninit(&ctx);return fail("The microphone could not start recording.",r);}
    printf("{\"type\":\"started\"}\n");fflush(stdout);
    if(!start_stdin_worker()){atomic_store(&capture.fault,4);}
    next_level=now_ms()+100;
    while(!atomic_load(&capture.stop) && !atomic_load(&capture.fault) && !interrupted) {
        uint64_t now;
        if(!drain_ring(&writer)){atomic_store(&capture.fault,5);break;}
        now=now_ms();
        if(now>=next_level) {
            printf("{\"type\":\"level\",\"peak\":%.6f,\"seconds\":%.3f}\n",atomic_exchange(&capture.peak,0)/32768.0,writer.frames/(double)AUDIO_RATE);
            fflush(stdout);next_level=now+100;
        }
        if(now-atomic_load(&capture.last_callback_ms)>10000){atomic_store(&capture.fault,6);break;}
        wait_ms(10);
    }
    atomic_store(&capture.stopping,1);ma_device_stop(&device);ma_device_uninit(&device);
    if(!drain_ring(&writer)){atomic_store(&capture.fault,5);}
    fault=atomic_load(&capture.fault);ok=wav_close(&writer);
    ma_pcm_rb_uninit(&capture.ring);ma_context_uninit(&ctx);
    if(!ok)fault=5;
    if(fault) {
        const char *message=fault==2?"Audio capture overflowed. Close demanding applications and retry.":fault==3?"The microphone stopped or was disconnected.":fault==4?"Could not start recording control.":fault==5?"Could not write or finalize the recording. Check available disk space.":fault==6?"The microphone stopped delivering audio.":"The microphone returned invalid audio.";
        return fail(message,fault);
    }
    printf("{\"type\":\"stopped\",\"duration\":%.6f}\n",writer.frames/(double)AUDIO_RATE);fflush(stdout);return 0;
}
static int convert_command(const char *input,const char *output) {
    ma_decoder decoder; ma_decoder_config cfg=ma_decoder_config_init(ma_format_s16,1,AUDIO_RATE);ma_result r;
    wav_writer writer;int16_t buffer[8192];int failed=0;const char *reason=NULL;ma_uint64 declared_frames=0;
    if(same_file(input,output))return fail("Input and output must be different files.",-1);
    r=decoder_open(input,&cfg,&decoder);
    if(r!=MA_SUCCESS)return fail("Could not decode the audio. Choose a valid WAV, MP3, or FLAC file.",r);
    if(ma_decoder_get_length_in_pcm_frames(&decoder,&declared_frames)==MA_SUCCESS && declared_frames>MAX_FRAMES) {
        ma_decoder_uninit(&decoder);
        return fail("Audio exceeds the one-hour limit. Split it into shorter files first.",-1);
    }
    if(!wav_open(&writer,output)){ma_decoder_uninit(&decoder);return fail("Could not create the converted audio file.",errno);}
    for(;;) {
        ma_uint64 n=0;
        if(interrupted){failed=1;reason="Audio conversion was interrupted.";break;}
        r=ma_decoder_read_pcm_frames(&decoder,buffer,8192,&n);
        if(r!=MA_SUCCESS && r!=MA_AT_END){failed=1;reason="The audio file could not be fully decoded.";break;}
        if(writer.frames+n>MAX_FRAMES){failed=1;reason="Audio exceeds the one-hour limit. Split it into shorter files first.";break;}
        if(n && !wav_write(&writer,buffer,(uint32_t)n)){failed=1;reason="Could not write converted audio. Check available disk space.";break;}
        if(!n || r==MA_AT_END)break;
    }
    ma_decoder_uninit(&decoder);
    if(!wav_close(&writer)){failed=1;reason="Could not finalize converted audio. Check available disk space.";}
    if(!failed && writer.frames==0){failed=1;reason="The selected audio file contains no samples.";}
    if(failed){remove_output(output);return fail(reason,-1);}
    printf("{\"duration\":%.6f,\"sampleRate\":16000,\"channels\":1}\n",writer.frames/(double)AUDIO_RATE);fflush(stdout);return 0;
}

static int run(int argc,char **argv) {
    const char *input=NULL,*output=NULL,*device=NULL;double limit=MAX_SECONDS;int i;
    setlocale(LC_NUMERIC,"C");signal(SIGINT,on_signal);signal(SIGTERM,on_signal);
    if(argc==2 && !strcmp(argv[1],"devices"))return devices_command();
    if(argc==2 && !strcmp(argv[1],"--version")){puts("SpeechDesk audio 1.0.0 (miniaudio 0.11.25)");return 0;}
    if(argc<2)return fail("Usage: whistle-audio devices | record --output PATH [--device ID] [--max-seconds 3600] | convert --input PATH --output PATH",-1);
    if(strcmp(argv[1],"record") && strcmp(argv[1],"convert"))return fail("Unknown command.",-1);
    for(i=2;i<argc;++i) {
        if(i+1>=argc)return fail("An option is missing its value.",-1);
        if(!strcmp(argv[i],"--output") && !output)output=argv[++i];
        else if(!strcmp(argv[i],"--input") && !input && !strcmp(argv[1],"convert"))input=argv[++i];
        else if(!strcmp(argv[i],"--device") && !device && !strcmp(argv[1],"record"))device=argv[++i];
        else if(!strcmp(argv[i],"--max-seconds") && !strcmp(argv[1],"record")) {
            char *end=NULL;errno=0;limit=strtod(argv[++i],&end);
            if(errno || !end || *end || !isfinite(limit) || limit<0.1 || limit>MAX_SECONDS)return fail("The recording limit must be between 0.1 and 3600 seconds.",-1);
        } else return fail("Unknown or duplicate option.",-1);
    }
    if(!output || !*output)return fail("An output file is required.",-1);
    if(!strcmp(argv[1],"convert")){if(!input || !*input)return fail("An input file is required.",-1);return convert_command(input,output);}
    return record_command(output,device,limit);
}
#ifdef _WIN32
int wmain(int argc,wchar_t **wargv) {
    char **argv=(char **)calloc((size_t)argc+1,sizeof(char *));int i,result;
    if(!argv)return 1;
    for(i=0;i<argc;++i) {
        int n=WideCharToMultiByte(CP_UTF8,0,wargv[i],-1,NULL,0,NULL,NULL);
        if(n<=0 || !(argv[i]=(char *)malloc((size_t)n)))return 1;
        WideCharToMultiByte(CP_UTF8,0,wargv[i],-1,argv[i],n,NULL,NULL);
    }
    result=run(argc,argv);for(i=0;i<argc;++i)free(argv[i]);free(argv);return result;
}
#else
int main(int argc,char **argv) {return run(argc,argv);}
#endif
