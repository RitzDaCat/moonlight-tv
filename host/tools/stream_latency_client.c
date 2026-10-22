// SPDX-License-Identifier: GPL-3.0-only
/** @file stream_latency_client.c
 * @brief Localhost-only Moonlight receiver for the unchanged wire host-latency metric.
 * Does not launch apps, pair with the live server, decode, display, or send input.
 */
#define _POSIX_C_SOURCE 200809L
#include <Limelight.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static atomic_int ended;
static FILE *encoded;
static unsigned frames, missing, last_frame, samples, zero_latency;
#ifdef OWNED_FEC_VALIDATION
typedef struct _reed_solomon reed_solomon;
static atomic_uint fec_recoveries, fec_recovery_errors;
/** @brief Linker wrapper counts the unchanged native client's actual recovery calls. */
extern int __real_reed_solomon_decode(reed_solomon *, unsigned char **, unsigned char *, int, int);
int __wrap_reed_solomon_decode(reed_solomon *rs, unsigned char **data, unsigned char *marks, int count, int bytes) {
    int result=__real_reed_solomon_decode(rs,data,marks,count,bytes);
    atomic_fetch_add(result==0?&fec_recoveries:&fec_recovery_errors,1);
    return result;
}
#else
static unsigned fec_recoveries, fec_recovery_errors;
#endif
#define MAX_FRAMES 16384
static uint16_t latency[MAX_FRAMES];
static uint64_t assembly_us, total_bytes;
static int expected_format;
static unsigned expected_width=1920,expected_height=1080;
static int collect_frame_times;
static struct frame_time { unsigned frame; uint64_t arrival_ns,first_packet_us,enqueue_us; uint16_t wire_latency; } frame_times[MAX_FRAMES];
static unsigned time_count;

/** @brief Require the requested real stream format and geometry. */
static int setup(int format,int width,int height,int fps,void *context,int flags) {
    (void)context;(void)flags;
    fprintf(stderr,"Stream format=%d geometry=%dx%d fps=%d\n",format,width,height,fps);
    return width==(int)expected_width&&height==(int)expected_height&&format==expected_format ? 0 : -1;
}
/** @brief Record packet readiness, not decoder or display completion. */
static int receive(PDECODE_UNIT unit) {
    if(collect_frame_times){
        if(time_count>=MAX_FRAMES){atomic_store(&ended,-1);return DR_NEED_IDR;}
        struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
        frame_times[time_count++]=(struct frame_time){(unsigned)unit->frameNumber,
            (uint64_t)now.tv_sec*1000000000ull+(uint64_t)now.tv_nsec,
            unit->receiveTimeUs,unit->enqueueTimeUs,unit->frameHostProcessingLatency};
    }
    unsigned current=(unsigned)unit->frameNumber;
    if(frames&&current>last_frame+1)missing+=current-last_frame-1;
    last_frame=current;++frames;
    if(frames>20) {
        if(unit->frameHostProcessingLatency&&samples<MAX_FRAMES)latency[samples++]=unit->frameHostProcessingLatency;
        else if(!unit->frameHostProcessingLatency)++zero_latency;
        if(unit->enqueueTimeUs>=unit->receiveTimeUs)assembly_us+=unit->enqueueTimeUs-unit->receiveTimeUs;
    }
    unsigned length=0;
    for(PLENTRY entry=unit->bufferList;entry;entry=entry->next) {
        if(entry->length<0){atomic_store(&ended,-1);return DR_NEED_IDR;}
        length+=(unsigned)entry->length;
        if(encoded&&fwrite(entry->data,1,(size_t)entry->length,encoded)!=(size_t)entry->length){atomic_store(&ended,-1);return DR_NEED_IDR;}
    }
    if(length!=(unsigned)unit->fullLength){atomic_store(&ended,-1);return DR_NEED_IDR;}
    total_bytes+=length;
    return DR_OK;
}
/** @brief Report transport failure without calling stop from a library callback. */
static void terminated(int error){fprintf(stderr,"Connection terminated: %d\n",error);atomic_store(&ended,error ? -1 : 1);}
static void failed(int stage,int error){fprintf(stderr,"Stage %d failed: %d\n",stage,error);atomic_store(&ended,-1);}
static int compare(const void *a,const void *b){return *(const uint16_t*)a-*(const uint16_t*)b;}
static double seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
/** @brief Decode one test-session key; never log it. */
static int key(const char *text,char *bytes) {
    if(strlen(text)!=32)return -1;
    for(unsigned i=0;i<16;++i){unsigned value;if(sscanf(text+i*2,"%2x",&value)!=1)return -1;bytes[i]=(char)value;}
    return 0;
}
/** @brief Read an ephemeral session descriptor created by the isolated runner. */
int main(int argc,char **argv) {
    if(argc==2&&!strcmp(argv[1],"--launch-query")){puts(LiGetLaunchUrlQueryParameters());return 0;}
    if(argc<2||argc>4){fprintf(stderr,"Usage: stream_latency_client SESSION_DESCRIPTOR [TEMP_ENCODED_STREAM [TEMP_FRAME_TIMES_CSV]]\n");return 1;}
    collect_frame_times=argc==4;
    FILE *config=fopen(argv[1],"r");if(!config){perror("session descriptor");return 1;}
    char line[8][2048];
    for(unsigned i=0;i<8;++i){if(!fgets(line[i],sizeof(line[i]),config)){fclose(config);return 1;}line[i][strcspn(line[i],"\r\n")]=0;}
    char geometry[64];if(fgets(geometry,sizeof(geometry),config)){
        if(sscanf(geometry,"%ux%u",&expected_width,&expected_height)!=2||
           !((expected_width==1280&&expected_height==720)||(expected_width==1920&&expected_height==1080)||(expected_width==2560&&expected_height==1440)||(expected_width==3840&&expected_height==2160))){fclose(config);return 1;}
    }
    fclose(config);
    if(strncmp(line[2],"rtsp://127.0.0.1:",17)&&strncmp(line[2],"rtspenc://127.0.0.1:",20)){fprintf(stderr,"Only owned localhost sessions allowed\n");return 1;}
    SERVER_INFORMATION server;LiInitializeServerInformation(&server);server.address="127.0.0.1";server.serverInfoAppVersion=line[0];server.serverInfoGfeVersion=line[1];server.rtspSessionUrl=line[2];server.serverCodecModeSupport=atoi(line[3]);
    STREAM_CONFIGURATION stream;LiInitializeStreamConfiguration(&stream);stream.width=expected_width;stream.height=expected_height;stream.fps=120;stream.bitrate=23000;stream.packetSize=1392;stream.streamingRemotely=STREAM_CFG_LOCAL;stream.audioConfiguration=AUDIO_CONFIGURATION_STEREO;stream.clientRefreshRateX100=12000;stream.colorSpace=COLORSPACE_REC_709;
    expected_format=!strcmp(line[6],"av1") ? VIDEO_FORMAT_AV1_MAIN8 : VIDEO_FORMAT_H265;stream.supportedVideoFormats=expected_format;
    if(key(line[4],stream.remoteInputAesKey)||key(line[5],stream.remoteInputAesIv))return 1;
    unsigned duration=(unsigned)strtoul(line[7],NULL,10);if(duration<2||duration>120)return 1;
    if(argc>=3){encoded=fopen(argv[2],"wb");if(!encoded){perror("encoded output");return 1;}}
    CONNECTION_LISTENER_CALLBACKS connection;LiInitializeConnectionCallbacks(&connection);connection.connectionTerminated=terminated;connection.stageFailed=failed;
    DECODER_RENDERER_CALLBACKS video;LiInitializeVideoCallbacks(&video);video.setup=setup;video.submitDecodeUnit=receive;video.capabilities=CAPABILITY_DIRECT_SUBMIT;
    double start=seconds();int result=LiStartConnection(&server,&stream,&connection,&video,NULL,NULL,0,NULL,0);
    if(!result){start=seconds();while(seconds()-start<duration&&!atomic_load(&ended)){struct timespec t={0,20000000};nanosleep(&t,NULL);}LiStopConnection();}
    double elapsed=seconds()-start;
    if(encoded&&fclose(encoded))result=-1;
    if(collect_frame_times){FILE *times=fopen(argv[3],"w");if(!times)return 2;
        if(fprintf(times,"frame,arrival_ns,first_packet_us,enqueue_us,wire_latency_100us\n")<0)result=-1;
        for(unsigned i=0;i<time_count;++i){struct frame_time *t=&frame_times[i];if(fprintf(times,"%u,%llu,%llu,%llu,%u\n",t->frame,(unsigned long long)t->arrival_ns,(unsigned long long)t->first_packet_us,(unsigned long long)t->enqueue_us,t->wire_latency)<0)result=-1;}
        if(fclose(times))result=-1;
    }
    if(result||atomic_load(&ended)<0||samples<100){fprintf(stderr,"Incomplete stream: result=%d frames=%u samples=%u\n",result,frames,samples);return 2;}
    double sum=0;for(unsigned i=0;i<samples;++i)sum+=latency[i]/10.0;qsort(latency,samples,sizeof(*latency),compare);
    printf("{\"fec_recoveries\":%u,\"fec_recovery_errors\":%u,\"frames\":%u,\"missing_frames\":%u,\"host_latency_samples\":%u,\"not_applicable_latency_frames\":%u,\"bytes\":%llu,\"seconds\":%.3f,\"host_processing_ms\":{\"mean\":%.4f,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f},\"mean_assembly_us\":%.3f,\"boundary\":\"Sunshine wire host span; no TV decode, game input or presentation\"}\n",fec_recoveries,fec_recovery_errors,frames,missing,samples,zero_latency,(unsigned long long)total_bytes,elapsed,sum/samples,latency[samples/2]/10.0,latency[samples*95/100]/10.0,latency[samples*99/100]/10.0,frames>20 ? (double)assembly_us/(frames-20) : 0);
    return 0;
}
