#!/bin/bash
# End-to-end test of the host stack against the simulated board.
# usage: test/run_sim_test.sh <build dir> <dir with libaio-iob*.dll> [seconds]
set -e
BUILD=$(realpath "$1")
DLLS=$(realpath "$2")
SECS=${3:-8}
HERE=$(dirname "$(realpath "$0")")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$BUILD/bi2x-modextract" "$DLLS" "$WORK"

python3 "$HERE/sim_bi2x.py" "$WORK/bi2x_tdj.bin" "$WORK/bi2x_sci.bin" \
    --seconds "$SECS" > "$WORK/sim.out" &
SIM=$!
for i in $(seq 50); do [ -s "$WORK/sim.out" ] && break; sleep 0.1; done

set +e
BI2X_PORT=$(head -1 "$WORK/sim.out") "$BUILD/test_host" "$WORK/"
HOST_RC=$?
wait $SIM
set -e

REPORT=$(tail -1 "$WORK/sim.out")
echo "$REPORT" | python3 -m json.tool
python3 - "$REPORT" <<'PY'
import json, sys
r = json.loads(sys.argv[1])
ok = (r['error_count'] == 0 and r['loaded'] == ['tdj', 'sci']
      and r['outputs'] == '0012007c1f057c00000030000000001f00'
      and r['resets'] == [1] and r['gamma_ok'] and r['tape_written'] == 707
      and r['tape_sample']['18'] == 0x4800 and r['tape_sample']['19'] == 0x4BE0
      and r['tape_sample']['37'] == 0x03E0 and r['commits'] >= 1
      and r['host_encrypted_frames'] > 0)
print('SIM CHECKS PASS' if ok else 'SIM CHECKS FAIL')
sys.exit(0 if ok else 1)
PY
SIM_RC=$?
exit $(( HOST_RC | SIM_RC ))
