#pragma once
#include <stdint.h>
#include <stddef.h>
#include <algorithm>

namespace road_http {
// Nonblocking transport: available(), read(dst,n), connected(), cancelled().
// Reads only advertised bytes and retries temporary TLS no-data results under
// independent idle/total deadlines. No HTTPClient scratch allocation or readBytes.
template<class Transport,class Clock> class BodyReader {
    Transport &source;Clock &clock;
    uint32_t started,lastProgress;
    uint8_t buffer[1024];size_t pos=0,len=0;
    uint8_t *out;size_t capacity;
    void (*progress)(void *,size_t)=nullptr;void *progressContext=nullptr;
public:
    const char *error="";
    size_t bytes=0;
    unsigned readCalls=0,idlePolls=0;
    BodyReader(Transport &s,Clock &c,uint8_t *p,size_t cap,void (*report)(void *,size_t)=nullptr,void *ctx=nullptr)
        :source(s),clock(c),started(c.now()),lastProgress(started),out(p),capacity(cap),progress(report),progressContext(ctx){}
    bool read(int expected,bool chunked) {
        if(!chunked&&expected>=0&&size_t(expected)>capacity)return fail("tile input limit");
        if(chunked) {
            unsigned chunks=0;
            while(++chunks<=131072) {
                char line[128];if(!read_line(line,sizeof(line)))return false;
                size_t count=0;unsigned digits=0;const char *p=line;
                for(;*p&&*p!=';';++p) {
                    unsigned d=*p>='0'&&*p<='9'?unsigned(*p-'0'):*p>='a'&&*p<='f'?unsigned(*p-'a'+10):*p>='A'&&*p<='F'?unsigned(*p-'A'+10):16;
                    if(d==16||count>capacity/16||count*16+d>capacity)return fail("invalid tile chunk size");
                    count=count*16+d;++digits;
                }
                if(!digits)return fail("invalid tile chunk size");
                if(count==0) {
                    size_t trailers=0;
                    do {if(!read_line(line,sizeof(line)))return false;trailers+=strlen_local(line)+2;if(trailers>2048)return fail("tile trailer limit");}while(*line);
                    return true;
                }
                if(!copy(count))return false;
                if(get()!=13||get()!=10)return fail("invalid tile chunk ending");
            }
            return fail("tile chunk limit");
        }
        if(expected>=0)return copy(size_t(expected));
        while(true) {
            int c=get();if(c==-1)return true;if(c<0)return false;
            if(bytes==capacity)return fail("tile input limit");out[bytes++]=uint8_t(c);report_progress();
        }
    }
private:
    static size_t strlen_local(const char *s){const char *p=s;while(*p)++p;return size_t(p-s);}
    bool fail(const char *why){if(!*error)error=why;return false;}
    void report_progress(){if(progress&&(bytes&1023)==0)progress(progressContext,bytes);}
    int get() {
        if(source.cancelled()){fail("cancelled");return -2;}
        if(clock.now()-started>=30000){fail("tile body total timeout");return -2;}
        while(pos==len) {
            int available=source.available();
            if(available>0) {
                size_t want=std::min(size_t(available),sizeof(buffer));++readCalls;
                int got=source.read(buffer,want);
                if(got>int(want)){fail("invalid tile transport read");return -2;}
                if(got>0){pos=0;len=size_t(got);lastProgress=clock.now();break;}
            }
            if(!source.connected()&&source.available()<=0)return -1;
            if(source.cancelled()){fail("cancelled");return -2;}
            if(clock.now()-started>=30000){fail("tile body total timeout");return -2;}
            if(clock.now()-lastProgress>=10000){fail("tile body idle timeout");return -2;}
            ++idlePolls;clock.pause();
        }
        return buffer[pos++];
    }
    bool copy(size_t n) {
        if(n>capacity-bytes)return fail("tile input limit");
        while(n--) {int c=get();if(c<0)return fail("tile connection closed before body complete");out[bytes++]=uint8_t(c);report_progress();}
        return true;
    }
    bool read_line(char *line,size_t cap) {
        size_t n=0;
        while(n+1<cap) {
            int c=get();if(c<0)return fail("tile connection closed during chunk header");
            if(c==13){if(get()!=10)return fail("invalid tile chunk header");line[n]=0;return true;}
            if(c==0||c==10)return fail("invalid tile chunk header");
            line[n++]=char(c);
        }
        return fail("tile chunk header limit");
    }
};
}
