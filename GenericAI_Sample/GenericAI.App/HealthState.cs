namespace GenericAI.App
{
    // Process-wide health, surfaced on GET /Alive so the recorder (ARGO) can tell
    // whether this wrapper is functional. This Motion-only reference wrapper has no
    // model to load, so it stays healthy; the mechanism is here for protocol parity
    // with the full GenericAI wrapper (which reports detector/model failures here).
    internal static class HealthState
    {
        // null / empty => healthy. Volatile: written once at startup, read from
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
