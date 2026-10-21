#pragma once
// Call only from loop(). Network/JSON work runs on a worker that NEVER touches SD/LVGL.
namespace roads_download {
void step(double lat, double lon, float rangeKm, bool enabled, bool transferBusy);
// UI-thread coordination with browser preloading. Existing network work is
// cancelled cooperatively; uploads wait until its queue and SD files are closed.
void browserPause(bool paused);
bool browserReady();
const char *status();
const char *lastError();
int httpStatus();      // 0 before a request, negative for an HTTPClient transport error
int responseLength(); // -1 when unknown/chunked
const char *tile();
unsigned bytesRead();
unsigned jsonPeak();
const char *roadClass();
unsigned elapsedMs();
unsigned headersMs();
unsigned downloadMs();
unsigned transportMs();
unsigned idleMs();
unsigned processingMs();
unsigned queueWaitMs();
unsigned readCalls();
unsigned emptyPolls();
unsigned roadsReceived();
unsigned completedAreas();
unsigned totalAreas();
unsigned firstSectionMs();
void progressText(char *out, unsigned size);
}
