#pragma once

#include "intel.h"

// Fetch headlines from the Orb's gateway.
//
// `topics` is a comma-separated list the worker understands (general, world, sports,
// science, tech, business, space); nullptr or empty asks for general. `want` is how many
// headlines to bring back, 1..INTEL_MAX_ITEMS.
bool intel_fetch(const char *topics, int want, IntelSnapshot &out);
