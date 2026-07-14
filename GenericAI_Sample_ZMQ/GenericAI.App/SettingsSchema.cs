using System.Collections.Generic;
using Newtonsoft.Json;

namespace GenericAI.App
{
    // Built-in payload for GET /GetSettingsSchema (spec §5). The ConfigClient GETs
    // this to render the dynamic AI-settings UI; the user's filled values come back
    // verbatim in SetParameters.ai_settings (same field shape, each carrying value).
    //
    // This is the MOTION-only sample wrapper, so the schema exposes just the motion
    // tunables (sensitivity + threshold) plus the shared jpg_compress / trigger_interval.
    // (The full GenericAI_ZMQ wrapper additionally serves an object-detection schema.)
    //
    // Per spec §5.5 (i18n) label / description / option.label are ALWAYS locale maps
    // ({ "zh-TW": ..., "en": ... }); ConfigClient resolves by current UI locale,
    // falling back to default_locale. Each field carries both `default` and `value`;
    // on serve value == default and the recorder overwrites value when feeding it back.
    internal static class SettingsSchema
    {
        // Motion schema JSON, built once. Served verbatim by GET /GetSettingsSchema.
        public static string Json => MotionJson;

        private static readonly string MotionJson = BuildMotion();

        // Locale-map helper (spec §5.5): each entry is an explicit (locale, text) pair
        // so it serialises to { "zh-TW": ..., "en": ... }. Add languages by adding pairs.
        private static Dictionary<string, string> Loc(params (string locale, string text)[] entries)
        {
            var map = new Dictionary<string, string>(entries.Length);
            foreach (var e in entries) map[e.locale] = e.text;
            return map;
        }

        // Motion schema: the motion detector's tunables (sensitivity + change threshold),
        // mirroring the native MotionDetector defaults (sensitivity=50, threshold=25).
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
        // larger payload). The wrapper clamps to <=100 and ignores <=0.
        private static object JpgCompressField() => new
        {
            key = "jpg_compress", type = "int",
            label       = Loc(("zh-TW", "JPEG 壓縮品質"), ("en", "JPEG Quality")),
            description = Loc(("zh-TW", "回傳關鍵影格的 JPEG 品質，越高畫質越好、檔案越大"), ("en", "JPEG quality of returned keyframes; higher is better quality and larger size")),
            @default = 30, value = 30, min = 1, max = 100
        };
    }
}
