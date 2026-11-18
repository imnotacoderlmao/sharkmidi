#!/bin/sh
# run from the despair directory:  sh tools/bench_sweep.sh   (then ./bench_sweep [-r reps] [-c chunk_ticks] file.mid [more.mid ...])
# renderer.c is not changed: a copy with process_run_events renamed, so the variants can be swapped in, is put in tools/.bench
set -e
D=$(pwd)
mkdir -p tools/.bench
sed -e 's/static int64_t process_run_events(/static int64_t process_run_events_orig(/' \
    -e 's/= process_run_events(/= runfn(/' \
    -e "s#\"\\.\\./#\"$D/#" -e "s#\"renderer.h\"#\"$D/render/renderer.h\"#" -e "s#\"shaders.h\"#\"$D/render/shaders.h\"#" \
    render/renderer.c > tools/.bench/renderer_h.c
${CC:-gcc} -O3 -fopenmp -march=native -fno-math-errno -w -I. -Ithird_party/glad/include tools/bench_sweep.c parse/parser.c parse/midistorage.c \
    third_party/conmidi_digit_formatter.c third_party/glad/src/glad.c -o bench_sweep -lm -ldl
echo built ./bench_sweep
