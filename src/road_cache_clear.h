#pragma once
#include <stddef.h>
#include <string.h>
#include <stdio.h>

namespace road_cache {
enum class Scan { entry, end, missing, error };
inline bool suffix(const char *name,const char *ending) {
    size_t n=strlen(name),e=strlen(ending);if(n<e)return false;
    name+=n-e;
    while(*ending) {
        char c=*name++;if(c>='A'&&c<='Z')c+=32;
        if(c!=*ending++)return false;
    }
    return true;
}
inline bool removable(const char *name) {
    if(!name||!*name||strchr(name,'/')||strchr(name,'\\')||!strcmp(name,".")||!strcmp(name,".."))return false;
    return suffix(name,".bin")||suffix(name,".bin.tmp")||
        !strcmp(name,"_download.pts")||!strcmp(name,"_download.part");
}
// Storage opens only these two fixed directories and closes each entry before
// deletion. One entry per step keeps clearing a large cache out of the HTTP handler.
class ClearJob {
    unsigned directory_=0;
    bool opened_=false;
public:
    enum State { idle,running,complete,failed } state=idle;
    unsigned deleted=0;
    const char *error="";
    void start() {directory_=0;opened_=false;deleted=0;error="";state=running;}
    template<class Storage> void step(Storage &storage) {
        if(state!=running)return;
        const char *dir=directory_==0?"/roads":"/roads/v2";
        if(!opened_) {
            Scan s=storage.open(dir);
            if(s==Scan::missing){advance(storage);return;}
            if(s!=Scan::entry){fail(storage,"cannot open road cache");return;}
            opened_=true;return;
        }
        char name[128];bool directory=false;
        Scan s=storage.next(name,sizeof(name),directory);
        if(s==Scan::end){advance(storage);return;}
        if(s!=Scan::entry){fail(storage,"cannot scan road cache");return;}
        if(directory||!removable(name))return;
        char path[160];snprintf(path,sizeof(path),"%s/%s",dir,name);
        if(!storage.remove(path)){fail(storage,"cannot remove road cache file");return;}
        ++deleted;
    }
private:
    template<class Storage> void advance(Storage &storage) {
        storage.close();opened_=false;
        if(++directory_==2)state=complete;
    }
    template<class Storage> void fail(Storage &storage,const char *message) {
        storage.close();opened_=false;error=message;state=failed;
    }
};
}
