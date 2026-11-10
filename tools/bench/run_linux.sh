#!/usr/bin/env bash
# Linux side of the head-to-head (docs/benchmarks.md): AVION's harness (GCC -O2), ffmpeg and OpenCV on the same inputs, timed
# without the counter, then counted with tools/bench/malloc_count.so preloaded into each. One JSON object per line on stdout.
# Needs: g++, ffmpeg, libopencv-dev (apt). Inputs are copied to the WSL disk first (/mnt is a 9P share: slow for every reader).
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=build/bench/linux
WORK=/tmp/avion_bench
mkdir -p "$OUT" "$WORK"
FILES=(assets/test_fixtures/costa_rica_4k60_hdr.mp4 assets/test_fixtures/lg_black_4k60_hdr.mp4)
RAW=build/bench/costa_640x480_gray.raw
for f in "${FILES[@]}" "$RAW"; do [ -f "$WORK/$(basename "$f")" ] || cp "$f" "$WORK/"; done
PICTURES=${PICTURES:-3000}

gcc -O2 -shared -fPIC tools/bench/malloc_count.c -o "$WORK/malloc_count.so"
g++ -std=c++20 -O2 -ffp-contract=off -pthread -Iinclude tools/bench_headtohead.cpp src/linear_memory_arena.cpp src/platform/posix/SystemMemory.cpp \
    src/demux/IsobmffParser.cpp src/platform/FileBlockSource.cpp src/codec/Rbsp.cpp src/codec/H264SyntaxParser.cpp src/codec/HevcSyntaxParser.cpp \
    src/codec/BitstreamPerception.cpp src/video/Perception.cpp src/video/HorizonLevel.cpp -ldl -o "$WORK/bench_headtohead"
g++ -std=c++17 -O2 tools/bench/opencv_baseline.cpp $(pkg-config --cflags --libs opencv4) -ldl -o "$WORK/opencv_baseline"
COUNT="$WORK/malloc_count.so"

printf '{"bench":"machine","cpu":"%s","cores":%s,"kernel":"%s","gcc":"%s","ffmpeg":"%s","opencv":"%s"}\n' \
  "$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //')" "$(nproc)" "$(uname -r)" "$(gcc -dumpfullversion)" \
  "$(ffmpeg -hide_banner -version | head -1 | cut -d' ' -f3)" "$(pkg-config --modversion opencv4)"

if [ -z "${ONLY_FFMPEG:-}" ]; then
for f in "${FILES[@]}"; do
  in="$WORK/$(basename "$f")"
  "$WORK/bench_headtohead" scan "$in" "$PICTURES" 2>/dev/null | sed "s/^{/{\"file\":\"$(basename "$f")\",\"counted\":false,/"
  LD_PRELOAD="$COUNT" "$WORK/bench_headtohead" scan "$in" "$PICTURES" 2>/dev/null | sed "s/^{/{\"file\":\"$(basename "$f")\",\"counted\":true,/"
done
raw="$WORK/$(basename "$RAW")"
"$WORK/bench_headtohead" flow "$raw" 640 480 600
LD_PRELOAD="$COUNT" "$WORK/bench_headtohead" flow "$raw" 640 480 600 | sed 's/^{/{"counted":true,/'
"$WORK/bench_headtohead" survey "$raw" 640 480 600
LD_PRELOAD="$COUNT" "$WORK/bench_headtohead" survey "$raw" 640 480 600 | sed 's/^{/{"counted":true,/'
for t in 1 0; do
  "$WORK/opencv_baseline" flow "$raw" 640 480 600 "$t"
  LD_PRELOAD="$COUNT" "$WORK/opencv_baseline" flow "$raw" 640 480 600 "$t" | sed 's/^{/{"counted":true,/'
  "$WORK/opencv_baseline" survey "$raw" 640 480 600 "$t"
  LD_PRELOAD="$COUNT" "$WORK/opencv_baseline" survey "$raw" 640 480 600 "$t" | sed 's/^{/{"counted":true,/'
done
fi

# ffmpeg: full decode through scdet, its scene-change detector, on every picture (the pixel-domain way to find cuts; scdet passes
# every picture on, so -frames:v bounds the pictures decoded). Its steady-state allocations are the difference between a 1,000-
# and a 3,000-picture run, so start-up and teardown cancel out.
ffscan() { # file pictures threads preload
  local log="$WORK/ffmpeg.log"
  if [ "$4" = 1 ]; then env MALLOC_COUNT_REPORT=1 LD_PRELOAD="$COUNT" ffmpeg -nostdin -hide_banner -benchmark -threads "$3" -i "$1" -frames:v "$2" -an \
      -vf scdet -f null - 2>"$log" || true
  else ffmpeg -nostdin -hide_banner -benchmark -threads "$3" -i "$1" -frames:v "$2" -an \
      -vf scdet -f null - 2>"$log" || true; fi
  local bench; bench=$(grep -o 'utime=[0-9.]*s stime=[0-9.]*s rtime=[0-9.]*s' "$log" | tail -1 | tr -d 's' | sed 's/utime=//;s/ time=/ /;s/ rtime=/ /')
  local frames; frames=$(grep -o 'frame= *[0-9]*' "$log" | tail -1 | grep -o '[0-9]*')
  local count; count=$(grep -o 'malloc_count total=[0-9]*' "$log" | grep -o '[0-9]*$' || echo -1)
  local cuts; cuts=$(grep -c 'lavfi.scd.time' "$log" || true)
  local times; times=$(grep -o 'lavfi.scd.time: [0-9.]*' "$log" | grep -o '[0-9.]*$' | paste -sd, - || true)
  read -r ut st rt <<<"$bench"
  printf '{"bench":"ffmpeg_scan","file":"%s","threads":%s,"counted":%s,"pictures":%s,"utime_s":%s,"stime_s":%s,"rtime_s":%s,"allocations_total":%s,"cuts":%s,"cut_times":[%s]}\n' \
    "$(basename "$1")" "$3" "$([ "$4" = 1 ] && echo true || echo false)" "${frames:-0}" "${ut:-0}" "${st:-0}" "${rt:-0}" "${count:--1}" "$cuts" "$times"
}
for f in "${FILES[@]}"; do
  in="$WORK/$(basename "$f")"
  ffscan "$in" "$PICTURES" 0 0
  ffscan "$in" 600 1 0
  ffscan "$in" 1000 0 1
  ffscan "$in" "$PICTURES" 0 1
done
