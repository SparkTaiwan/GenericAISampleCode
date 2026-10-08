#!/usr/bin/env bash
# End-to-end tests for the Linux GenericAI wrapper against tests/simulator.py.
#
#   tests/run_e2e.sh <build dir>/GenericAI [video-with-people.mp4]
#
# Cases:
#   args        bad args -> exit 2; port already in use -> exit 3
#   zmq-motion  ZMQ frames + ZMQ results, 2 channels, motion detector
#   mmf-motion  /dev/shm MMF frames + HTTP POST results, motion detector
#   zmq-objdet  ZMQ, object detection (YOLOX); with a video argument it must
#               also recognise at least one class
#   shutdown    every run must exit 0 on SIGTERM within 15 s
set -u

APP_DIR="$(cd "${1:?usage: $0 <GenericAI output dir> [video]}" && pwd)"
VIDEO="${2:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(mktemp -d /tmp/gai_e2e.XXXXXX)"
SIM="python3 $HERE/simulator.py"
export GENERICAI_LOG_DIR="$OUT/logs"
PASS=0
FAIL=0

ok()   { echo "  PASS: $*"; PASS=$((PASS + 1)); }
bad()  { echo "  FAIL: $*"; FAIL=$((FAIL + 1)); }

start_wrapper() {  # args... ; sets WPID
    "$APP_DIR/GenericAI" "$@" > "$OUT/wrapper_$CASE.log" 2>&1 &
    WPID=$!
}

stop_wrapper() {  # checks graceful shutdown (exit 0 within 15 s)
    kill -TERM "$WPID" 2>/dev/null
    for _ in $(seq 1 150); do
        kill -0 "$WPID" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "$WPID" 2>/dev/null; then
        bad "$CASE: wrapper did not exit within 15 s of SIGTERM"
        kill -KILL "$WPID"
        wait "$WPID" 2>/dev/null
        return
    fi
    wait "$WPID"
    local rc=$?
    [ "$rc" -eq 0 ] && ok "$CASE: clean SIGTERM shutdown (exit 0)" || bad "$CASE: exit code $rc after SIGTERM"
}

echo "== logs and keyframes: $OUT"

# Turn on file logging for the run (and restore the shipped config afterwards).
cp "$APP_DIR/GenericAI.Config" "$OUT/GenericAI.Config.orig"
sed -i 's/^log_to_file = 0/log_to_file = 1/; s/^show_debug = 0/show_debug = 1/' "$APP_DIR/GenericAI.Config"
trap 'cp "$OUT/GenericAI.Config.orig" "$APP_DIR/GenericAI.Config"' EXIT

CASE=args
echo "== $CASE"
"$APP_DIR/GenericAI" port=abc > /dev/null 2>&1; rc=$?
[ "$rc" -eq 2 ] && ok "bad port -> exit 2" || bad "bad port -> exit $rc (want 2)"
"$APP_DIR/GenericAI" frobnicate=1 > /dev/null 2>&1; rc=$?
[ "$rc" -eq 2 ] && ok "unknown key -> exit 2" || bad "unknown key -> exit $rc (want 2)"
"$APP_DIR/GenericAI" server_ip=127.0.0.1 > /dev/null 2>&1; rc=$?
[ "$rc" -eq 2 ] && ok "incomplete server_ip shorthand -> exit 2" || bad "incomplete shorthand -> exit $rc (want 2)"
python3 -c 'import socket,time; s=socket.socket(); s.bind(("127.0.0.1",46500)); s.listen(); time.sleep(5)' &
BLOCKER=$!
sleep 0.5
"$APP_DIR/GenericAI" port=46500 > /dev/null 2>&1; rc=$?
[ "$rc" -eq 3 ] && ok "port in use -> exit 3" || bad "port in use -> exit $rc (want 3)"
kill $BLOCKER 2>/dev/null; wait $BLOCKER 2>/dev/null

CASE=zmq-motion
echo "== $CASE"
start_wrapper port=46000 mode=single channel_count=2 detector=motion \
    server_ip=127.0.0.1 result_port=9905 stream_port=9906
if $SIM zmq --port 46000 --channels 2 --detector motion --seconds 10 --out "$OUT/$CASE"; then
    ok "$CASE: results on every channel"
else
    bad "$CASE: simulator failed"
fi
stop_wrapper

CASE=mmf-motion
echo "== $CASE"
start_wrapper port=46100 detector=motion
if $SIM mmf --port 46100 --detector motion --seconds 10 --out "$OUT/$CASE"; then
    ok "$CASE: results over HTTP callback"
else
    bad "$CASE: simulator failed"
fi
stop_wrapper

CASE=zmq-objdet
echo "== $CASE"
start_wrapper port=46200 mode=single channel_count=1 detector=objectdetection \
    server_ip=127.0.0.1 result_port=9915 stream_port=9916
if [ -n "$VIDEO" ]; then
    $SIM zmq --port 46200 --stream-port 9916 --result-port 9915 --detector objectdetection \
        --video "$VIDEO" --seconds 15 --alive-timeout 120 --out "$OUT/$CASE"
    rc=$?
    [ "$rc" -eq 0 ] && ok "$CASE: objects recognised in $VIDEO" || bad "$CASE: no valid detections"
else
    # testsrc2 has no objects: only check the model loads and frames flow.
    $SIM zmq --port 46200 --stream-port 9916 --result-port 9915 --detector objectdetection \
        --seconds 8 --alive-timeout 120 --min-results 0 --out "$OUT/$CASE"
    rc=$?
    [ "$rc" -eq 0 ] && ok "$CASE: model loaded, pipeline ran" || bad "$CASE: simulator failed"
fi
grep -h "detector backend" "$GENERICAI_LOG_DIR"/GenericAI-46200.log 2>/dev/null | tail -1 | sed 's/^/  info: /'
stop_wrapper

echo
echo "== $PASS passed, $FAIL failed (logs: $OUT)"
[ "$FAIL" -eq 0 ]
