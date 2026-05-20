#include "pch.h"
#include <iostream>
#include <string>
#include <cstdint>
#include <thread>
#include <mutex>
#include <atomic>
#include <map>
#include <memory>
#include <vector>
#include "detectors_motion.h"

using namespace std;

// ── Shared structs (identical layout to multi-mode for P/Invoke compatibility) ─────────────────

struct ROI {
    int x;
    int y;
};

struct SettingParameters {
    char version[32];
    char analytics_event_api_url[256];
    int image_width;
    int image_height;
    int jpg_compress;
    int sensitivity[10];
    int threshold[10];
    ROI rois[10][10];
    // v1.3 additions
    char mode[32];
    char channel_id[64];
    char count_reset[32];
    char ai_settings_json[4096];
};

const int MMF_DATA_HEADER = 0x1234;
const int MMF_DATA_FOOTER = 0x4321;

struct MMF_Data {
    __int64 header    = MMF_DATA_HEADER;
    int image_status  = 0;
    int image_width   = 0;
    int image_height  = 0;
    int image_size    = 0;
    uint64_t timestamp = 0;
    unsigned char image_data[1920 * 1080 * 3];
    __int64 footer    = MMF_DATA_FOOTER;
};

typedef void(__stdcall* CallBackFunction)(int channelid, int width, int height,
    unsigned char* imageframe, int image_size, uint64_t timestamp,
    ROI* rois_rects, int rois_count, int node_count);

// ── Per-channel state ─────────────────────────────────────────────────────────────────────────
//
// settings_mtx protects all mutable channel state (hMap, roi_rects, detector, is_setting).
// g_channels_mtx protects the map itself.
// running is atomic so the detection thread can bail without holding any lock.

struct ChannelState {
    int          port_num   = 0;
    char         mmf_name[64] = {};

    HANDLE       hMap       = INVALID_HANDLE_VALUE;
    atomic<bool> running    { false };
    bool         is_setting = false;

    mutex        settings_mtx;               // per-channel lock

    MotionDetector*       detector = nullptr;
    vector<ROIRect>       roi_rects;
    vector<vector<ROI>>   original_roi_points;

    thread bg_thread;
};

// ── Globals ───────────────────────────────────────────────────────────────────────────────────
// shared_ptr keeps ChannelState alive while a detection thread still holds a reference to it.

mutex                              g_channels_mtx;
map<int, shared_ptr<ChannelState>> g_channels;
CallBackFunction                   g_callbackFunctionPtr = nullptr;

// ── MMF helper ────────────────────────────────────────────────────────────────────────────────
// Must be called with ch->settings_mtx held.

static int getMMFForChannel(ChannelState* ch, unsigned char** frame,
    int& width, int& height, int& size, uint64_t& timestamp)
{
    int mmf_size = sizeof(MMF_Data);

    if (ch->hMap == INVALID_HANDLE_VALUE || ch->hMap == NULL)
        ch->hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, false, ch->mmf_name);

    if (ch->hMap == NULL)
        return -1;

    MMF_Data* data = (MMF_Data*)MapViewOfFile(ch->hMap, FILE_MAP_ALL_ACCESS, 0, 0, mmf_size);
    if (data == NULL) {
        CloseHandle(ch->hMap);
        ch->hMap = INVALID_HANDLE_VALUE;
        return -1;
    }

    if (data->header != MMF_DATA_HEADER || data->footer != MMF_DATA_FOOTER) {
        memset(data, 0, sizeof(MMF_Data));
        data->header = MMF_DATA_HEADER;
        data->footer = MMF_DATA_FOOTER;
    }

    if (data->image_status == 1) {
        if (*frame != nullptr) { delete[] *frame; *frame = nullptr; }
        *frame = new unsigned char[data->image_size];
        memcpy(*frame, data->image_data, data->image_size);
        timestamp   = data->timestamp;
        size        = data->image_size;
        width       = data->image_width;
        height      = data->image_height;
        data->image_status = 2;
    }

    UnmapViewOfFile(data);
    return 1;
}

// ── Per-channel detection thread ──────────────────────────────────────────────────────────────
//
// Locking strategy:
//   1. Hold g_channels_mtx BRIEFLY to get a shared_ptr → all other channels run freely.
//   2. Hold ch->settings_mtx while reading MMF + running detection → only this channel waits
//      if SetChannelParameters arrives concurrently.
//   3. Sleep OUTSIDE all locks.

void ChannelRecognizeTask(int channelPort)
{
    cout << "[Single] Channel " << channelPort << " detection thread started" << endl;

    int debugCount = 0;
    int mmfFailCount = 0;

    while (true) {
        // ── Step 1: get channel reference (brief global lock) ──────────────────
        shared_ptr<ChannelState> ch;
        {
            lock_guard<mutex> lk(g_channels_mtx);
            auto it = g_channels.find(channelPort);
            if (it == g_channels.end() || !it->second->running.load())
                break;
            ch = it->second;
        }

        // ── Step 2: MMF read + detection (per-channel lock, not global) ────────
        unsigned char* image_data = nullptr;
        int  w = 0, h = 0, sz = 0;
        uint64_t ts = 0;
        bool should_stop = false;

        {
            lock_guard<mutex> lk(ch->settings_mtx);

            if (!ch->running.load()) {
                should_stop = true;
            } else {
                int mmfRet = getMMFForChannel(ch.get(), &image_data, w, h, sz, ts);

                // ── Periodic heartbeat (every 300 loops ≈ 1.5 s) ──────────────
                if (debugCount % 300 == 0) {
                    cout << "[Single] Ch" << channelPort
                         << " loop#" << debugCount
                         << " is_setting=" << ch->is_setting
                         << " mmf=" << (mmfRet > 0 ? "ok" : "FAIL(" + to_string(mmfFailCount) + ")")
                         << " sz=" << sz
                         << " rois=" << ch->roi_rects.size()
                         << " detector=" << (ch->detector ? "yes" : "no")
                         << endl;
                }
                if (mmfRet <= 0) mmfFailCount++; else mmfFailCount = 0;
                debugCount++;

                if (ch->is_setting && sz > 0) {
                    if (ch->detector && !ch->roi_rects.empty()) {
                        vector<int> detected_idx;
                        int n = ch->detector->Detect(image_data, w, h,
                            ch->roi_rects.data(), (int)ch->roi_rects.size(), detected_idx);

                        if (n > 0 && g_callbackFunctionPtr) {
                            cout << "[Single] Ch" << channelPort
                                 << " motion detected n=" << n << ", firing callback" << endl;
                            vector<ROI> flat;
                            for (int ri : detected_idx)
                                if (ri >= 0 && ri < (int)ch->original_roi_points.size())
                                    for (auto& pt : ch->original_roi_points[ri])
                                        flat.push_back(pt);

                            int nodes = n > 0 ? (int)flat.size() / n : 0;
                            g_callbackFunctionPtr(channelPort, w, h,
                                image_data, sz, ts, flat.data(), n, nodes);
                        }
                    } else {
                        // Fallback debug: fire once every 60 frames
                        if (debugCount % 60 == 0 && g_callbackFunctionPtr) {
                            ROI rois[2][4] = {
                                { {0,0},{10,10},{30,30},{40,40} },
                                { {50,50},{60,60},{70,70},{80,80} }
                            };
                            ROI flat[8];
                            int idx = 0;
                            for (int i = 0; i < 2; i++)
                                for (int j = 0; j < 4; j++)
                                    flat[idx++] = rois[i][j];
                            g_callbackFunctionPtr(channelPort, w, h,
                                image_data, sz, ts, flat, 2, 4);
                        }
                    }
                }
            }
        } // ← release per-channel lock here

        if (image_data) { delete[] image_data; image_data = nullptr; }
        if (should_stop) break;

        // ── Step 3: sleep outside all locks ──────────────────────────────────
        this_thread::sleep_for(chrono::milliseconds(5));
    }

    cout << "[Single] Channel " << channelPort << " detection thread stopped" << endl;
}

// ── Exported DLL functions ────────────────────────────────────────────────────────────────────

extern "C" {

    __declspec(dllexport) void Initialize(int basePort)
    {
        cout << "[Single] DLL Initialized, base port = " << basePort << endl;
    }

    // OpenChannel: allocates a NEW ChannelState with its own shared-memory name and
    // starts a dedicated detection thread.  Each channel reads a separate MMF region.
    __declspec(dllexport) void OpenChannel(int channelPort, const char* mmfName)
    {
        {
            lock_guard<mutex> lk(g_channels_mtx);
            if (g_channels.count(channelPort)) {
                cout << "[Single] Channel " << channelPort << " already open" << endl;
                return;
            }
            auto ch        = make_shared<ChannelState>();
            ch->port_num   = channelPort;
            ch->detector   = new MotionDetector(500, 25, 50);
            ch->running    = true;
            strncpy_s(ch->mmf_name, mmfName, sizeof(ch->mmf_name) - 1);
            g_channels[channelPort] = ch;
            // Start thread while map entry exists so first iteration finds itself
            ch->bg_thread = thread(ChannelRecognizeTask, channelPort);
        }
        cout << "[Single] Channel " << channelPort << " opened, mmf=" << mmfName << endl;
    }

    // CloseChannel: signals the thread, removes from map, then joins safely.
    // The shared_ptr keeps ChannelState alive until join() returns and 'ch' goes out of scope.
    __declspec(dllexport) void CloseChannel(int channelPort)
    {
        shared_ptr<ChannelState> ch;
        {
            lock_guard<mutex> lk(g_channels_mtx);
            auto it = g_channels.find(channelPort);
            if (it == g_channels.end()) return;
            ch = it->second;
            ch->running = false;     // signal thread to exit
            g_channels.erase(it);    // remove from map (shared_ptr still held locally)
        }
        // Join outside global lock so detection thread can re-acquire g_channels_mtx to exit
        if (ch->bg_thread.joinable())
            ch->bg_thread.join();

        // Cleanup under per-channel lock to flush any in-progress detection work
        {
            lock_guard<mutex> lk(ch->settings_mtx);
            if (ch->hMap != INVALID_HANDLE_VALUE) {
                CloseHandle(ch->hMap);
                ch->hMap = INVALID_HANDLE_VALUE;
            }
            if (ch->detector) { delete ch->detector; ch->detector = nullptr; }
        }
        cout << "[Single] Channel " << channelPort << " closed" << endl;
        // shared_ptr destructor frees ChannelState
    }

    // SetChannelParameters: obtains a shared_ptr under the global lock, then holds only the
    // per-channel lock during the actual update → does NOT block other channels' detection.
    __declspec(dllexport) void SetChannelParameters(int channelPort, const struct SettingParameters* params)
    {
        shared_ptr<ChannelState> ch;
        {
            lock_guard<mutex> lk(g_channels_mtx);
            auto it = g_channels.find(channelPort);
            if (it == g_channels.end()) {
                cout << "[Single] SetChannelParameters: channel " << channelPort << " not found" << endl;
                return;
            }
            ch = it->second;
        }

        lock_guard<mutex> lk(ch->settings_mtx);   // per-channel lock only

        cout << "[Single] Ch" << channelPort << " SetParameters:" << endl;
        cout << "  version:    " << params->version << endl;
        cout << "  url:        " << params->analytics_event_api_url << endl;
        cout << "  resolution: " << params->image_width << "x" << params->image_height << endl;
        cout << "  jpg:        " << params->jpg_compress << endl;
        if (strlen(params->mode)            > 0) cout << "  mode:       " << params->mode << endl;
        if (strlen(params->channel_id)      > 0) cout << "  channel_id: " << params->channel_id << endl;
        if (strlen(params->count_reset)     > 0) cout << "  count_reset:" << params->count_reset << endl;
        if (strlen(params->ai_settings_json)> 0) cout << "  ai_settings:" << params->ai_settings_json << endl;

        ch->roi_rects.clear();
        ch->original_roi_points.clear();

        for (int i = 0; i < 10; i++) {
            if (params->sensitivity[i] <= 0) continue;
            if (ch->detector && i == 0)
                ch->detector->SetThresholdAndSensitivity(params->threshold[i], params->sensitivity[i]);

            vector<ROI> pts;
            for (int j = 0; j < 10; j++)
                if (params->rois[i][j].x >= 0)
                    pts.push_back(params->rois[i][j]);

            if ((int)pts.size() >= 2) {
                int last = ((int)pts.size() >= 3) ? 2 : (int)pts.size() - 1;
                ROIRect rect { pts[0].x, pts[0].y, pts[last].x, pts[last].y };
                ch->roi_rects.push_back(rect);
                ch->original_roi_points.push_back(pts);
            }
        }

        ch->is_setting = true;
        cout << "[Single] Ch" << channelPort << " ready with "
             << ch->roi_rects.size() << " ROI(s)" << endl;
    }

    __declspec(dllexport) void RegisterCallback(CallBackFunction callback)
    {
        g_callbackFunctionPtr = callback;
    }

    __declspec(dllexport) void UnregisterCallback()
    {
        g_callbackFunctionPtr = nullptr;
    }

    __declspec(dllexport) void Deinitialize()
    {
        // snapshot ports then close each
        vector<int> ports;
        {
            lock_guard<mutex> lk(g_channels_mtx);
            for (auto& kv : g_channels)
                ports.push_back(kv.first);
        }
        for (int p : ports)
            CloseChannel(p);

        cout << "[Single] DLL Deinitialized" << endl;
    }

} // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        cout << "[Single] DLL Loaded" << endl;
        break;
    case DLL_PROCESS_DETACH:
        cout << "[Single] DLL Unloaded" << endl;
        break;
    }
    return TRUE;
}
