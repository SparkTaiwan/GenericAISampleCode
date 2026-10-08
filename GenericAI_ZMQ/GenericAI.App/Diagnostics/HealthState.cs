namespace GenericAI.App
{
    // Process-wide health, surfaced on GET /Alive so the recorder (ARGO) can tell
    // whether this wrapper is functional. When the detector fails to initialise
    // (e.g. the ONNX model is missing) the process stays alive in a degraded state
    // and reports the reason here instead of crashing.
    internal static class HealthState
    {
        // null / empty => healthy. Volatile: written once from Main, read from
        // HTTP handler threads.
        private static volatile string s_error;

        public static void SetError(string message)
        {
            s_error = string.IsNullOrEmpty(message) ? "unknown error" : message;
        }

        public static string Error
        {
            get { return s_error; }
        }

        public static bool IsHealthy
        {
            get { return string.IsNullOrEmpty(s_error); }
        }

        // Set once native init has finished: after the default ai_settings seed
        // and the ZMQ receiver start, or after the decision to run degraded. The
        // HTTP listeners are up before that (so the port-in-use check comes
        // first); until then /SetParameters answers 503 so the recorder retries
        // (it keeps m_needSendSetting until it gets a 200). Answering 200 there
        // used to drop the settings — native had no channels yet — and the
        // default seed then overwrote them.
        private static volatile bool s_nativeReady;

        public static void SetNativeReady()
        {
            s_nativeReady = true;
        }

        public static bool IsNativeReady
        {
            get { return s_nativeReady; }
        }
    }
}
