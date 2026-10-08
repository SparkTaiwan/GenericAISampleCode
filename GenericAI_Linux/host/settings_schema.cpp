#include "settings_schema.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <utility>

namespace gai_host {

namespace {

using ojson = nlohmann::ordered_json;

std::atomic<int> g_detector_kind{0};  // Motion

// Locale map (spec §5.5): { "zh-TW": ..., "en": ... }.
ojson Loc(const char* zh, const char* en) {
    ojson m = ojson::object();
    m["zh-TW"] = zh;
    m["en"] = en;
    return m;
}

// Shared field: keyframe JPEG quality, channel-scoped (output quality, not
// detection tuning).
ojson JpgCompressField() {
    ojson f;
    f["key"] = "jpg_compress";
    f["type"] = "int";
    f["scope"] = "channel";
    f["label"] = Loc("JPEG 壓縮品質", "JPEG Quality");
    f["description"] = Loc("回傳關鍵影格的 JPEG 品質，越高畫質越好、檔案越大",
                           "JPEG quality of returned keyframes; higher is better quality and larger size");
    f["default"] = 30;
    f["value"] = 30;
    f["min"] = 1;
    f["max"] = 100;
    return f;
}

ojson TriggerIntervalField(const char* zh_desc, const char* en_desc) {
    ojson f;
    f["key"] = "trigger_interval";
    f["type"] = "int";
    f["scope"] = "channel";
    f["label"] = Loc("觸發間隔（秒）", "Trigger Interval (sec)");
    f["description"] = Loc(zh_desc, en_desc);
    f["default"] = 1;
    f["value"] = 1;
    f["min"] = 0;
    f["max"] = 3600;
    return f;
}

ojson FloatField(const char* key, const char* zh_label, const char* en_label,
                 const char* zh_desc, const char* en_desc,
                 double def, double min, double max, double step) {
    ojson f;
    f["key"] = key;
    f["type"] = "float";
    f["scope"] = "roi";
    f["label"] = Loc(zh_label, en_label);
    f["description"] = Loc(zh_desc, en_desc);
    f["default"] = def;
    f["value"] = def;
    f["min"] = min;
    f["max"] = max;
    f["step"] = step;
    return f;
}

std::string BuildObjectDetection() {
    ojson fields = ojson::array();
    fields.push_back(JpgCompressField());
    fields.push_back(FloatField("confidence", "信心門檻", "Confidence",
                                "低於此分數的偵測結果會被濾掉",
                                "Detections below this score are dropped",
                                0.70, 0.10, 1.00, 0.05));
    {
        ojson f;
        f["key"] = "classes";
        f["type"] = "string_array";
        f["scope"] = "roi";
        f["counting"] = true;
        f["label"] = Loc("偵測類別", "Classes");
        const std::pair<const char*, std::pair<const char*, const char*>> opts[] = {
            {"person",     {"人", "Person"}},
            {"car",        {"汽車", "Car"}},
            {"bus",        {"公車", "Bus"}},
            {"truck",      {"卡車", "Truck"}},
            {"motorcycle", {"機車", "Motorcycle"}},
            {"bicycle",    {"自行車", "Bicycle"}},
            {"cat",        {"貓", "Cat"}},
            {"dog",        {"狗", "Dog"}},
        };
        ojson options = ojson::array();
        for (const auto& o : opts) {
            ojson opt;
            opt["value"] = o.first;
            opt["label"] = Loc(o.second.first, o.second.second);
            options.push_back(opt);
        }
        f["options"] = options;
        f["default"] = ojson::array({"person", "car"});
        f["value"] = ojson::array({"person", "car"});
        fields.push_back(f);
    }
    fields.push_back(FloatField("object_size_min", "最小物件大小（畫面佔比 %）", "Min Object Size (% of frame)",
                                "只保留邊界框面積大於畫面此比例的物件，用來濾掉太小的偵測。0 表示不限制下限",
                                "Keep only objects whose bounding-box area exceeds this percentage of the frame; filters out tiny detections. 0 means no lower limit",
                                0.0, 0.0, 100.0, 0.5));
    fields.push_back(FloatField("object_size_max", "最大物件大小（畫面佔比 %）", "Max Object Size (% of frame)",
                                "只保留邊界框面積小於畫面此比例的物件，用來濾掉太大的偵測。100 表示不限制上限",
                                "Keep only objects whose bounding-box area is below this percentage of the frame; filters out oversized detections. 100 means no upper limit",
                                100.0, 0.0, 100.0, 0.5));
    fields.push_back(TriggerIntervalField(
        "送出 HTTP POST 的最小間隔秒數；送出後這段時間內的偵測都不再發送。0 表示不限制，有偵測就送",
        "Minimum seconds between HTTP POSTs; detections within this window after a send are suppressed. 0 means no limit, send on every detection"));

    ojson schema;
    schema["schema_version"] = "1.0";
    schema["default_locale"] = "zh-TW";
    schema["fields"] = fields;
    return schema.dump();
}

ojson IntRoiField(const char* key, const char* zh_label, const char* en_label,
                  const char* zh_desc, const char* en_desc, int def) {
    ojson f;
    f["key"] = key;
    f["type"] = "int";
    f["scope"] = "roi";
    f["label"] = Loc(zh_label, en_label);
    f["description"] = Loc(zh_desc, en_desc);
    f["default"] = def;
    f["value"] = def;
    f["min"] = 1;
    f["max"] = 100;
    return f;
}

std::string BuildMotion() {
    ojson fields = ojson::array();
    fields.push_back(JpgCompressField());
    fields.push_back(IntRoiField("sensitivity", "靈敏度", "Sensitivity",
                                 "偵測動作的靈敏程度，越高越容易觸發",
                                 "How sensitive motion detection is; higher triggers more easily", 50));
    fields.push_back(IntRoiField("threshold", "門檻值", "Threshold",
                                 "觸發偵測所需的畫面變化量",
                                 "Amount of frame change required to trigger", 25));
    fields.push_back(TriggerIntervalField(
        "送出 HTTP POST 的最小間隔秒數；送出後這段時間內的觸發都不再發送。0 表示不限制，有觸發就送",
        "Minimum seconds between motion HTTP POSTs; triggers within this window after a send are suppressed. 0 means no limit, send on every trigger"));

    ojson schema;
    schema["schema_version"] = "1.0";
    schema["default_locale"] = "zh-TW";
    schema["fields"] = fields;
    return schema.dump();
}

}  // namespace

void SettingsSchema::Configure(int detector_kind) { g_detector_kind = detector_kind; }

const std::string& SettingsSchema::Json() {
    static const std::string motion = BuildMotion();
    static const std::string object_detection = BuildObjectDetection();
    return g_detector_kind.load() == 0 ? motion : object_detection;
}

}  // namespace gai_host
