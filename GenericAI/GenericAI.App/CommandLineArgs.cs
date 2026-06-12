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
                   " [encode_workers=<N>] [send_workers=<M>]" +
                   "\n(detector type is a compile-time flag in GenericAI.Native/gai_config.h)";
        }
    }
}
