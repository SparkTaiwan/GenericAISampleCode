# ARGO Generic AI Integration Guide — Version 1.3

This guide describes how an external AI/analytics wrapper integrates with **Spark Recorder / ARGO**. It is the 1.3 successor to the 1.2 sample-code guide, updated for the current `GenericAI_ZMQ` wrapper.

**What changed since 1.2**

- **ZMQ transport** added as the default production path (works local *or* remote). The legacy Shared-Memory + HTTP path is still supported for local, backward-compatible setups.
- **One process serves N channels** (single mode), multiplexed by `channel_id`, instead of one process per channel.
- **Self-describing settings schema** (`GET /GetSettingsSchema`) with a per-field **scope** (`device` / `channel` / `roi`) and an **object-detection** field set (confidence / classes / object size) in addition to Motion (sensitivity / threshold).
- **Richer result events**: per-class counts (`items`) alongside the keyframe and ROI hits.
- **Health reporting** on `/Alive` (`{"status","version"}`) so a degraded wrapper (e.g. missing model) is surfaced instead of being restart-looped.
- The reference wrapper is `GenericAI.exe` (the 1.2 `SampleWrapper.exe`).

> The version string on the wire (`"version": "1.3"`) selects the protocol level. A 1.3 wrapper still accepts 1.2 payloads.

---

## 1. Architecture and data flow

The recorder (ARGO side, the `AStreamPerimeter_GenericAI` AI-service module) and the wrapper (`GenericAI.exe`) are connected by **three independent planes**:

```text
Spark Recorder / AStreamPerimeter_GenericAI         GenericAI.exe (wrapper)
+-------------------------------+                   +-------------------------------+
| HTTP client  (control plane)  | ---(1) request--> | HTTP listeners (per channel)  |
|  GET  /Alive                  |                   |  /Alive  (health + version)   |
|  GET  /GetSettingsSchema      | <--(1a) reply---- |  /GetSettingsSchema           |
|  POST /SetParameters          |                   |  /SetParameters               |
|  GET  /GetLicense             |                   |  /GetLicense                  |
+-------------------------------+                   +-------------------------------+

+-------------------------------+                   +-------------------------------+
| Frame plane (source)          | ==(2) frames====> | Frame plane (sink)            |
|  ZMQ PUSH  (encoded H.264/5)  |                   |  ZMQ PULL -> NAL decode       |
|   -- or legacy --             |                   |   -- or legacy --             |
|  MMF writer (raw I420)        |                   |  MMF reader                   |
+-------------------------------+                   +-------------------------------+

+-------------------------------+                   +-------------------------------+
| Result plane (sink)           | <=(3) results==== | Result plane (source)         |
|  ZMQ PULL  (analytics JSON)   |                   |  ZMQ PUSH                     |
|   -- or legacy --             |                   |   -- or legacy --             |
|  HTTP /PostAnalyticsResult    |                   |  HTTP POST                    |
+-------------------------------+                   +-------------------------------+
```

1. **Control plane (HTTP/JSON).** The recorder probes `/Alive`, fetches the settings UI schema with `/GetSettingsSchema`, and pushes per-channel configuration with `POST /SetParameters`. The control listeners always live on the wrapper's HTTP control port (`port=`), regardless of the frame/result transport.
2. **Frame plane.** ZMQ (encoded H.264/H.265 decoded by the wrapper — works remotely) or legacy MMF shared memory (raw I420, local only).
3. **Result plane.** ZMQ (analytics JSON PUSHed back) or legacy HTTP POST to `analytics_event_api_url`.

**Components**

| Layer | Implementation | Output | Responsibility |
| --- | --- | --- | --- |
| Host | C# (.NET Framework 4.8) | `GenericAI.exe` | HTTP control listeners, ZMQ result sender / MMF readers, JPEG encode + send, native interop. |
| Detector | C++ (MSVC v140) | `GenericAI.Native.dll` | Detectors (Motion, YOLOX person), per-channel pipeline, ZMQ frame receiver + NAL decode. Exports the `GAI_*` C ABI. |

---

## 2. AI Integration Specifications

### 2.1 Communication APIs

All control endpoints are HTTP on the wrapper's control port. `{port}` is the wrapper's `exe_server_port` (channel 0); each additional channel listens on `exe_server_port + index`.

| # | Direction | Method + URL | Body | Purpose |
| --- | --- | --- | --- | --- |
| 2.1.1 | Recorder → wrapper | `POST http://{host}:{port}/SetParameters` | JSON (see §3) | Configure a channel (resolution, ROIs, AI settings). Returns `200`. |
| 2.1.2 | Recorder → wrapper | `GET http://{host}:{port}/Alive` | — | Health + protocol version. Returns `200` with `{"status":"ok","version":"1.3"}` (or `"status":"error"` + `"message"` when degraded). |
| 2.1.3 | Recorder → wrapper | `GET http://{host}:{port}/GetSettingsSchema` | — | Returns the settings schema JSON (§3.2) the ConfigClient renders into the AI-settings UI. |
| 2.1.4 | Recorder → wrapper | `GET http://{host}:{port}/GetLicense` | — | License validation/request. Returns `200`. |
| 2.1.5 | wrapper → Recorder | Result event | JSON (§4.5) | Analytics result. **ZMQ**: PUSHed over the result plane (default). **Legacy**: `POST {analytics_event_api_url}` (`/PostAnalyticsResult`). |

> `host` is `127.0.0.1` for a local wrapper. For a remote wrapper the control listeners must bind a reachable interface (`http_host=`) and the recorder dials that IP.

### 2.2 Transport selection

The wrapper chooses each plane's transport **purely from launch arguments** (§7):

- `frame_endpoint` set → frames over **ZMQ**; unset → **MMF** shared memory.
- `result_endpoint` set → results over **ZMQ**; unset → **HTTP** `POST /PostAnalyticsResult`.

The planes are independent. Production uses ZMQ for both; the legacy local path uses MMF + HTTP.

---

## 3. Suggested Detailed Settings

### 3.1 `SetParameters` request (v1.3)

Up to **10 ROIs**, each with up to **10 points**. Object-detection tuning is carried **per ROI** (`rois[i].ai_settings`) and/or **per channel** (top-level `ai_settings`); a ROI value overrides the channel value.

```json
{
  "version": "1.3",
  "analytics_event_api_url": "http://127.0.0.1:9901/PostAnalyticsResult",
  "image_width": 1280,
  "image_height": 720,
  "draw_roi": true,
  "ai_settings": {
    "jpg_compress": 30,
    "trigger_interval": 1,
    "confidence": 0.70,
    "classes": ["person", "car"],
    "object_size_min": 0.0,
    "object_size_max": 100.0
  },
  "rois": [
    {
      "sensitivity": 50,
      "threshold": 50,
      "ai_settings": {
        "confidence": 0.80,
        "classes": ["person"],
        "object_size_min": 0.5,
        "object_size_max": 60.0
      },
      "rects": [
        {"x": 100, "y": 100},
        {"x": 200, "y": 100},
        {"x": 200, "y": 200},
        {"x": 100, "y": 200}
      ]
    }
  ]
}
```

Notes:

- `analytics_event_api_url` is only used in the **legacy HTTP** result path; in ZMQ result mode it is ignored.
- In v1.3 `jpg_compress` and `trigger_interval` come from `ai_settings` (channel scope). The top-level `jpg_compress` is still accepted for v1.2 compatibility.
- Motion uses `sensitivity` / `threshold` (per ROI). Object detection uses `confidence` / `classes` / `object_size_*`. A detector ignores fields it does not use.

### 3.2 `GetSettingsSchema` response

`GET /GetSettingsSchema` returns the field schema the ConfigClient renders; the user's values return verbatim inside `SetParameters` (channel fields in the top-level `ai_settings`, ROI fields in `rois[i].ai_settings`). Each field declares a **scope** (§4.2). Shape (Motion example):

```json
{
  "schema_version": "1.0",
  "default_locale": "zh-TW",
  "fields": [
    { "key": "jpg_compress", "type": "int", "scope": "channel",
      "label": {"zh-TW": "JPEG 壓縮品質", "en": "JPEG Quality"},
      "default": 30, "value": 30, "min": 1, "max": 100 },
    { "key": "sensitivity", "type": "int", "scope": "roi",
      "label": {"zh-TW": "靈敏度", "en": "Sensitivity"},
      "default": 50, "value": 50, "min": 1, "max": 100 },
    { "key": "threshold", "type": "int", "scope": "roi",
      "label": {"zh-TW": "門檻值", "en": "Threshold"},
      "default": 25, "value": 25, "min": 1, "max": 100 },
    { "key": "trigger_interval", "type": "int", "scope": "channel",
      "label": {"zh-TW": "觸發間隔（秒）", "en": "Trigger Interval (sec)"},
      "default": 1, "value": 1, "min": 0, "max": 3600 }
  ]
}
```

The object-detection schema replaces `sensitivity`/`threshold` with `confidence` (float, `roi`), `classes` (string_array, `roi`; countable class set), and `object_size_min` / `object_size_max` (float % of frame, `roi`). `label` / `description` / `option.label` are always locale maps (`{ "zh-TW": …, "en": … }`).

### 3.3 Schema field definitions

Each `fields[]` entry is one field object. **The ConfigClient renders a *different UI control depending on the field's `type`*** — this is the most important thing to get right when you design your own schema, because the chosen `type` is what the operator actually sees and edits.

**Field properties**

| Property | Required | Applies to | Description |
| --- | --- | --- | --- |
| `key` | ✔ | all | Field key. Echoed back as the `ai_settings` key (the AI reads only `key` + `value`; the schema metadata is not stored). |
| `type` | ✔ | all | Selects the UI control — see the next table. Absent → `string`. |
| `scope` | | all | `device` / `channel` / `roi` (§4.2). Absent → `channel`. |
| `label` | ✔ | all | Display name. Plain string or locale map. |
| `description` | | all | Shown as a tooltip on the control. |
| `unit` | | numeric | Unit string appended after the value in read-only views. |
| `required` | | all | Whether a value must be provided. |
| `default` / `value` | ✔ | all | Built-in default / current value (on serve `value == default`; the recorder overwrites `value` when feeding settings back). |
| `min` / `max` / `step` | | numeric | Slider bounds and tick granularity. |
| `options` | ✔ | `enum`, `string_array` | Choice list; each entry `{ "value": …, "label": … }` (label may be a locale map). |
| `counting` | | `string_array` | Marks the field's options as the countable item set (their per-item counts appear in the result `items`, §4.5). |

**`type` → UI control**

| `type` | UI control in ARGO Config | Value type on the wire | Notes |
| --- | --- | --- | --- |
| `int` / `float` | **Slider** | number | `min`/`max` bound it; `step` is the tick. `int` snaps to 1, `float` to `step`. |
| `int_range` / `float_range` | **two inputs (low / high)** | `[low, high]` | A range (two values). |
| `bool` | **Checkbox (on / off)** | boolean | |
| `string` | **Text box** | string | Free text. |
| `enum` | **Combo box (single select)** | string | Exactly one `value` from `options`. |
| `string_array` | **Checkbox list (multi-select)** | string[] | Any subset of `options`. With `counting`, the options are the countable set (e.g. detection classes). |

**Value round-trip.** After the operator edits a field, the ConfigClient emits a flat `{ key: value }` per field (no schema metadata) — channel-scoped fields into the top-level `ai_settings`, ROI-scoped fields into `rois[i].ai_settings`. Value types follow the table above.

**Worked mapping (current built-in schema):** `sensitivity` / `threshold` / `jpg_compress` / `trigger_interval` (`int`) → Slider; `confidence` / `object_size_min` / `object_size_max` (`float`) → Slider; `classes` (`string_array` + `options` + `counting`) → checkbox list.

---

## 4. Parameter Description

### 4.1 Top-level parameters (`SetParameters`)

| Name | Type | Example | Description |
| --- | --- | --- | --- |
| `version` | string | `"1.3"` | Protocol version. Selects 1.3 vs 1.2 behaviour. |
| `analytics_event_api_url` | string (URL) | `http://127.0.0.1:9901/PostAnalyticsResult` | HTTP endpoint for results (legacy result path only). |
| `image_width` / `image_height` | int | `1280` / `720` | Frame resolution in pixels. |
| `draw_roi` | bool | `true` | Draw ROI outlines on the returned keyframe JPEG. |
| `jpg_compress` | int (1–100) | `30` | Keyframe JPEG quality (higher = better/larger). v1.3 sources it from `ai_settings`; top-level accepted for v1.2. |
| `ai_settings` | object | see §3.1 | Channel-scoped settings (the schema's `scope:"channel"` fields, each carrying its `value`). |
| `rois` | array | see §4.3 | ROI list (max 10). |

### 4.2 Scopes

Every schema field declares where it is edited and how it flows on the wire:

| scope | one value per… | edited on | carried in |
| --- | --- | --- | --- |
| `device` | whole device | device page | top-level `ai_settings` (device-wide) |
| `channel` | smart stream | stream page | top-level `SetParameters.ai_settings` |
| `roi` | detection region | ROI editor | `SetParameters.rois[i].ai_settings` |

Rule of thumb: "what counts as a hit" (detection tuning) → `roi`; "how we output / whole stream" → `channel`; "one setting for the entire device" → `device`.

### 4.3 `rois` array

Each ROI carries detection parameters plus its polygon points.

| Name | Type | Example | Description |
| --- | --- | --- | --- |
| `sensitivity` | int (0–100) | `50` | **Motion**: detection sensitivity (higher = more sensitive). |
| `threshold` | int (0–100) | `50` | **Motion**: change threshold (lower = triggers more easily). |
| `ai_settings` | object | see §3.1 | **Object detection** per-ROI tuning: `confidence` (0–1), `classes` (subset of the supported set), `object_size_min`/`object_size_max` (% of frame area, 0–100). Absent → inherit the channel value. |
| `rects` | array | 4 points | ROI polygon (§4.4). |

Supported detection classes (fixed order; `classes` values and the result `items` names use these): `person`, `car`, `bus`, `truck`, `motorcycle`, `bicycle`, `cat`, `dog`.

### 4.4 `rects` parameter

A polygon of up to 10 `{x, y}` points; the sample and recorder use 4 corners. Recommended, consistent order:

**Top-left → Top-right → Bottom-right → Bottom-left**

```json
"rects": [
  {"x": 100, "y": 100},
  {"x": 200, "y": 100},
  {"x": 200, "y": 200},
  {"x": 100, "y": 200}
]
```

`x` / `y` are integer pixel coordinates from the top-left of the image. Inconsistent ordering can produce a mis-shaped polygon.

### 4.5 Recognition event result

Sent over the ZMQ result plane (or HTTP POST in legacy mode) whenever a channel produces a detection.

```json
{
  "version": "1.3",
  "port_num": 51000,
  "keyframe": "/9j/4AAQSkZJR...",
  "timestamp": 15003215760000,
  "rois_rects": [
    [ {"x":0,"y":0}, {"x":10,"y":0}, {"x":10,"y":10}, {"x":0,"y":10} ]
  ],
  "items": [
    { "name": "person", "value": "3" },
    { "name": "car",    "value": "1" }
  ]
}
```

| Name | Type | Description |
| --- | --- | --- |
| `version` | string | Result format version. |
| `port_num` | int | Channel identifier = the wrapper channel's port (`exe_server_port + index`). The recorder routes the result by this. |
| `keyframe` | string (Base64) | JPEG keyframe, Base64-encoded. Optionally drawn with ROI outlines (`draw_roi`). |
| `timestamp` | int | Frame time, Windows FILETIME (100 ns ticks, UTC). |
| `rois_rects` | array of arrays | The ROI polygons that fired, each an array of `{x,y}` points. |
| `items` | array | Per-class detection counts as `{ "name": "<class>", "value": "<count>" }`. The `name` matches a `classes` option. Omitted when empty. |

---

## 5. Shared Memory Settings (legacy / local)

Used when **no** `frame_endpoint` is passed. The recorder writes raw I420 frames into a per-channel named shared-memory segment and the wrapper reads them. Windows shared memory; max resolution 1920×1080.

```cpp
constexpr std::int64_t kMmfHeader = 0x1234;
constexpr std::int64_t kMmfFooter = 0x4321;
constexpr std::size_t  kMmfFrameCapacity = 1920ull * 1080ull * 3ull;

struct MmfData {
    std::int64_t  header;        // kMmfHeader
    int           image_status;  // 0 = idle, 1 = new frame, 2 = consumed (detection got frame)
    int           image_width;
    int           image_height;
    int           image_size;    // frame byte count
    std::uint64_t timestamp;     // Windows FILETIME (100 ns ticks, UTC)
    unsigned char image_data[kMmfFrameCapacity];  // raw I420
    std::int64_t  footer;        // kMmfFooter
};
```

- **Segment name:** `ChannelFrame_<port>`, where `<port>` is the channel's control port (`exe_server_port + index`).
- **`image_status`:** `0` = no data, `1` = new frame ready for the wrapper, `2` = wrapper has consumed it.
- The wrapper validates `header`/`footer`, waits for `image_status == 1`, reads the I420 payload (size derived from `image_width`/`image_height`), runs detection, and marks the frame consumed.

---

## 6. ZMQ Settings

The default production transport. Both planes are ZMQ TCP sockets. **The recorder BINDs; the wrapper CONNECTs**, so the wrapper can be (re)started independently and ZMQ reconnects automatically.

| Plane | Recorder side (bind) | Wrapper side (connect) | Payload |
| --- | --- | --- | --- |
| Frame | `PUSH` on `tcp://*:<ai_stream_port>` | `PULL` ← `frame_endpoint` | Encoded H.264/H.265 access units |
| Result | `PULL` on `tcp://*:<http_server_port>` | `PUSH` → `result_endpoint` | Analytics-result JSON (§4.5) |

> The result plane reuses the **`http_server_port`** value from the recorder config as its port number; in ZMQ mode a ZMQ `PULL` socket is bound there — it is **not** an HTTP server. Frame and result ports are independent (`ai_stream_port` ≠ `http_server_port`), and both differ from the HTTP control port (`exe_server_port`).

**Frame message format.** Each frame is a **2-part** ZMQ message; `channel_id` multiplexes all channels over the one socket (the receiver routes to the channel whose port equals `channel_id = exe_server_port + index`):

- **Part 0** — `ZmqFrameHeader` (packed):

```cpp
#pragma pack(push, 1)
struct ZmqFrameHeader {
    uint32_t magic;        // 0x5A4D4600  ('Z','M','F',0)
    uint16_t version;      // 1
    uint8_t  codec;        // 1 = H264, 2 = H265
    uint8_t  is_keyframe;  // 1 = IDR/keyframe, 0 = P/B
    uint16_t width;        // coded width  (informational)
    uint16_t height;       // coded height (informational)
    uint32_t channel_id;   // routes to the channel (exe_server_port + index)
    uint64_t timestamp;    // Windows FILETIME (100 ns ticks, UTC)
    uint32_t payload_sz;   // NAL byte count in part 1
};
#pragma pack(pop)
```

- **Part 1** — the raw encoded NAL bytes (`payload_sz` bytes) for one coded picture. Keyframes should be self-decodable (SPS/PPS in-band or prepended).

**Result message format.** A single-part ZMQ message carrying the same result JSON as §4.5 (the `port_num` field identifies the channel).

**Socket tuning** (reference): frame `PUSH` uses a finite send timeout + bounded HWM so a stalled wrapper drops frames instead of wedging the encoder; result `PULL` uses a short receive timeout so the loop can observe shutdown. `LINGER=0` on close.

---

## 7. Execution and Naming Conventions

### 7.1 Launch arguments

```text
GenericAI.exe port=<int> [channel_count=<N>] [mode=single|multi] [detector=motion|objectdetection]
              [server_ip=<ip> result_port=<port> stream_port=<port>]
              [frame_endpoint=tcp://host:port] [result_endpoint=tcp://host:port]
              [http_host=<ip|+>] [encode_workers=<N>] [send_workers=<M>]
```

| Argument | Maps to (recorder) | Notes |
| --- | --- | --- |
| `port` | `exe_server_port` | HTTP control port (`/Alive`, `/SetParameters`) and channel-0 id. **Required in production.** |
| `channel_count` | channel count | Channels this process serves (`single` mode). Control listeners open on `port … port+N-1`. |
| `mode` | `process_mode` | `single` = one process, all channels (**required for ZMQ**); `multi` = one process per channel. |
| `detector` | detection type | `motion` or `objectdetection`. Omitted → the wrapper's compile-time default. |
| `frame_endpoint` | `tcp://<ip>:<ai_stream_port>` | ZMQ frame plane to PULL from. Unset → MMF frames. |
| `result_endpoint` | `tcp://<ip>:<http_server_port>` | ZMQ result plane to PUSH to. Unset → HTTP result POST. |
| `server_ip` + `result_port` + `stream_port` | recorder IP + `http_server_port` + `ai_stream_port` | Shorthand that expands to `result_endpoint`/`frame_endpoint`. Must be given together. |
| `http_host` | — | Control-listener bind address (default `127.0.0.1`; use the machine IP or `+` for a remote recorder). |

**ZMQ launch example** (one process, 8 channels, recorder on the same host):

```text
GenericAI.exe port=51000 channel_count=8 mode=single detector=motion ^
              server_ip=127.0.0.1 stream_port=61000 result_port=9903
```

**MMF launch example** (legacy, one process per channel):

```text
GenericAI.exe port=51007 mode=multi detector=motion
```

### 7.2 Naming and rules

1. The executable may be renamed; configure ARGO to call the correct filename. The **argument format is fixed**.
2. `port=` is mandatory and equals `exe_server_port`; the channel id is `exe_server_port + index`.
3. ZMQ requires `mode=single`; the recorder does not enable ZMQ for `mode=multi`.
4. The three ports are distinct: control (`exe_server_port`), frame (`ai_stream_port`), result (`http_server_port`). Pointing `result_port` at the stream port is a common mistake — frames flow but results never arrive.
5. The executable should be a **signed application** so Windows / antivirus does not block it.

---

## 8. License

The Generic AI Detection License serial number and channel count are provided by **Taiwan Spark Technology Co., Ltd.** Contact your representative to enable these features.
