using System.Collections.Generic;

namespace GenericAI.App
{
    // Serialised verbatim to the analytics_event_api_url endpoint that
    // spark.recorder is listening on (parser at
    // spark.recorder/modules/AIService/Generic/GenericAIDevice.cpp:249-312).
    // Fields below match the v1.2 schema; do not rename or recase. byte[]
    // keyframe serialises as a base64 string under Newtonsoft.
    internal sealed class HttpEnvelope
    {
        public string version { get; set; }
        public int port_num { get; set; }
        public byte[] keyframe { get; set; }
        public ulong timestamp { get; set; }
        public List<List<NativeInterop.ROI>> rois_rects { get; set; }
    }
}
