#pragma once
#ifdef ARDUINO
class WebServer;
namespace road_preload {
void begin(WebServer &server);
void step(bool otherTransferBusy);
}
#endif
