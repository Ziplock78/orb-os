#pragma once
#include <stddef.h>
#ifdef ARDUINO
class WebServer;
namespace road_vector_download {
void prepareWorker(); // startup: reserve internal network stack before heap fragmentation
void begin(WebServer &server);
// UI task only. No network, at most one bounded SD operation per pass.
void step(double lat,double lon,double range,bool enabled,bool sdBusy);
void progressText(char *out,size_t capacity); // UI task: current phase, live KB, saved/total
}
#endif
