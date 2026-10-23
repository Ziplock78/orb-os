#include "road_vector.h"
#include "road_vector_gzip.h"
#include <assert.h>
#include <vector>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdlib.h>
using namespace road_vector;
using V=std::vector<uint8_t>;
static void var(V &v,uint64_t n) {while(n>127){v.push_back(uint8_t(n)|128);n>>=7;}v.push_back(uint8_t(n));}
static void num(V &v,unsigned f,uint64_t n){var(v,f*8);var(v,n);}
static void msg(V &v,unsigned f,const V &b){var(v,f*8+2);var(v,b.size());v.insert(v.end(),b.begin(),b.end());}
static void str(V &v,unsigned f,const char *s){msg(v,f,V(s,s+strlen(s)));}
static uint64_t zz(int n){return n<0?uint64_t(-int64_t(n)*2-1):uint64_t(n)*2;}
static V geom(int x0=0,int y0=0,int x1=4096,int y1=4096) {
    V g;var(g,9);var(g,zz(x0));var(g,zz(y0));var(g,10);var(g,zz(x1-x0));var(g,zz(y1-y0));return g;
}
static V feat(unsigned cls=0,bool ramp=false,V g=geom(),unsigned type=2) {
    V f,t;var(t,0);var(t,cls);if(ramp){var(t,1);var(t,3);}msg(f,2,t);num(f,3,type);msg(f,4,g);return f;
}
static V tile(const std::vector<V> &features,uint32_t extent=4096) {
    V l,b; // Features deliberately precede name, version and dictionaries.
    for(const auto &f:features)msg(l,2,f);
    str(l,1,"transportation");num(l,15,2);num(l,5,extent);
    str(l,3,"class");str(l,3,"ramp");
    for(const char *s:{"motorway","trunk","primary"}){V v;str(v,1,s);msg(l,4,v);}
    V value;num(value,7,1);msg(l,4,value);msg(b,3,l);return b;
}
static bool unpack(const V &v,V &out,Stats &s,size_t cap=kMaxRoadCacheBytes) {
    out.resize(cap);size_t n=0;
    bool ok=decode({v.data(),v.size()},{10,194,387},123,out.data(),out.size(),n,s);
    out.resize(n);return ok;
}
static bool cancel(void *) {return false;}
static void tests() {
    Stats s;V out;
    assert(unpack(tile({feat(),feat(1),feat(2),feat(0,true),feat(0,false,geom(),3)}),out,s));
    assert(s.features==5&&s.roads==2&&s.segments==2&&out.size()==HEADER+16);
    assert(cache_valid({out.data(),out.size()},{10,194,387},123));
    assert(!cache_valid({out.data(),out.size()},{10,195,387},123));
    assert(!cache_valid({out.data(),out.size()},{10,194,387},124));
    out.back()^=1;assert(!cache_valid({out.data(),out.size()},{10,194,387},123));
    assert(unpack(tile({feat(0,false,geom(-4096,2048,8192,2048))}),out,s));
    assert(out[HEADER]==0&&out[HEADER+1]==0&&out[HEADER+4]==0&&out[HEADER+5]==64);
    assert(unpack(tile({feat(0,false,geom(-10,-10,-1,-1))}),out,s)&&s.segments==0);
    assert(unpack(tile({feat(0,false,geom(4096,0,4096,4000))}),out,s)&&s.segments==0);
    assert(unpack(tile({feat(0,false,geom(0,0,0,4000))}),out,s)&&s.segments==1);
    assert(unpack(tile({feat(2)}),out,s)&&out.size()==HEADER); // valid empty coverage
    assert(!unpack(tile({feat(99)}),out,s));
    assert(!unpack(tile({feat()},0),out,s));
    assert(!unpack(tile({feat()}),out,s,HEADER));
    assert(!unpack(V{26,255,255,255,255,255,255,255,255,255,255},out,s));
    V bad=tile({feat()});bad.pop_back();assert(!unpack(bad,out,s));
    V g=geom();var(g,9);var(g,zz(-2048));var(g,zz(-2048));var(g,10);var(g,2);var(g,2);
    assert(unpack(tile({feat(0,false,g)}),out,s)&&s.segments==2); // multipart cursor persists
    assert(!unpack(tile({feat(0,false,V{10,0,0})}),out,s)); // LineTo without MoveTo
    assert(!unpack(tile({feat(0,false,V{9,0,0,15})}),out,s)); // ClosePath invalid for lines
    assert(!unpack(tile({feat(0,false,V{9,0,0})}),out,s));
    V input=tile({feat()});out.resize(kMaxRoadCacheBytes);size_t n;
    size_t measured=0;assert(decode({input.data(),input.size()},{10,194,387},123,nullptr,kMaxRoadCacheBytes,measured,s));
    assert(measured==HEADER+8);
    assert(!decode({input.data(),input.size()},{10,194,387},123,out.data(),out.size(),n,s,cancel));
    assert(n==0&&!strcmp(s.error,"cancelled"));
    Tile p[MAX_TILES];assert(plan(40.2,-111.65,30,p)==9);
    assert(plan(40.2,-111.65,10,p)==4);
    size_t count=plan(80,179.99,100,p);assert(count>0&&count<=MAX_TILES);
    bool wrapped=false;for(size_t i=0;i<count;++i){assert(valid(p[i]));if(p[i].x==0)wrapped=true;for(size_t j=0;j<i;++j)assert(!same(p[i],p[j]));}assert(wrapped);
    assert(plan(90,0,30,p)==0&&plan(NAN,0,30,p)==0&&plan(0,0,-1,p)==0);
    double x0=-20,y0=0,x1=20,y1=0;assert(clip_circle(x0,y0,x1,y1,10)&&fabs(x0+10)<1e-9&&fabs(x1-10)<1e-9);
    x0=0;y0=0;x1=1;y1=1;assert(clip_circle(x0,y0,x1,y1,10)&&x0==0&&x1==1);
    x0=20;y0=20;x1=30;y1=30;assert(!clip_circle(x0,y0,x1,y1,10));
    double px,py;project({10,194,387},8192,8192,latitude(387.5,1024),(194.5/1024)*360-180,30,218,px,py);
    assert(fabs(px)<1e-8&&fabs(py)<1e-8);
    V gz={31,139,8,0,0,0,0,0,0,3,3,0,0,0,0,0,0,0,0,0};Bytes raw;uint32_t crc,size;
    assert(gzip_payload({gz.data(),gz.size()},raw,crc,size)&&raw.n==2&&size==0);
    gz[3]=32;assert(!gzip_payload({gz.data(),gz.size()},raw,crc,size));
    assert(crc32(reinterpret_cast<const uint8_t *>("123456789"),9)==0xcbf43926);
    std::cout<<"vector road tests passed\n";
}
int main(int argc,char **argv) {
    if(argc==7) {
        std::ifstream file(argv[1],std::ios::binary);V input((std::istreambuf_iterator<char>(file)),{}),output(kMaxRoadCacheBytes);
        if(!file||input.size()>kMaxTileInputBytes){std::cerr<<"input file\n";return 1;}
        Tile t{uint32_t(strtoul(argv[3],nullptr,10)),uint32_t(strtoul(argv[4],nullptr,10)),uint32_t(strtoul(argv[5],nullptr,10))};
        Stats stats;size_t n;
        if(!decode({input.data(),input.size()},t,source_id(argv[6]),output.data(),output.size(),n,stats)) {std::cerr<<stats.error<<"\n";return 1;}
        std::ofstream out(argv[2],std::ios::binary);out.write(reinterpret_cast<char *>(output.data()),n);
        if(!out)return 1;
        std::cout<<"features="<<stats.features<<" roads="<<stats.roads<<" segments="<<stats.segments<<" cacheBytes="<<n<<"\n";return 0;
    }
    tests();return 0;
}
