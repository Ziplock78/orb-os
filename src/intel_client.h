#pragma once

#include "intel.h"

// Fetch headlines from the Orb's gateway.
//
// `topics` is a comma-separated list the worker understands (general, world, sports,
// science, tech, business, space); nullptr or empty asks for general. `source` is bbc or
// guardian (nullptr/empty asks bbc); a topic with no feed for that source is not a failure,
// the gateway substitutes what the topic actually has and says so in its own log, same
// graceful-fallback shape the aircraft feed uses when one edge refuses a request. `want` is
// how many headlines to bring back, 1..INTEL_MAX_ITEMS.
bool intel_fetch(const char *topics, const char *source, int want, IntelSnapshot &out);
