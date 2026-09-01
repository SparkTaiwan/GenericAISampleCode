# GenericAI.TestServer

ZMQ frame-plane smoke-test for **GenericAI_ZMQ**. Plays the recorder /
AIServiceModule side so you can exercise the ZMQ + NAL path without ARGO.

1. HTTP `POST /SetParameters` to the AI (full-frame 4-corner ROI so detections
   aren't filtered; supplies threshold/sensitivity).
2. **BIND** a ZMQ `PUSH` socket; `GenericAI.exe` **CONNECTs** a `PULL` to it.
3. Frame source:
   - `rtsp=<url>` -> spawns **ffmpeg** to pull RTSP, streams Annex-B NAL, stamps
     each frame with the **real Windows FILETIME** (UTC) at send time.
   - `file=<clip.h264>` -> reads a raw Annex-B file (debug fallback, synthetic ts).
   Each access unit is PUSHed as `[ZmqFrameHeader | NAL]`, tagged with `channel_id`.
   SPS/PPS are cached and prepended to keyframes so a mid-stream join still decodes.
4. Best-effort HTTP listener for `PostAnalyticsResult` -> prints `[RESULT] channel=.. timestamp=..`.

No NuGet: ZMQ is P/Invoked into the wrapper's libzmq (`libzmq-v143-mt-4_3_5.dll`).
RTSP mode needs **ffmpeg.exe** on PATH (or pass `ffmpeg=<path>`).

## Build / run
- Build x64 (libzmq is x64). Run the exe from the wrapper output `bin\Release\x64\`
  (so `libzmq-*.dll` + `libsodium.dll` are reachable).

1. Start the AI in ZMQ mode:
   ```
   GenericAI.exe port=51000 channel_count=1 mode=single detector=objectdetection frame_endpoint=tcp://127.0.0.1:5556
   ```
2a. RTSP:
   ```
   GenericAI.TestServer.exe rtsp=rtsp://user:pass@cam/stream width=1920 height=1080 ai_port=51000 channel=51000
   ```
2b. File (fallback):
   ```
   GenericAI.TestServer.exe file=clip.h264 width=1920 height=1080 ai_port=51000 channel=51000
   ```

`channel` must equal the AI channel's port (`ai_port + index`); channel 0 = `ai_port`.
`width`/`height` must be >= the real stream resolution (the AI pool is sized from them).

### Results over ZMQ (instead of HTTP POST)
By default results come back as an HTTP POST to the built-in listener. To use the
ZMQ result plane instead, bind a PULL here and point the AI's PUSH at it:

```
GenericAI.exe ... frame_endpoint=tcp://127.0.0.1:5556 result_endpoint=tcp://127.0.0.1:5557
GenericAI.TestServer.exe rtsp=... width=.. height=.. result_zmq=tcp://*:5557
```

Connection model: module/test server BINDs PULL, the AI CONNECTs PUSH (auto-reconnect).
The result payload is the same JSON; `[RESULT] channel=.. timestamp=..` is printed either way.

## Success
- AI console: `Received SetParameters request`, `[zmq] ch=.. received=N decoded=M`, `[DETECT] ..`.
- Test server: `SetParameters -> 200 OK`, `sent N frames`, `[RESULT] channel=.. timestamp=..`.
- The `[RESULT] timestamp` equals the `ts=` the frame was sent with (round-trip).

## Notes
- H.264 only (header codec=1). For HEVC use `hevc_mp4toannexb` and codec=2 (code change).
- `transport=udp` if the camera/network prefers UDP RTP.

## Args
`rtsp=` | `file=` , `width=` `height=` `ai_host=` `ai_port=` `channel=` `zmq=`
`transport=tcp|udp` `ffmpeg=` `fps=` `loop=true|false` `result_port=`
