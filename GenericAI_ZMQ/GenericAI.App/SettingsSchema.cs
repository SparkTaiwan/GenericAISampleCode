using System.Collections.Generic;
using Newtonsoft.Json;

namespace GenericAI.App
{
    // Built-in payload for GET /GetSettingsSchema (spec §5). The ConfigClient GETs
    // this to render the dynamic AI-settings UI; the user's filled values come back
    // verbatim in SetParameters.ai_settings (same fields shape, each carrying value).
    //
    // Per spec §5.5 (i18n) the design decision is: label / description / option.label
    // are ALWAYS locale maps ({ "zh-TW": ..., "en": ... }); the built-in fills every
    // locale here so even the built-in AI is multi-language. ConfigClient resolves by
    // current UI locale, falling back to default_locale.
    //
    // Field carries both `default` (built-in default) and `value` (current). On serve
    // value == default; the recorder overwrites value when feeding settings back.
    //
    // NOTE: placeholder values for bring-up — the native detectors do not consume
    // these yet. Refine per detector (motion vs objectdetection) once wired.
    internal static class SettingsSchema
    {
        // Process-wide detector kind, set once at startup (Program.cs) so
        // /GetSettingsSchema serves the schema for the running detector: object
        // detection (confidence + classes) vs motion (sensitivity + threshold).
        private static int _detectorKind = (int)DetectorType.Person;

        public static void Configure(int detectorKind) { _detectorKind = detectorKind; }

        // Schema JSON for the running detector. Motion gets the motion schema;
        // everything else (Person / unset -1) gets object detection.
        public static string Json =>
            _detectorKind == (int)DetectorType.Motion ? MotionJson : ObjectDetectionJson;

        private static readonly string ObjectDetectionJson = BuildObjectDetection();
        private static readonly string MotionJson = BuildMotion();

        // Locale-map helper (spec §5.5): each entry is an explicit (locale, text)
        // pair so the locale is visible at the call site and serialises to
        // { "zh-TW": ..., "en": ... }. Add languages by adding pairs.
        private static Dictionary<string, string> Loc(params (string locale, string text)[] entries)
        {
            var map = new Dictionary<string, string>(entries.Length);
            foreach (var e in entries) map[e.locale] = e.text;
            return map;
        }

        // Object-detection schema: confidence + detectable class set. Scoped to align
        // with the dongle's current capability. Class options/order mirror
        // GenericAI.Native/class_table.h 1:1.
        private static string BuildObjectDetection()
        {
            var schema = new
            {
                schema_version = "1.0",
                default_locale = "zh-TW",
                fields = new object[]
                {
                    JpgCompressField(),
                    new {
                        key = "confidence", type = "float",
                        label       = Loc(("zh-TW", "信心門檻"), ("en", "Confidence")),
                        description = Loc(("zh-TW", "低於此分數的偵測結果會被濾掉"), ("en", "Detections below this score are dropped")),
                        @default = 0.70, value = 0.70, min = 0.10, max = 1.00, step = 0.05
                    },
                    new {
                        key = "classes", type = "string_array",
                        counting = true,   // this field's options are the countable item catalog (each can be sent as value__Count)
                        label = Loc(("zh-TW", "偵測類別"), ("en", "Classes")),
                        options = new object[]
                        {
                            new { value = "person",     label = Loc(("zh-TW", "人"),     ("en", "Person")) },
                            new { value = "car",        label = Loc(("zh-TW", "汽車"),   ("en", "Car")) },
                            new { value = "bus",        label = Loc(("zh-TW", "公車"),   ("en", "Bus")) },
                            new { value = "truck",      label = Loc(("zh-TW", "卡車"),   ("en", "Truck")) },
                            new { value = "motorcycle", label = Loc(("zh-TW", "機車"),   ("en", "Motorcycle")) },
                            new { value = "bicycle",    label = Loc(("zh-TW", "自行車"), ("en", "Bicycle")) },
                            new { value = "cat",        label = Loc(("zh-TW", "貓"),     ("en", "Cat")) },
                            new { value = "dog",        label = Loc(("zh-TW", "狗"),     ("en", "Dog")) },
                        },
                        @default = new[] { "person", "car" }, value = new[] { "person", "car" }
                    },
                    new {
                        key = "object_size_min", type = "float",
                        label       = Loc(("zh-TW", "最小物件大小（畫面佔比 %）"), ("en", "Min Object Size (% of frame)")),
                        description = Loc(("zh-TW", "只保留邊界框面積大於畫面此比例的物件，用來濾掉太小的偵測。0 表示不限制下限"), ("en", "Keep only objects whose bounding-box area exceeds this percentage of the frame; filters out tiny detections. 0 means no lower limit")),
                        @default = 0.0, value = 0.0, min = 0.0, max = 100.0, step = 0.5
                    },
                    new {
                        key = "object_size_max", type = "float",
                        label       = Loc(("zh-TW", "最大物件大小（畫面佔比 %）"), ("en", "Max Object Size (% of frame)")),
                        description = Loc(("zh-TW", "只保留邊界框面積小於畫面此比例的物件，用來濾掉太大的偵測。100 表示不限制上限"), ("en", "Keep only objects whose bounding-box area is below this percentage of the frame; filters out oversized detections. 100 means no upper limit")),
                        @default = 100.0, value = 100.0, min = 0.0, max = 100.0, step = 0.5
                    },
                    new {
                        key = "trigger_interval", type = "int",
                        label       = Loc(("zh-TW", "觸發間隔（秒）"), ("en", "Trigger Interval (sec)")),
                        description = Loc(("zh-TW", "送出 HTTP POST 的最小間隔秒數；送出後這段時間內的偵測都不再發送。0 表示不限制，有偵測就送"), ("en", "Minimum seconds between HTTP POSTs; detections within this window after a send are suppressed. 0 means no limit, send on every detection")),
                        @default = 1, value = 1, min = 0, max = 3600
                    },
                }
            };
            return JsonConvert.SerializeObject(schema);
        }

        // Motion schema: the motion detector's tunables (sensitivity + change
        // threshold), mirroring MotionDetector's defaults (sensitivity=50,
        // threshold=25). Object-detection-only keys (confidence/classes) are absent.
        private static string BuildMotion()
        {
            var schema = new
            {
                schema_version = "1.0",
                default_locale = "zh-TW",
                fields = new object[]
                {
                    JpgCompressField(),
                    new {
                        key = "sensitivity", type = "int",
                        label       = Loc(("zh-TW", "靈敏度"), ("en", "Sensitivity")),
                        description = Loc(("zh-TW", "偵測動作的靈敏程度，越高越容易觸發"), ("en", "How sensitive motion detection is; higher triggers more easily")),
                        @default = 50, value = 50, min = 1, max = 100
                    },
                    new {
                        key = "threshold", type = "int",
                        label       = Loc(("zh-TW", "門檻值"), ("en", "Threshold")),
                        description = Loc(("zh-TW", "觸發偵測所需的畫面變化量"), ("en", "Amount of frame change required to trigger")),
                        @default = 25, value = 25, min = 1, max = 100
                    },
                    new {
                        key = "trigger_interval", type = "int",
                        label       = Loc(("zh-TW", "觸發間隔（秒）"), ("en", "Trigger Interval (sec)")),
                        description = Loc(("zh-TW", "送出 HTTP POST 的最小間隔秒數；送出後這段時間內的觸發都不再發送。0 表示不限制，有觸發就送"), ("en", "Minimum seconds between motion HTTP POSTs; triggers within this window after a send are suppressed. 0 means no limit, send on every trigger")),
                        @default = 1, value = 1, min = 0, max = 3600
                    },
                }
            };
            return JsonConvert.SerializeObject(schema);
        }

        // Shared field: keyframe JPEG quality (1-100, higher = better quality /
        // larger payload). Common to every detector, so both schemas include it.
        // The wrapper clamps to <=100 and ignores <=0 (ParameterStore.Update);
        // EncodeWorker feeds this to turbojpeg per channel.
        private static object JpgCompressField() => new
        {
            key = "jpg_compress", type = "int",
            label       = Loc(("zh-TW", "JPEG 壓縮品質"), ("en", "JPEG Quality")),
            description = Loc(("zh-TW", "回傳關鍵影格的 JPEG 品質，越高畫質越好、檔案越大"), ("en", "JPEG quality of returned keyframes; higher is better quality and larger size")),
            @default = 30, value = 30, min = 1, max = 100
        };
    }
}
