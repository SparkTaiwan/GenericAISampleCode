namespace GenericAI.App
{
    internal sealed class CommandLineArgs
    {
        // Lets `F5 / Debug Run` work without configuring VS launch args.
        // Production spawn always passes port= explicitly
        // (spark.recorder/.../AStreamPerimeter_GenericAI.cpp:442). Kept below
        // Windows' default dynamic port range (49152..65535) so the consecutive
        // [port, port + channel_count) block doesn't collide with whatever
        // outbound connections other processes happen to be holding.
        public const int DefaultPort = 46000;

        public int Port { get; private set; } = DefaultPort;
        public bool PortFromArgs { get; private set; }
        public int ChannelCount { get; private set; } = 1;

        // Echoed from the recorder (spec v1.3). "single" = one process serves
        // channel_count channels; "multi" = one process per channel. Accepted so
        // the recorder can pass mode=single without the exe rejecting the arg.
        public string Mode { get; private set; } = "multi";

        // Detector requested by the recorder. This reference wrapper only
        // implements Motion, so the value is accepted for protocol compatibility
        // and otherwise ignored.
        public string Detector { get; private set; } = "motion";

        public static bool TryParse(string[] args, out CommandLineArgs parsed, out string error)
        {
            parsed = new CommandLineArgs();
            error = null;

            if (args == null) return true;

            foreach (string raw in args)
            {
                int eq = raw.IndexOf('=');
                if (eq <= 0)
                {
                    error = $"expected key=value, got '{raw}'";
                    return false;
                }
                string key = raw.Substring(0, eq).Trim().ToLowerInvariant();
                string val = raw.Substring(eq + 1).Trim();

                switch (key)
                {
                    case "port":
                        if (!int.TryParse(val, out int p) || p <= 0 || p > 65535)
                        {
                            error = $"invalid port: {val}";
                            return false;
                        }
                        parsed.Port = p;
                        parsed.PortFromArgs = true;
                        break;

                    case "channel_count":
                        if (!int.TryParse(val, out int cc) || cc < 1)
                        {
                            error = $"invalid channel_count: {val}";
                            return false;
                        }
                        parsed.ChannelCount = cc;
                        break;

                    case "mode":
                        {
                            string m = val.ToLowerInvariant();
                            if (m != "single" && m != "multi")
                            {
                                error = $"invalid mode: {val} (expected single|multi)";
                                return false;
                            }
                            parsed.Mode = m;
                        }
                        break;

                    case "detector":
                        {
                            // Motion-only reference wrapper: accept the known values
                            // for compatibility but do not act on them.
                            string d = val.ToLowerInvariant();
                            if (d != "motion" && d != "objectdetection" && d != "objdetection" && d != "person")
                            {
                                error = $"invalid detector: {val}";
                                return false;
                            }
                            parsed.Detector = d;
                        }
                        break;

                    default:
                        error = $"unknown key: {key}";
                        return false;
                }
            }

            return true;
        }

        public static string Usage()
        {
            return "Usage: GenericAI.exe [port=<int>] [channel_count=<N>]" +
                   " [mode=single|multi] [detector=motion]" +
                   "\n(this reference wrapper is Motion-only; mode/detector are accepted for compatibility)";
        }
    }
}
