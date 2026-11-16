#pragma once
#include <mutex>

#include "gpu/state.h"

namespace d3d {

gpu::State& State();
std::recursive_mutex& Lock();
void LogPush();  // a crash: where the push buffer and the scratch buffer stand

}  // namespace d3d
