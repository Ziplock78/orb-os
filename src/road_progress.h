#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
namespace road_progress {
enum class Phase {none,waiting,downloading,unpacking,preparing,saving,checking,retrying,failed,paused,clearing,clearFailed,needSd,needWifi,restarting};
struct Snapshot {
    Phase phase=Phase::none;
    unsigned saved=0,total=0,deleted=0,seconds=0;
    uint32_t bytes=0;int length=-1;
};
inline void format(const Snapshot &s,char *out,size_t cap) {
    if(!cap)return;out[0]=0;if(s.phase==Phase::none)return;
    if(s.phase==Phase::clearing){snprintf(out,cap,"Clearing road cache\n%u files removed",s.deleted);return;}
    if(s.phase==Phase::clearFailed){snprintf(out,cap,"Road cache clear failed\nRetry at theorb.local/roads");return;}
    char label[96];
    switch(s.phase) {
        case Phase::waiting:snprintf(label,sizeof(label),"Waiting for map service: %us",s.seconds);break;
        case Phase::downloading:
            if(s.length>0)snprintf(label,sizeof(label),"Downloading roads: %lu/%lu KB",(unsigned long)((s.bytes+1023)/1024),(unsigned long)((uint32_t(s.length)+1023)/1024));
            else snprintf(label,sizeof(label),"Downloading roads: %lu KB",(unsigned long)((s.bytes+1023)/1024));break;
        case Phase::unpacking:snprintf(label,sizeof(label),"Unpacking roads");break;
        case Phase::preparing:snprintf(label,sizeof(label),"Preparing road map");break;
        case Phase::saving:snprintf(label,sizeof(label),"Saving roads to SD");break;
        case Phase::checking:snprintf(label,sizeof(label),"Checking saved roads");break;
        case Phase::retrying:snprintf(label,sizeof(label),"Retrying roads in %us",s.seconds);break;
        case Phase::failed:snprintf(label,sizeof(label),"Road download failed");break;
        case Phase::paused:snprintf(label,sizeof(label),"Road download paused");break;
        case Phase::needSd:snprintf(label,sizeof(label),"Insert SD card for road maps");break;
        case Phase::needWifi:snprintf(label,sizeof(label),"Connect Wi-Fi to download roads");break;
        case Phase::restarting:snprintf(label,sizeof(label),"Restarting road downloads");break;
        default:return;
    }
    if(s.total)snprintf(out,cap,"%s\n%u/%u tiles saved",label,s.saved,s.total);
    else snprintf(out,cap,"%s",label);
}
}
