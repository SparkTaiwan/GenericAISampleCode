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
        public int EncodeWorkers { get; private set; } = 2;
        public int SendWorkers { get; private set; } = 2;

        // Process mode echoed from the recorder (spec v1.3). "single" = this one
        // process serves channel_count channels on consecutive ports; "multi" =
        // one process per channel (channel_count stays 1). Informational here —
        // channel_count already drives the actual topology — but accepted so the
        // recorder can pass `mode=single` per spec without the exe rejecting it.
        public string Mode { get; private set; } = "multi";

        // Detector backend chosen at runtime: 0 = Motion, 1 = Person (object
        // detection). -1 means "not specified" -> native falls back to the
        // compile-time default in gai_config.h.
        public int DetectorKind { get; private set; } = -1;

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

                    case "encode_workers":
                        if (!int.TryParse(val, out int ew) || ew < 1 || ew > 16)
                        {
                            error = $"invalid encode_workers: {val}";
                            return false;
                        }
                        parsed.EncodeWorkers = ew;
                        break;

                    case "send_workers":
                        if (!int.TryParse(val, out int sw) || sw < 1 || sw > 16)
                        {
                            error = $"invalid send_workers: {val}";
                            return false;
                        }
                        parsed.SendWorkers = sw;
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
                        switch (val.ToLowerInvariant())
                        {
                            case "motion":
                                parsed.DetectorKind = (int)DetectorType.Motion;
                                break;
                            case "person":
                            case "objectdetection":
                            case "objdetection":
                                parsed.DetectorKind = (int)DetectorType.Person;
                                break;
                            default:
                                error = $"invalid detector: {val} (expected motion|objectdetection)";
                                return false;
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
                   " [mode=single|multi] [detector=motion|objectdetection]" +
                   " [encode_workers=<N>] [send_workers=<M>]" +
                   "\n(detector defaults to the compile-time flag in GenericAI.Native/gai_config.h when omitted)";
        }
    }
}
