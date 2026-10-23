#include "roads_sd.h"
#include "road_vector.h"
#include "map_snapshot.h"
#include <assert.h>
#include <fstream>
#include <string>
#include <vector>
#include <stdlib.h>
#include <iostream>

static unsigned drawn=0,allocations=0,freed=0;
static bool refuse=false;
extern "C" void lv_draw_line_dsc_init(lv_draw_line_dsc_t *d){memset(d,0,sizeof(*d));}
extern "C" void lv_draw_line(lv_draw_ctx_t *,const lv_draw_line_dsc_t *,const lv_point_t *,const lv_point_t *){++drawn;}
extern "C" void *lv_mem_alloc(size_t n){++allocations;return refuse?nullptr:malloc(n);}
extern "C" void lv_mem_free(void *p){++freed;free(p);}
extern "C" void lv_img_cache_invalidate_src(const void *){}
extern "C" uint32_t lv_snapshot_buf_size_needed(lv_obj_t *,lv_img_cf_t){return 651468;}
extern "C" lv_res_t lv_snapshot_take_to_buf(lv_obj_t *,lv_img_cf_t cf,lv_img_dsc_t *image,void *pixels,uint32_t capacity){
    assert(capacity==651468);image->data=static_cast<uint8_t *>(pixels);image->data_size=capacity;image->header.cf=cf;return LV_RES_OK;
}
static void write(const std::string &path,const std::vector<uint8_t> &b){
    std::ofstream f(path,std::ios::binary);assert(f);f.write(reinterpret_cast<const char *>(b.data()),b.size());assert(f);
}
int main(int argc,char **argv){
    assert(argc==2);const std::string root=argv[1];
    {
        radar::MapSnapshot snap;assert(snap.take(nullptr));const void *p=snap.image()->data;
        refuse=true;unsigned before=allocations;
        for(int i=0;i<100;++i)assert(snap.take(nullptr)&&snap.image()->data==p);
        assert(allocations==before);
    }
    assert(freed==1);
    {radar::MapSnapshot snap;assert(!snap.take(nullptr)&&!snap.image());}
    refuse=false;
    using namespace road_vector;
    Tile t{10,194,387};double lat=latitude(387.5,1024),lon=194.5/1024*360-180;
    std::remove((root+"/roads/r40_-112.bin").c_str());
    std::remove((root+"/roads/v2/0000007b_10_194_387.bin").c_str());
    roads_sd::set_root(root.c_str());roads_sd::init();roads_sd::set_vector_source(123);
    auto project=[&](){assert(roads_sd::project(lat,lon,30,233,233,218));drawn=0;roads_sd::draw(nullptr,lv_color_t{},150,1);return drawn;};
    assert(project()==0);
    // Legacy binary containing one local segment remains readable without v2.
    std::vector<uint8_t> legacy(16);legacy[0]=1;legacy[2]=2;legacy[6]=2;
    int16_t coords[4]={int16_t(lround((lat-.02)*180)),int16_t(lround(lon*180)),int16_t(lround((lat+.02)*180)),int16_t(lround(lon*180))};
    memcpy(legacy.data()+8,coords,8);write(root+"/roads/r40_-112.bin",legacy);assert(project()==1);
    std::vector<uint8_t> cache(HEADER+8);memcpy(cache.data(),"ORV1",4);
    put32(cache.data()+4,t.z);put32(cache.data()+8,t.x);put32(cache.data()+12,t.y);
    put32(cache.data()+16,EXTENT);put32(cache.data()+20,1);put32(cache.data()+24,123);
    uint16_t points[4]={4000,8192,12000,8192};
    for(int i=0;i<4;++i){cache[HEADER+i*2]=uint8_t(points[i]);cache[HEADER+i*2+1]=uint8_t(points[i]>>8);}
    put32(cache.data()+28,crc32(cache.data()+HEADER,8));
    char path[96];road_vector::path(path,sizeof(path),t,123);write(root+path,cache);
    assert(project()==1); // v2 supersedes legacy, no doubled strokes
    lv_point_t projected[4];uint16_t lengths[2];bool ok;
    assert(roads_sd::project_flat(lat,lon,30,233,233,218,projected,4,lengths,2,ok)==1&&ok);
    assert(projected[0].y==233&&projected[1].y==233&&projected[0].x<233&&projected[1].x>233);
    cache.back()^=1;write(root+path,cache);assert(project()==1); // corrupt v2 falls back to legacy
    cache.resize(HEADER);put32(cache.data()+20,0);put32(cache.data()+28,0);write(root+path,cache);
    assert(project()==0); // validated empty vector coverage suppresses legacy
    unsigned rev=roads_sd::revision();roads_sd::published();assert(roads_sd::revision()==rev+1);
    assert(roads_sd::project(lat,lon,0,233,233,218));drawn=0;roads_sd::draw(nullptr,lv_color_t{},150,1);assert(drawn==0);
    std::remove((root+path).c_str());std::remove((root+"/roads/r40_-112.bin").c_str());
    cache.resize(HEADER+8);put32(cache.data()+20,1);
    for(int i=0;i<4;++i){cache[HEADER+i*2]=uint8_t(points[i]);cache[HEADER+i*2+1]=uint8_t(points[i]>>8);}
    put32(cache.data()+28,crc32(cache.data()+HEADER,8));write(root+path,cache);assert(project()==1);
    rev=roads_sd::revision();roads_sd::set_cache_clearing(true);
    assert(roads_sd::revision()==rev+1&&project()==0);
    ok=false;assert(roads_sd::project_flat(lat,lon,30,233,233,218,projected,4,lengths,2,ok)==0&&ok);
    roads_sd::set_cache_clearing(false);assert(roads_sd::revision()==rev+2&&project()==1);
    std::remove((root+path).c_str());
    std::cout<<"vector SD reader, projection, fallback and snapshot tests passed\n";
}
