using System;
using System.IO;

namespace GenericAI.App
{
    // Console output helper. Verbose/debug lines (WriteLine) are gated by a runtime
    // switch loaded once at startup from a "GenericAI.Config" file next to the exe:
    //
    //     show_debug = 1          # C# verbose console output (this class)
    //     show_native_debug = 1   # native informational log lines ([AI]/[PersonDetector]/
    //                             # [MotionDetector]/[channel]/[zmq]); mirrored into
    //                             # native via GAI_SetVerbose (see Program.cs)
    //     log_to_file = 1         # persist INFO/WARN/ERROR to a file (FileLogger),
    //                             # independent of the console switches above
    //
    // Both default OFF and are INDEPENDENT, so debug output can be toggled WITHOUT
    // rebuilding -- just drop/edit GenericAI.Config next to GenericAI.exe. ErrorLine
    // (failures: bad args, HTTP listener failed, native init failed, FATAL, ...) and
    // native std::cerr errors are ALWAYS printed so a run that dies early still shows
    // WHY, even with both switches off.
    internal static class ConsoleLog
    {
        // C# verbose/debug console output on (show_debug)? Set once on the startup
        // thread by LoadFromConfig(); volatile so worker threads that log see it.
        public static volatile bool Enabled = false;

        // Native informational logging on (show_native_debug)? Independent of Enabled;
        // Program.cs mirrors this into native via GAI_SetVerbose.
        public static volatile bool NativeDebug = false;

        // Config file looked up next to the executable.
        public const string ConfigFileName = "GenericAI.Config";

        // Read GenericAI.Config (if present) and set the show_debug / show_native_debug
        // switches. Each accepts 1 / true / yes / on (case-insensitive). Call once at
        // startup, BEFORE the first WriteLine and before GAI_SetVerbose. Never throws.
        public static void LoadFromConfig()
        {
            try
            {
                string dir = AppDomain.CurrentDomain.BaseDirectory ?? string.Empty;
                string path = Path.Combine(dir, ConfigFileName);
                if (!File.Exists(path))
                    return;

                foreach (string raw in File.ReadAllLines(path))
                {
                    // key = value, with '#' or '//' comments and blank lines ignored.
                    string line = raw.Trim();
                    if (line.Length == 0 || line[0] == '#' || line.StartsWith("//"))
                        continue;
                    int eq = line.IndexOf('=');
                    if (eq <= 0)
                        continue;
                    string key = line.Substring(0, eq).Trim();
                    string val = line.Substring(eq + 1).Trim();

                    if (string.Equals(key, "show_debug", StringComparison.OrdinalIgnoreCase))
                        Enabled = ParseBool(val);
                    else if (string.Equals(key, "show_native_debug", StringComparison.OrdinalIgnoreCase))
                        NativeDebug = ParseBool(val);
                    else if (string.Equals(key, "log_to_file", StringComparison.OrdinalIgnoreCase))
                        // Persist the verbose logs to %ProgramData%\Spark\GenericAI\Logs\
                        // GenericAI-<port>.log (async writer). Independent of the console
                        // switches above, so you can capture to file without console spam.
                        FileLogger.Enabled = ParseBool(val);
                }
            }
            catch
            {
                // A missing/broken config must never break startup: leave flags as-is.
            }
        }

        private static bool ParseBool(string val)
        {
            return val == "1"
                || string.Equals(val, "true", StringComparison.OrdinalIgnoreCase)
                || string.Equals(val, "yes", StringComparison.OrdinalIgnoreCase)
                || string.Equals(val, "on", StringComparison.OrdinalIgnoreCase);
        }

        // Verbose/debug line -- gated by show_debug.
        public static void WriteLine(string line)
        {
            if (!Enabled) return;
            Console.WriteLine(line);
        }

        // Error/failure line -- ALWAYS shown, regardless of show_debug.
        public static void ErrorLine(string line)
        {
            Console.Error.WriteLine(line);
        }
    }
}
