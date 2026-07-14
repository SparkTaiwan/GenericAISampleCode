using System;
using System.IO;

namespace GenericAI.App
{
    // Console output helper. Verbose/debug lines (WriteLine) are gated by a runtime
    // switch loaded once at startup from a "GenericAI.Config" file next to the exe:
    //
    //     show_debug = 1
    //
    // enables them (default: OFF), so debug output can be toggled WITHOUT rebuilding
    // -- just drop/edit GenericAI.Config next to GenericAI.exe. ErrorLine (failures:
    // bad args, HTTP listener failed, native init failed, FATAL, ...) is ALWAYS printed
    // so a run that dies early still shows WHY, even with debug output off.
    internal static class ConsoleLog
    {
        // Verbose/debug console output on? Set once on the startup thread by
        // LoadFromConfig(); volatile so the worker threads that log see the value.
        public static volatile bool Enabled = false;

        // Config file looked up next to the executable.
        public const string ConfigFileName = "GenericAI.Config";

        // Read GenericAI.Config (if present) and turn verbose console output on when it
        // contains "show_debug = 1" (also accepts true / yes / on, case-insensitive).
        // Call once at startup, BEFORE the first WriteLine. Never throws.
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
                    if (!string.Equals(key, "show_debug", StringComparison.OrdinalIgnoreCase))
                        continue;

                    string val = line.Substring(eq + 1).Trim();
                    Enabled = val == "1"
                        || string.Equals(val, "true", StringComparison.OrdinalIgnoreCase)
                        || string.Equals(val, "yes", StringComparison.OrdinalIgnoreCase)
                        || string.Equals(val, "on", StringComparison.OrdinalIgnoreCase);
                }
            }
            catch
            {
                // A missing/broken config must never break startup: leave Enabled as-is.
            }
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
