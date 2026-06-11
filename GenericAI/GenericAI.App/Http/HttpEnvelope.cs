using System.Collections.Generic;
using Newtonsoft.Json;

namespace GenericAI.App
{
    // Serialised verbatim to the analytics_event_api_url endpoint that
    // spark.recorder is listening on (parser at
    // spark.recorder/modules/AIService/Generic/GenericAIDevice.cpp:249-312).
    // Fields below match v1.2 schema; do not rename or recase.
    internal sealed class HttpEnvelope
    {
        public string version { get; set; }
        public int port_num { get; set; }
        // byte[] serialises as a base64 string under Newtonsoft — same wire
        // format as the previous Convert.ToBase64String path, minus the
        // intermediate base64 string allocation (LOH-sized for typical JPEGs).
        public byte[] keyframe { get; set; }
        public ulong timestamp { get; set; }

        // Flat ROI storage: length = RoisCount * NodeCount. Avoids the per-frame
        // List<List<ROI>> allocation done previously in the callback hot path.
        [JsonIgnore]
        public NativeInterop.ROI[] RoisFlat { get; set; }
        [JsonIgnore]
        public int RoisCount { get; set; }
        [JsonIgnore]
        public int NodeCount { get; set; }

        // Restores nested [[{x,y},...],...] wire format on serialise without
        // materialising the outer/inner Lists. Newtonsoft.Json walks
        // IEnumerable<ROI> via the generic Serialize<T> path so the ROI struct
        // is not boxed.
        [JsonProperty("rois_rects")]
        public IEnumerable<IEnumerable<NativeInterop.ROI>> RoisRects
        {
            get
            {
                int rois = RoisCount;
                int nodes = NodeCount;
                NativeInterop.ROI[] flat = RoisFlat;
                for (int i = 0; i < rois; i++)
                    yield return EnumerateGroup(flat, i * nodes, nodes);
            }
        }

        private static IEnumerable<NativeInterop.ROI> EnumerateGroup(
            NativeInterop.ROI[] arr, int baseIdx, int count)
        {
            for (int j = 0; j < count; j++) yield return arr[baseIdx + j];
        }
    }
}
