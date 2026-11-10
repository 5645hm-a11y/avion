#!/usr/bin/env bash
# Mutation check for tests/acoustic_intent_tests.cpp: each mutant of the recogniser or the filterbank must fail the
# suite. Run from the repository root (as build/linux/gcc.sh is). Mutants are built in build/linux/mutants.
set -uo pipefail
cd "$(dirname "$0")/../.."
out=build/linux/mutants; mkdir -p $out
W="-std=c++20 -O1 -ffp-contract=off -pthread -Iinclude -Itests"
COMMON="tests/acoustic_intent_tests.cpp src/audio/MicroVad.cpp src/ai/LocalIntentDispatcher.cpp src/text/Utf8.cpp src/linear_memory_arena.cpp src/platform/posix/SystemMemory.cpp tests/allocation_probe.cpp"
survivors=0
mutate() {
  name=$1; file=$2; from=$3; to=$4
  python3 - "$file" "$from" "$to" "$out/$name.cpp" <<'EOF' || { echo "$name: PATTERN NOT FOUND ONCE"; survivors=$((survivors+1)); return; }
import sys
s = open(sys.argv[1]).read()
if s.count(sys.argv[2]) != 1: sys.exit(1)
open(sys.argv[4], 'w').write(s.replace(sys.argv[2], sys.argv[3]))
EOF
  if [ "$file" = src/ai/AcousticIntentRecognizer.cpp ]; then rest="src/audio/FilterbankFeatures.cpp"; else rest="src/ai/AcousticIntentRecognizer.cpp"; fi
  g++ $W "$out/$name.cpp" $rest $COMMON -o "$out/$name" || { echo "$name: DID NOT COMPILE"; survivors=$((survivors+1)); return; }
  if "$out/$name" tests/data/voice_corpus.bin > "$out/$name.txt" 2>&1; then echo "$name: SURVIVED"; survivors=$((survivors+1)); else echo "$name: killed ($(grep -c '^FAIL' "$out/$name.txt") checks failed)"; fi
}
R=src/ai/AcousticIntentRecognizer.cpp; F=src/audio/FilterbankFeatures.cpp
mutate no_filler     $R 'else if (best_command == kFiller) result.match = AcousticMatch::Filler;' 'else if (false) {}'
mutate no_margin     $R 'else if (other >= 0 && other - best < set_.margin * best)' 'else if (false)'
mutate no_accept     $R 'if (best < 0 || best > set_.accept_distance)' 'if (best < 0)'
mutate no_trim       $R 'constexpr float kTrimBelowPeak = 5.0f;' 'constexpr float kTrimBelowPeak = 1e9f;'
mutate no_floor      $R 'constexpr float kFloorBelowPeak = 4.6051702f;' 'constexpr float kFloorBelowPeak = 1e9f;'
mutate no_variance   $R 'const float gain = kNormalizedScale / std::sqrt(variance + 1e-6f);' 'const float gain = 1.0f;'
mutate no_mean       $R 'c = (c - mean[k]) * gain;' 'c = c * gain;'
mutate no_deltas     $R 'constexpr float kDeltaWeight = 0.6f;' 'constexpr float kDeltaWeight = 0.0f;'
mutate no_band       $R 'band = (std::max)(longer / 5, 8u) + (n > m ? n - m : m - n);' 'band = longer;'
mutate no_rate_bound $R 'if (!n || !m || n > 2 * m || m > 2 * n) return -1;' 'if (!n || !m) return -1;'
mutate reversed      $R 'const float* y = b + static_cast<std::size_t>(j - 1) * kDimensions;' 'const float* y = b + static_cast<std::size_t>(m - j) * kDimensions;'
mutate no_emphasis   $F 'value = (x - 0.97f * before) * window_[i];' 'value = x * window_[i];'
mutate no_window     $F 'value = (x - 0.97f * before) * window_[i];' 'value = (x - 0.97f * before);'
mutate wrong_filters $F 'energy[lower_[k]] += weight_[k] * power; energy[lower_[k] + 1] += (1.0f - weight_[k]) * power;' 'energy[lower_[k]] += power;'
# Connected words (sentences) and the rules around them.
mutate no_partial    $R 'if (!Phrase(alone, spoken_, sizeof(spoken_))) result.match = AcousticMatch::Partial;' ''
mutate no_unit_number $R 'return parse("rewind ", " seconds").status == IntentStatus::Ok && parse("rewind ", "").status != IntentStatus::Ok;' 'return false;'
mutate no_amount_rule $R 'if (loses) return 0;' ''
mutate no_word_limit $R 'if (set_.word_limit <= 0 || words[i].cost <= set_.word_limit * reference)' 'if (true)'
mutate running_alone $R 'if (t.running) continue;' ''
mutate whole_norm    $R 'constexpr std::uint32_t kNormalizationWindow = 35;' 'constexpr std::uint32_t kNormalizationWindow = 0;'
mutate no_sentences  $R 'set_.garbage != 0 && frames >= set_.sentence_frames' 'false'
echo "survivors: $survivors"
exit $survivors
