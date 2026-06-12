using System;

namespace GenericAI.App
{
    // Gates the console lines that mirror the CSharp sample's output
    // (HTTP server state, SetParameters echo, send results, startup info).
    // Manual switch like FileLogger.Enabled / TimingRecorder.Enabled:
    // edit Enabled -> rebuild. Default true keeps the sample-matching output.
    //
    // Enabled is a compile-time const: when false, the guard makes the rest
    // of each body unreachable (CS0162) and the JIT strips the calls, so the
    // call sites cost nothing. Timing lines are gated by TimingRecorder.Enabled
    // instead and are unaffected by this switch.
#pragma warning disable 162
    internal static class ConsoleLog
    {
        public const bool Enabled = true;

        public static void WriteLine(string line)
        {
            if (!Enabled) return;
            Console.WriteLine(line);
        }

        public static void ErrorLine(string line)
        {
            if (!Enabled) return;
            Console.Error.WriteLine(line);
        }
    }
#pragma warning restore 162
}
