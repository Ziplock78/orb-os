#pragma once
// Portable bounded OpenMapTiles road decoder. No Arduino, LVGL or heap allocation.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <stdio.h>

namespace road_vector {
// MAX_INPUT is a POSIX terminal macro (255) on ESP32. Avoid generic limit
// identifiers: later SDK includes can replace even namespace-qualified tokens.
constexpr size_t kMaxTileInputBytes = 512 * 1024, kMaxRoadCacheBytes = 256 * 1024;
inline bool input_capacity(int contentLength,size_t &capacity) {
    if(contentLength < -1 || contentLength>int(kMaxTileInputBytes)){capacity=0;return false;}
    capacity=contentLength>0?size_t(contentLength):kMaxTileInputBytes;return true;
}
constexpr uint32_t EXTENT = 16384;
constexpr size_t HEADER = 32, MAX_TILES = 64;
struct Tile { uint32_t z=0, x=0, y=0; };
inline bool same(Tile a, Tile b) { return a.z==b.z && a.x==b.x && a.y==b.y; }
inline uint32_t u32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
inline void put32(uint8_t *p, uint32_t v) { for (int i=0;i<4;++i) p[i]=uint8_t(v>>(i*8)); }
inline uint32_t crc_update(uint32_t c,const uint8_t *p, size_t n) {
    while(n--) { c^=*p++; for(int i=0;i<8;++i) c=(c>>1)^(0xedb88320u & (0u-(c&1))); }
    return c;
}
inline uint32_t crc32(const uint8_t *p,size_t n) { return ~crc_update(~0u,p,n); }
inline uint32_t source_id(const char *s) {
    uint32_t h=2166136261u; while(*s) { h^=uint8_t(*s++); h*=16777619u; } return h;
}
inline bool valid(Tile t) { return t.z<=14 && t.x<(1u<<t.z) && t.y<(1u<<t.z); }
inline void path(char *s, size_t n, Tile t, uint32_t source) {
    snprintf(s,n,"/roads/v2/%08lx_%lu_%lu_%lu.bin",(unsigned long)source,
             (unsigned long)t.z,(unsigned long)t.x,(unsigned long)t.y);
}
inline double tile_y(double lat, double n) {
    lat=std::max(-85.05112878,std::min(85.05112878,lat));
    return (1-log(tan(0.7853981633974483+lat*0.008726646259971648))/3.141592653589793)*n/2;
}
inline double latitude(double y, double n) { return atan(sinh(3.141592653589793*(1-2*y/n)))*57.29577951308232; }
// Bounding square deliberately includes edge tiles. Reduce zoom if the bounded
// plan would overflow, rather than silently omitting coverage at high latitudes.
inline size_t plan(double lat, double lon, double range, Tile *out) {
    if(!std::isfinite(lat)||!std::isfinite(lon)||!std::isfinite(range)||
       fabs(lat)>85.05112878||lon < -180||lon>180||range<=0||range>500) return 0;
    int z=range<=10?11:range<=30?10:range<=50?9:8;
    const double dy=range/111.195, dx=std::min(180.0,dy/std::max(0.01,cos(lat*0.0174532925199433)));
    for(;z>=0;--z) {
        int n=1<<z;
        int x0=int(floor((lon-dx+180)/360*n)), x1=int(floor((lon+dx+180)/360*n));
        int y0=std::max(0,int(floor(tile_y(lat+dy,n))));
        int y1=std::min(n-1,int(floor(tile_y(lat-dy,n))));
        if(x1-x0+1>n) x1=x0+n-1;
        if(size_t(x1-x0+1)*size_t(y1-y0+1)>MAX_TILES) continue;
        size_t count=0;
        for(int y=y0;y<=y1;++y) for(int x=x0;x<=x1;++x)
            out[count++]={uint32_t(z),uint32_t((x%n+n)%n),uint32_t(y)};
        const double hx=(lon+180)/360*n, hy=tile_y(lat,n);
        std::sort(out,out+count,[=](Tile a,Tile b) {
            double ax=fabs(a.x+0.5-hx), bx=fabs(b.x+0.5-hx);
            ax=std::min(ax,n-ax); bx=std::min(bx,n-bx);
            return ax*ax+pow(a.y+0.5-hy,2)<bx*bx+pow(b.y+0.5-hy,2);
        });
        return count;
    }
    return 0;
}

struct Bytes { const uint8_t *p=nullptr; size_t n=0; };
inline bool eq(Bytes b,const char *s) { return b.n==strlen(s)&&!memcmp(b.p,s,b.n); }
struct Reader {
    Bytes b; size_t pos=0;
    bool var(uint64_t &v) {
        v=0;
        for(unsigned i=0;i<10;++i) {
            if(pos==b.n) return false;
            uint8_t c=b.p[pos++]; if(i==9 && c>1) return false;
            v|=uint64_t(c&127)<<(7*i); if(!(c&128)) return true;
        }
        return false;
    }
    bool bytes(Bytes &v) {
        uint64_t n; if(!var(n)||n>b.n-pos) return false;
        v={b.p+pos,size_t(n)}; pos+=size_t(n); return true;
    }
    bool field(uint32_t &id,unsigned &wire) {
        uint64_t t; if(!var(t)||t>0xffffffffu||!(t>>3)) return false;
        id=uint32_t(t>>3); wire=unsigned(t&7); return true;
    }
    bool skip(unsigned wire) {
        uint64_t v; Bytes s;
        if(wire==0) return var(v);
        if(wire==2) return bytes(s);
        size_t n=wire==1?8:wire==5?4:0;
        if(!n||n>b.n-pos) return false; pos+=n; return true;
    }
};
// Liang-Barsky clipping; used for producer tile buffers and screen circle chords.
inline bool clip_box(double &x0,double &y0,double &x1,double &y1,double hi) {
    double a=0,b=1,dx=x1-x0,dy=y1-y0;
    double p[4]={-dx,dx,-dy,dy}, q[4]={x0,hi-x0,y0,hi-y0};
    for(int i=0;i<4;++i) {
        if(p[i]==0) { if(q[i]<0) return false; }
        else { double t=q[i]/p[i]; if(p[i]<0) a=std::max(a,t); else b=std::min(b,t); }
    }
    if(a>b) return false;
    x1=x0+b*dx; y1=y0+b*dy; x0+=a*dx; y0+=a*dy; return true;
}
struct Stats { uint32_t features=0,roads=0,segments=0; const char *error=""; };
using Poll = bool (*)(void *);
struct Decoder {
    uint8_t *out; size_t cap, used=HEADER; Stats stats; Poll poll=nullptr; void *ctx=nullptr;
    bool fail(const char *s) { stats.error=s; return false; }
    bool segment(double x0,double y0,double x1,double y1,uint32_t extent) {
        if(!clip_box(x0,y0,x1,y1,extent)) return true;
        // A line lying exactly on a shared border belongs to its neighbour's
        // left/top edge, preventing double opacity on tile seams.
        if((x0==extent&&x1==extent)||(y0==extent&&y1==extent))return true;
        double v[4]={x0,y0,x1,y1}; uint16_t q[4];
        for(int i=0;i<4;++i) q[i]=uint16_t(lround(v[i]*EXTENT/extent));
        if(q[0]==q[2]&&q[1]==q[3]) return true;
        if(used+8>cap) return fail("road cache limit");
        for(int i=0;i<4;++i) {
            if(out){out[used]=uint8_t(q[i]);out[used+1]=uint8_t(q[i]>>8);}used+=2;
        }
        ++stats.segments; return true;
    }
    bool geometry(Bytes b,uint32_t extent) {
        Reader r{b}; int64_t x=0,y=0; bool start=false,hasLine=false;
        size_t ops=0;
        while(r.pos<b.n) {
            uint64_t cmd; if(!r.var(cmd)||cmd>0xffffffffu) return fail("geometry command");
            unsigned op=unsigned(cmd&7), count=unsigned(cmd>>3);
            if(!count||(op!=1&&op!=2)||(op==1&&count!=1)||(op==2&&!start)) return fail("line command");
            if(op==1 && start && !hasLine) return fail("empty line");
            for(unsigned i=0;i<count;++i) {
                if(++ops>131072) return fail("geometry limit");
                if((ops&255)==0 && poll && !poll(ctx)) return fail("cancelled");
                uint64_t a,bv; if(!r.var(a)||!r.var(bv)||a>0xffffffffu||bv>0xffffffffu) return fail("geometry delta");
                int64_t nx=x+int64_t(a>>1)-(a&1?int64_t(a|1):0);
                int64_t ny=y+int64_t(bv>>1)-(bv&1?int64_t(bv|1):0);
                if(nx < -2147483648LL||nx>2147483647LL||ny < -2147483648LL||ny>2147483647LL) return fail("coordinate overflow");
                if(op==2 && !segment(double(x),double(y),double(nx),double(ny),extent)) return false;
                x=nx;y=ny;start=true;hasLine=op==2;
            }
        }
        return (start&&hasLine)||fail("empty geometry");
    }
    bool feature(Bytes b,const Bytes *keys,size_t nk,const Bytes *values,size_t nv,uint32_t extent) {
        Reader r{b}; Bytes tags,geom; uint64_t type=0;
        while(r.pos<b.n) {
            uint32_t f;unsigned w;if(!r.field(f,w)) return fail("feature field");
            if(f==2&&w==2) { if(tags.p||!r.bytes(tags)) return fail("feature tags"); }
            else if(f==3&&w==0) { if(!r.var(type)) return fail("feature type"); }
            else if(f==4&&w==2) { if(geom.p||!r.bytes(geom)) return fail("feature geometry"); }
            else if(!r.skip(w)) return fail("feature wire");
        }
        bool road=false,ramp=false;
        Reader t{tags};
        while(t.pos<tags.n) {
            uint64_t k,v;if(!t.var(k)||!t.var(v)||k>=nk||v>=nv) return fail("tag index");
            Reader val{values[v]}; Bytes str; uint64_t number=0; bool truth=false;
            while(val.pos<val.b.n) {
                uint32_t f;unsigned w;if(!val.field(f,w)) return fail("value field");
                if(f==1&&w==2) { if(!val.bytes(str)) return fail("value string"); }
                else if((f==4||f==5||f==6||f==7)&&w==0) { if(!val.var(number)) return fail("value number"); truth=number!=0; }
                else if(!val.skip(w)) return fail("value wire");
            }
            if(eq(keys[k],"class")) road=eq(str,"motorway")||eq(str,"trunk");
            if(eq(keys[k],"ramp")) ramp=truth||eq(str,"1");
        }
        ++stats.features;
        if(type!=2||!road||ramp) return true;
        ++stats.roads; return geometry(geom,extent);
    }
    bool layer(Bytes b) {
        // Fixed dictionary budget (8 KB on a 32-bit target). Features are visited
        // only after the full dictionary pass, regardless of protobuf field order.
        Bytes keys[256], values[768]; size_t nk=0,nv=0; Bytes name; uint64_t extent=4096,version=0;
        Reader r{b};
        while(r.pos<b.n) {
            uint32_t f;unsigned w;if(!r.field(f,w)) return fail("layer field");
            if(f==1&&w==2) { if(!r.bytes(name)) return fail("layer name"); }
            else if(f==5&&w==0) { if(!r.var(extent)) return fail("layer extent"); }
            else if(f==15&&w==0) { if(!r.var(version)) return fail("layer version"); }
            else if(!r.skip(w)) return fail("layer wire");
        }
        if(!eq(name,"transportation")) return true;
        if(version!=2||extent==0||extent>1048576) return fail("unsupported road layer");
        r.pos=0;
        while(r.pos<b.n) {
            uint32_t f;unsigned w;if(!r.field(f,w)) return fail("dictionary field");
            if(f==3&&w==2) { if(nk==256||!r.bytes(keys[nk++])) return fail("key limit"); }
            else if(f==4&&w==2) { if(nv==768||!r.bytes(values[nv++])) return fail("value limit"); }
            else if(!r.skip(w)) return fail("dictionary wire");
        }
        r.pos=0;
        while(r.pos<b.n) {
            uint32_t f;unsigned w;if(!r.field(f,w)) return fail("layer field");
            if(f==2&&w==2) {
                Bytes feat;if(!r.bytes(feat)||!feature(feat,keys,nk,values,nv,uint32_t(extent))) return false;
                if(stats.features>65536) return fail("feature limit");
                if(poll && (stats.features&31)==0 && !poll(ctx)) return fail("cancelled");
            } else if(!r.skip(w)) return fail("layer wire");
        }
        return true;
    }
};
inline bool decode(Bytes input,Tile tile,uint32_t source,uint8_t *out,size_t cap,size_t &len,Stats &stats,Poll poll=nullptr,void *ctx=nullptr) {
    len=0; stats={};
    if(!valid(tile)||input.n>kMaxTileInputBytes||cap<HEADER) { stats.error="input limit";return false; }
    Decoder d{out,std::min(cap,kMaxRoadCacheBytes)}; d.poll=poll;d.ctx=ctx;
    Reader r{input};
    while(r.pos<input.n) {
        uint32_t f;unsigned w;
        if(!r.field(f,w)) { d.fail("tile field");break; }
        if(f==3&&w==2) { Bytes layer;if(!r.bytes(layer)||!d.layer(layer)) { if(!*d.stats.error) d.fail("tile layer");break; } }
        else if(!r.skip(w)) { d.fail("tile wire");break; }
    }
    if(!*d.stats.error && poll && !poll(ctx)) d.fail("cancelled");
    stats=d.stats; if(*stats.error) return false;
    len=d.used;if(!out)return true; // measurement pass reserves only actual cache bytes
    memcpy(out,"ORV1",4);put32(out+4,tile.z);put32(out+8,tile.x);put32(out+12,tile.y);
    put32(out+16,EXTENT);put32(out+20,stats.segments);put32(out+24,source);
    put32(out+28,crc32(out+HEADER,d.used-HEADER));len=d.used;return true;
}
inline bool cache_header(const uint8_t *p,size_t size,Tile t,uint32_t source) {
    return size>=HEADER&&size<=kMaxRoadCacheBytes&&!memcmp(p,"ORV1",4)&&valid(t)&&
        u32(p+4)==t.z&&u32(p+8)==t.x&&u32(p+12)==t.y&&u32(p+16)==EXTENT&&u32(p+24)==source&&
        u32(p+20)<=(kMaxRoadCacheBytes-HEADER)/8&&HEADER+size_t(u32(p+20))*8==size;
}
inline bool cache_valid(Bytes b,Tile t,uint32_t source) {
    if(!cache_header(b.p,b.n,t,source)||crc32(b.p+HEADER,b.n-HEADER)!=u32(b.p+28)) return false;
    for(size_t i=HEADER;i<b.n;i+=2) if((unsigned(b.p[i])|(unsigned(b.p[i+1])<<8))>EXTENT) return false;
    return true;
}
// Convert one cached endpoint to geographic coordinates without legacy 1/180
// degree quantization. Screen projection uses the same local model as coastline.
inline void project(Tile t,uint16_t x,uint16_t y,double lat,double lon,double range,
                    double radius,double &sx,double &sy) {
    double n=double(1u<<t.z), plat=latitude(t.y+double(y)/EXTENT,n);
    double plon=(t.x+double(x)/EXTENT)/n*360-180;
    double dl=plon-lon; if(dl>180)dl-=360;if(dl < -180)dl+=360;
    double a=lat*0.0174532925199433,dlat=plat-lat;
    sx=dl*(cos(a)-sin(a)*dlat*0.0174532925199433)*(111.195*radius/range);
    sy=-dlat*(111.195*radius/range);
}
inline bool clip_circle(double &x0,double &y0,double &x1,double &y1,double radius) {
    double dx=x1-x0,dy=y1-y0,a=dx*dx+dy*dy;
    if(a==0) return false;
    double b=2*(x0*dx+y0*dy),c=x0*x0+y0*y0-radius*radius,disc=b*b-4*a*c;
    if(disc<0) return false;
    double root=sqrt(disc),lo=std::max(0.0,(-b-root)/(2*a)),hi=std::min(1.0,(-b+root)/(2*a));
    if(lo>=hi) return false;
    x1=x0+hi*dx;y1=y0+hi*dy;x0+=lo*dx;y0+=lo*dy;return true;
}
} // namespace road_vector
