#pragma once
#include <cstdint>
bool logic_trace_install();
void logic_trace_begin(uintptr_t simulation, uintptr_t player, bool host);
void logic_trace_end();
