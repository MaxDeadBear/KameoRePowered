#include "core/time.h"

#include <chrono>

int64_t KameoSteadyMilliseconds() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
