#pragma once
#include "road_vector.h"
namespace road_vector {
// Extract a single gzip member's raw DEFLATE bytes. Actual inflation is supplied
// by the ESP32 ROM (host tooling uses Python zlib). No optional header is trusted.
inline bool gzip_payload(Bytes b,Bytes &raw,uint32_t &crc,uint32_t &size) {
    if(b.n<18||b.p[0]!=31||b.p[1]!=139||b.p[2]!=8||(b.p[3]&0xe0)) return false;
    size_t p=10,end=b.n-8; unsigned flags=b.p[3];
    if(flags&4) {
        if(p+2>end) return false;
        size_t n=b.p[p]|size_t(b.p[p+1])<<8;p+=2;if(n>end-p)return false;p+=n;
    }
    for(unsigned f=8;f<=16;f*=2) if(flags&f) { while(p<end&&b.p[p])++p;if(p==end)return false;++p; }
    if(flags&2) { if(p+2>end)return false;uint32_t c=crc32(b.p,p);if((c&65535)!=(b.p[p]|uint32_t(b.p[p+1])<<8))return false;p+=2; }
    if(p>=end)return false;
    raw={b.p+p,end-p};crc=u32(b.p+end);size=u32(b.p+end+4);
    return size<=kMaxTileInputBytes;
}
}
