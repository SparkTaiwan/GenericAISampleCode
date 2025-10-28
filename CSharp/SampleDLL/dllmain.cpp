#include "pch.h"
#include <iostream>
#include <string>
#include <cstdint>  // for int64_t and uint64_t
#include <thread>
#include <mutex>

#include <vector>

using namespace std;

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
};


const int MMF_DATA_HEADER = 0x1234;
const int MMF_DATA_FOOTER = 0x4321;

struct MMF_Data {
    __int64 header = MMF_DATA_HEADER;
    //video status : 0=no use , 1=new frame, 2=detection got frame
    int image_status = 0;
    //resolution
    int image_width = 0;
    int image_height = 0;
    //video size
    int image_size = 0;
    //timestamp in Windows FileTime style 
    uint64_t  timestamp = 0;
    //video data
    unsigned char image_data[1920 * 1080 * 3];
    __int64 footer = MMF_DATA_FOOTER;
};

typedef void(__stdcall* CallBackFunction)(int channelid ,int width, int height,unsigned char* imageframe, int image_size, uint64_t timestamp, ROI* rois_rects , int rois_count ,int node_count);

HANDLE g_hMap = INVALID_HANDLE_VALUE;
int g_portnum = 0;
mutex g_mtx;
bool g_running = true;
thread g_bgThread;
string g_url;
CallBackFunction g_callbackFunctionPtr = nullptr;
bool g_isSetting = false;

int getMMF(unsigned char** frame, int& width, int& height, int& size, uint64_t& timestamp)
{
    int check = 0, count = 0;
    int retry_count = 0;
    char mmf_PT[200] = "ChannelFrame_%d";
    char mmf_name[200] = "";

    int mmf_size = sizeof(MMF_Data);
    //MMF process

    HANDLE hFile = INVALID_HANDLE_VALUE;
    sprintf_s(mmf_name, mmf_PT, g_portnum);

    if (!g_hMap || g_hMap == INVALID_HANDLE_VALUE)
    {
        g_hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, false, mmf_name);
    }
    if (g_hMap == NULL)
    {
        //cout << " get shared mem  1 " << mmf_name << endl;
        return -1;
    }

    MMF_Data* data = NULL;
    data = (MMF_Data*)MapViewOfFile(g_hMap, FILE_MAP_ALL_ACCESS, 0, 0, mmf_size);
    if (data == NULL)
    {
        CloseHandle(g_hMap);
        cout << " get shared mem  2" << endl;
        return -1;
    }

    //init
    if (data->header != MMF_DATA_HEADER || data->footer != MMF_DATA_FOOTER)
    {
        memset(data, 0, sizeof(MMF_Data));
        data->header = MMF_DATA_HEADER;
        data->footer = MMF_DATA_FOOTER;
    }    
    check = 0;

    //get image data
    //video status : 0=no use , 1=new frame, 2=detection got frame
    if (data->image_status == 1) //refresh 
    {
        if (*frame != nullptr) {
            delete[] * frame;
            *frame = nullptr;
        }
        *frame = new unsigned char[data->image_size];
        memcpy(*frame, data->image_data, data->image_size);
        timestamp = data->timestamp;
        size = data->image_size;
        width = data->image_width;
        height = data->image_height;
        
        //to MMF
        data->image_status = 2;
    }

    UnmapViewOfFile(data);

    return 1;
}


void RecognizeTask()
{
    cout << "start get shared mem thread " << endl;
    bool isDetected = false;
    int count = 0;
    while (true)
    {        
         {
            lock_guard<mutex> lock(g_mtx);
            if (!g_running)
                break;
            // Use the getMMF function
            unsigned char *image_data= nullptr;
            int image_width = 0, image_height = 0, image_size = 0;
            uint64_t timestamp = 0;
            
            if (getMMF(&image_data, image_width, image_height, image_size, timestamp) == 1)
            {
                if (g_isSetting && image_size > 0 )// get image and is received setting.
                {
                    // Perform recognition and fill result   
                    // for debug ==> this should be some detected method return roi_rects and count;
                    if (count % 60 == 0) isDetected = true; 
                    
                    if (isDetected && g_callbackFunctionPtr)
                    {
                        ROI rois[2][4] = {
                            { {0, 0}, {10, 10}, {30, 30}, {40, 40} },
                            { {50, 50}, {60, 60}, {70, 70}, {80, 80} }
                        };                        

                        ROI flattened_rois[2 * 4];
                        int index = 0;
                        for (int i = 0; i < 2; ++i) {
                            for (int j = 0; j < 4; ++j) {
                                flattened_rois[index] = rois[i][j];
                                index++;
                            }
                        }
                        //When detected , callback to C# layer to send Http event to Argo
                        g_callbackFunctionPtr(g_portnum, image_width, image_height, image_data, image_size, timestamp, flattened_rois, 2,4);
                        isDetected = false;
                    }

                    count++;
                }
            }    

            if ( image_data != nullptr) {
                delete[] image_data;
                image_data = nullptr;
            }
        }

        // Sleep for a specified duration
        this_thread::sleep_for(chrono::milliseconds(5));
    }
    cout << "exit get shared mem thread " << endl;
}

extern "C" {    

    __declspec(dllexport) void Initialize( int PortNumber)
    {
        // Initialization code
        g_portnum = PortNumber;
        std::cout << "DLL Initialized, Port ID =" << g_portnum << std::endl;
        g_bgThread = std::thread(RecognizeTask);


    }
    __declspec(dllexport) void SettingParameters(const struct SettingParameters* parameters)
    {
        g_url = parameters->analytics_event_api_url;
        // Store parameters
        std::cout << "Parameters set:" << std::endl;
        std::cout << "version: " << parameters->version << std::endl;
        std::cout << "analytics_event_api_url: " << parameters->analytics_event_api_url << std::endl;
        std::cout << "image_width: " << parameters->image_width << std::endl;
        std::cout << "image_height: " << parameters->image_height << std::endl;
        std::cout << "jpg_compress: " << parameters->jpg_compress << std::endl;
        for (int i = 0; i < 10; ++i) 
        {
            if (parameters->sensitivity[i] > 0)
            {
                std::cout << "sensitivity: " << parameters->sensitivity[i] << std::endl;
                std::cout << "threshold: " << parameters->threshold[i] << std::endl;
            }

            for (int j = 0; j < 10; ++j) {
                if (parameters->rois[i][j].x >= 0)
                {
                    std::cout << "ROI " << i << ": (" << parameters->rois[i][j].x << ", " << parameters->rois[i][j].y << ")" << std::endl;
                }
            }
        }
        g_isSetting =true;
    }

    __declspec(dllexport) void registerCallback(CallBackFunction callback)
    {
        g_callbackFunctionPtr = callback;
    }

    __declspec(dllexport) void unregisterCallback()
    {
        g_callbackFunctionPtr = nullptr;
    }

    __declspec(dllexport) void Deinitialize()
    {
        // Deinitialization code
        std::cout << "DLL Deinitialized" << std::endl;

        g_bgThread.join();
    }
}



BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        std::cout << "DLL Loaded" << std::endl;
        break;
    case DLL_THREAD_ATTACH:
        break;
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        std::cout << "DLL Unloaded" << std::endl;
        // Ensure any resources are cleaned up here
        // Ensure to join the background thread if it's running
        if (g_bgThread.joinable()) {
            g_running = false;
            g_bgThread.join();
        }
        break;
    }
    return TRUE;
}
