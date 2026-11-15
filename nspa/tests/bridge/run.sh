#!/bin/sh
# The sound card bridge's tests that need no hardware: the resampler's (quality, continuity,
# accounting, delay) and the simulation of whole bridges (lock, delay error, drift steps,
# xruns, other rates). Hardware: hw-bridge.sh (one card), multi-bridge.sh (several), and
# aloop_test.cpp on snd-aloop (see the headers of each).
#   nspa/tests/bridge/run.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
root=$(cd "$root/.." && pwd)
out=$(mktemp -d /tmp/nspa-bridge-tests.XXXXXX)
trap 'rm -rf "$out"' EXIT
fail=0
g++ -std=gnu++11 -faligned-new -O2 -march=x86-64 -I"$root/common" "$root/common/JackBridgeResampler.cpp" \
    "$here/resampler_test.cpp" -o "$out/resampler_test" || exit 1
g++ -std=gnu++11 -faligned-new -O2 -march=x86-64 -I"$root/common" "$root/common/JackBridgeCore.cpp" \
    "$root/common/JackBridgeStream.cpp" "$root/common/JackBridgeResampler.cpp" "$here/sim_test.cpp" \
    -o "$out/sim_test" || exit 1
"$out/resampler_test" --quick --no-speed | tail -1 | grep -q " 0 failed" && echo "PASS bridge-resampler" || { echo "FAIL bridge-resampler"; fail=1; }
if "$out/sim_test" > "$out/sim.log"; then
    echo "PASS bridge-sim: $(grep -c '^' "$out/sim.log") lines, all scenarios"
else
    echo "FAIL bridge-sim"; cat "$out/sim.log"; fail=1
fi
exit $fail
