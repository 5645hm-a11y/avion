#pragma once
// The CPU survey's vector levels (HorizonLevel::Accumulate, docs/picture.md "The CPU survey"): SSE2 where the target has it
// (x86-64 always), AVX2 where the processor and the system have it too, unless MEDIA_SURVEY_SCALAR asks for the scalar loops
// everywhere (the portable suite builds both). Internal to src/video.
#if !defined(MEDIA_SURVEY_SCALAR) && (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#define MEDIA_SURVEY_SSE2 1
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#define MEDIA_SURVEY_AVX2_TARGET
#else
#define MEDIA_SURVEY_AVX2_TARGET __attribute__((target("avx2")))
#endif
#else
#define MEDIA_SURVEY_SSE2 0
#endif

namespace media::video::detail {
// 0 the scalar loops, 1 SSE2, 2 AVX2: the best this processor and system have.
inline unsigned SurveyAvailable() noexcept {
#if MEDIA_SURVEY_SSE2
#if defined(_MSC_VER)
    int info[4]{}; __cpuid(info, 0);
    if (info[0] < 7) return 1;
    __cpuid(info, 1);
    const bool os = (info[2] & (1 << 27)) != 0 && (info[2] & (1 << 28)) != 0 && (_xgetbv(0) & 6) == 6; // OSXSAVE, AVX, the YMM state saved.
    __cpuidex(info, 7, 0);
    return os && (info[1] & (1 << 5)) ? 2u : 1u;
#else
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") ? 2u : 1u;
#endif
#else
    return 0;
#endif
}
inline unsigned g_survey_level = SurveyAvailable();
}
