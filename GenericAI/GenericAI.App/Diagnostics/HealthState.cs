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
    }
}
