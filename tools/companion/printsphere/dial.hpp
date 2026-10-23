#pragma once
#include "esp_err.h"
#include "printsphere/dial_logic.hpp"

namespace printsphere::dial {
esp_err_t initialize();
Snapshot snapshot();
}
