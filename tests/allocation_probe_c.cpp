// The allocation probe (allocation_probe.hpp) for C tests (c_api_tests.c).
#include "allocation_probe.hpp"
extern "C" void media_test_allocations_begin(void) { allocation_probe::Begin(); }
extern "C" std::uint64_t media_test_allocations_end(void) { return allocation_probe::End(); }
