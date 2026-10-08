#!/usr/bin/env python3
"""Recorder-side simulator for end-to-end tests of the Linux GenericAI wrapper.

Plays the part of spark.recorder's AStreamPerimeter_GenericAI / GenericAIDevice:

  zmq  BIND a PUSH (frame plane) and a PULL (result plane), POST /SetParameters,
       push H.264 access units with the 28-byte ZmqFrameHeader, collect results.
  mmf  Create /dev/shm/ChannelFrame_<port> exactly like the Linux recorder's
       mmf.cpp (MMF_Data_Generic), write I420 frames, receive results over the
       HTTP analytics callback.

Every result is validated (protocol version, port_num, base64 JPEG keyframe,
timestamp echoed from a frame we sent, rois_rects shape); the first keyframe
per channel is saved to --out. Exit code 0 = at least --min-results valid
results per channel and no invalid ones.

Video comes from ffmpeg: the lavfi testsrc2 pattern (moving, so Motion fires)
or --video <file> (e.g. a clip with people for object detection).

Needs: python3-zmq, ffmpeg (with libx264).
"""

import argparse
import base64
import http.server
import json
import mmap
import os
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

FRAME_MAGIC = 0x5A4D4600
FRAME_VERSION = 1
HEADER_FMT = "<IHBBHHIQI"  # packed, 28 bytes (zmq_frame_header.h)
assert struct.calcsize(HEADER_FMT) == 28

FILETIME_EPOCH_OFFSET = 116444736000000000  # 1601 -> 1970 in 100 ns


def filetime_now():
    return int(time.time() * 10_000_000) + FILETIME_EPOCH_OFFSET


def log(msg):
    print(f"[sim] {msg}", flush=True)


# ---- wrapper control plane ------------------------------------------------

def http_get(url, timeout=3):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.status, r.read().decode("utf-8")


def http_post_json(url, obj, timeout=5):
    data = json.dumps(obj).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read().decode("utf-8")


def wait_alive(port, timeout_s):
    deadline = time.time() + timeout_s
    last = None
    while time.time() < deadline:
        try:
            status, body = http_get(f"http://127.0.0.1:{port}/Alive")
            alive = json.loads(body)
            log(f"/Alive :{port} -> {status} {body}")
            return alive
        except Exception as ex:  # not up yet
            last = ex
            time.sleep(0.5)
    raise RuntimeError(f"wrapper /Alive on :{port} never answered ({last})")


def check_schema(port, detector):
    status, body = http_get(f"http://127.0.0.1:{port}/GetSettingsSchema")
    schema = json.loads(body)
    keys = [f["key"] for f in schema["fields"]]
    expected = (["jpg_compress", "sensitivity", "threshold", "trigger_interval"] if detector == "motion"
                else ["jpg_compress", "confidence", "classes", "object_size_min", "object_size_max",
                      "trigger_interval"])
    if keys != expected:
        raise RuntimeError(f"schema keys {keys} != {expected}")
    log(f"/GetSettingsSchema :{port} -> {status}, fields={keys}")


def set_parameters(port, width, height, url, detector):
    ai = {"jpg_compress": 60, "trigger_interval": 0}
    if detector != "motion":
        ai.update({"confidence": 0.4, "classes": ["person", "car", "bus", "truck"]})
    body = {
        "version": "1.3",
        "mode": "single",
        "channel_id": port,
        "analytics_event_api_url": url,
        "image_width": width,
        "image_height": height,
        "draw_roi": True,
        "ai_settings": ai,
        "rois": [{
            "sensitivity": 90,
            "threshold": 10,
            "rects": [{"x": 0, "y": 0}, {"x": width - 1, "y": 0},
                      {"x": width - 1, "y": height - 1}, {"x": 0, "y": height - 1}],
        }],
    }
    # Like the recorder: resend until 200 (the wrapper answers 503 while the
    # detector is still loading).
    deadline = time.time() + 120
    while True:
        try:
            status, resp = http_post_json(f"http://127.0.0.1:{port}/SetParameters", body)
            break
        except urllib.error.HTTPError as ex:
            if ex.code != 503 or time.time() > deadline:
                raise
            log(f"/SetParameters :{port} -> 503 (detector initializing), retrying")
            time.sleep(0.5)
    log(f"/SetParameters :{port} -> {status} {resp}")
    if status != 200:
        raise RuntimeError("SetParameters failed")

    # Negative check: a v1.2 body without jpg_compress must be a 400.
    bad = dict(body, version="1.2")
    try:
        http_post_json(f"http://127.0.0.1:{port}/SetParameters", bad)
        raise RuntimeError("v1.2 SetParameters without jpg_compress was accepted")
    except urllib.error.HTTPError as ex:
        if ex.code != 400:
            raise
        log(f"/SetParameters :{port} v1.2 without jpg_compress -> 400 (expected)")
    # Re-send the good one (the bad one never reached native, but be explicit).
    http_post_json(f"http://127.0.0.1:{port}/SetParameters", body)


# ---- video sources --------------------------------------------------------

def ffmpeg_input(args):
    if args.video:
        return ["-stream_loop", "-1", "-i", args.video]
    return ["-f", "lavfi", "-i", f"testsrc2=size={args.width}x{args.height}:rate={args.fps}"]


def h264_access_units(args):
    """Encodes the source to Annex-B H.264 with AUDs and splits it per access unit."""
    cmd = (["ffmpeg", "-loglevel", "error"] + ffmpeg_input(args) +
           ["-t", str(args.seconds), "-vf", f"scale={args.width}:{args.height},fps={args.fps}",
            "-c:v", "libx264", "-preset", "veryfast", "-tune", "zerolatency", "-bf", "0",
            "-g", str(args.fps), "-pix_fmt", "yuv420p",
            "-bsf:v", "h264_metadata=aud=insert", "-f", "h264", "-"])
    data = subprocess.run(cmd, check=True, stdout=subprocess.PIPE).stdout
    aud = b"\x00\x00\x00\x01\x09"
    starts = []
    i = data.find(aud)
    while i >= 0:
        starts.append(i)
        i = data.find(aud, i + 1)
    starts.append(len(data))
    aus = [data[starts[k]:starts[k + 1]] for k in range(len(starts) - 1)]
    log(f"encoded {len(aus)} H.264 access units ({len(data)} bytes)")
    return aus


def is_idr(au):
    i = 0
    while True:
        i = au.find(b"\x00\x00\x01", i)
        if i < 0 or i + 3 >= len(au):
            return False
        if au[i + 3] & 0x1F == 5:
            return True
        i += 3


def i420_frames(args):
    cmd = (["ffmpeg", "-loglevel", "error"] + ffmpeg_input(args) +
           ["-t", str(args.seconds), "-vf", f"scale={args.width}:{args.height},fps={args.fps}",
            "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"])
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    size = args.width * args.height * 3 // 2
    while True:
        buf = proc.stdout.read(size)
        if len(buf) < size:
            break
        yield buf
    proc.wait()


# ---- result validation ----------------------------------------------------

class Results:
    def __init__(self, ports, out_dir, sent_ts):
        self.lock = threading.Lock()
        self.valid = {p: 0 for p in ports}
        self.invalid = []
        self.out_dir = out_dir
        self.sent_ts = sent_ts
        self.items = {}

    def handle(self, raw):
        try:
            env = json.loads(raw)
            port = env["port_num"]
            if env["version"] != "1.3":
                raise ValueError(f"version {env['version']}")
            if port not in self.valid:
                raise ValueError(f"unknown port_num {port}")
            jpeg = base64.b64decode(env["keyframe"])
            if not (jpeg[:2] == b"\xff\xd8" and jpeg[-2:] == b"\xff\xd9"):
                raise ValueError("keyframe is not a JPEG")
            if env["timestamp"] not in self.sent_ts:
                raise ValueError(f"timestamp {env['timestamp']} was never sent")
            rois = env["rois_rects"]
            if not rois or not all(isinstance(g, list) and g and {"x", "y"} <= set(g[0]) for g in rois):
                raise ValueError(f"bad rois_rects {rois!r:.120}")
            with self.lock:
                first = self.valid[port] == 0
                self.valid[port] += 1
                for it in env.get("items", []):
                    self.items[it["name"]] = self.items.get(it["name"], 0) + 1
            if first:
                path = os.path.join(self.out_dir, f"keyframe_{port}.jpg")
                with open(path, "wb") as f:
                    f.write(jpeg)
                log(f"first result ch={port}: {len(jpeg)} B JPEG -> {path}, "
                    f"rois={len(rois)}x{len(rois[0])}, items={env.get('items')}")
        except Exception as ex:
            with self.lock:
                self.invalid.append(str(ex))
            log(f"INVALID result: {ex}")


# ---- modes ----------------------------------------------------------------

def run_zmq(args):
    import zmq

    ports = [args.port + k for k in range(args.channels)]
    ctx = zmq.Context()
    push = ctx.socket(zmq.PUSH)
    push.setsockopt(zmq.SNDHWM, 200)
    push.bind(f"tcp://*:{args.stream_port}")
    pull = ctx.socket(zmq.PULL)
    pull.bind(f"tcp://*:{args.result_port}")
    pull.setsockopt(zmq.RCVTIMEO, 200)
    log(f"bound frame PUSH :{args.stream_port}, result PULL :{args.result_port}")

    for p in ports:
        alive = wait_alive(p, args.alive_timeout)
        if alive.get("status") != "ok":
            raise RuntimeError(f"wrapper degraded: {alive}")
        check_schema(p, args.detector)
        set_parameters(p, args.width, args.height, "", args.detector)

    aus = h264_access_units(args)
    sent_ts = set()
    results = Results(ports, args.out, sent_ts)
    stop = threading.Event()

    def receiver():
        while not stop.is_set():
            try:
                results.handle(pull.recv())
            except zmq.Again:
                continue

    t = threading.Thread(target=receiver, daemon=True)
    t.start()

    period = 1.0 / args.fps
    nxt = time.time()
    for au in aus:
        for p in ports:
            ts = filetime_now()
            sent_ts.add(ts)
            hdr = struct.pack(HEADER_FMT, FRAME_MAGIC, FRAME_VERSION, 1, 1 if is_idr(au) else 0,
                              args.width, args.height, p, ts, len(au))
            push.send_multipart([hdr, au])
        nxt += period
        time.sleep(max(0.0, nxt - time.time()))

    time.sleep(args.drain)
    stop.set()
    t.join()
    push.close(0)
    pull.close(0)
    ctx.term()
    return report(results, args.min_results)


def run_mmf(args):
    port = args.port
    results_holder = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            n = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(n)
            results_holder["r"].handle(body)
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def log_message(self, *a):
            pass

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", args.http_port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{args.http_port}/event"
    log(f"HTTP analytics callback listening on {url}")

    # Same object the Linux recorder creates: shm_open("/ChannelFrame_<port>")
    # sized to sizeof(MMF_Data_Generic) (its image_data is an array of
    # unsigned char*, hence the 8x).
    shm_path = f"/dev/shm/ChannelFrame_{port}"
    shm_size = 8 + 4 * 4 + 8 + 8 * (1920 * 1080 * 3) + 8
    fd = os.open(shm_path, os.O_CREAT | os.O_RDWR, 0o666)
    os.ftruncate(fd, shm_size)
    mm = mmap.mmap(fd, shm_size)
    mm[0:8] = struct.pack("<q", 0x1234)
    log(f"created {shm_path} ({shm_size} bytes)")

    alive = wait_alive(port, args.alive_timeout)
    if alive.get("status") != "ok":
        raise RuntimeError(f"wrapper degraded: {alive}")
    check_schema(port, args.detector)
    set_parameters(port, args.width, args.height, url, args.detector)

    sent_ts = set()
    results = Results([port], args.out, sent_ts)
    results_holder["r"] = results

    period = 1.0 / args.fps
    nxt = time.time()
    written = skipped = 0
    for frame in i420_frames(args):
        status = struct.unpack_from("<i", mm, 8)[0]
        if status in (0, 2):  # recorder only writes when the wrapper consumed the last one
            ts = filetime_now()
            sent_ts.add(ts)
            struct.pack_into("<iiiiQ", mm, 8, 0, args.width, args.height, len(frame), ts)
            mm[32:32 + len(frame)] = frame
            struct.pack_into("<i", mm, 8, 1)
            written += 1
        else:
            skipped += 1
        nxt += period
        time.sleep(max(0.0, nxt - time.time()))
    log(f"MMF frames written={written} skipped(wrapper busy)={skipped}")

    time.sleep(args.drain)
    srv.shutdown()
    mm.close()
    os.close(fd)
    os.unlink(shm_path)
    return report(results, args.min_results)


def report(results, min_results):
    log(f"valid results per channel: {results.valid}; invalid: {len(results.invalid)}; "
        f"items seen: {results.items}")
    ok = not results.invalid and all(v >= min_results for v in results.valid.values())
    log("PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["zmq", "mmf"])
    ap.add_argument("--port", type=int, default=46000, help="wrapper base HTTP control port")
    ap.add_argument("--channels", type=int, default=1, help="zmq mode: channel count")
    ap.add_argument("--stream-port", type=int, default=9906)
    ap.add_argument("--result-port", type=int, default=9905)
    ap.add_argument("--http-port", type=int, default=9910, help="mmf mode: analytics callback port")
    ap.add_argument("--detector", choices=["motion", "objectdetection"], default="motion")
    ap.add_argument("--video", help="input video instead of testsrc2")
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--height", type=int, default=360)
    ap.add_argument("--fps", type=int, default=15)
    ap.add_argument("--seconds", type=int, default=10)
    ap.add_argument("--drain", type=float, default=3.0, help="seconds to wait for late results")
    ap.add_argument("--alive-timeout", type=float, default=60.0)
    ap.add_argument("--min-results", type=int, default=1)
    ap.add_argument("--out", default="sim_out")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    try:
        sys.exit(run_zmq(args) if args.mode == "zmq" else run_mmf(args))
    except Exception as ex:
        log(f"ERROR: {ex}")
        sys.exit(2)


if __name__ == "__main__":
    main()
