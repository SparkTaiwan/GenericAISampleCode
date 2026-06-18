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

        // --- ZMQ transport (opt-in). frame_endpoint set => frames arrive as NAL
        // over ZMQ (decoded internally) instead of MMF; result_endpoint set =>
        // results are PUSHed over ZMQ instead of HTTP POSTed. ---
        public string FrameEndpoint { get; private set; } = "";
        public bool UseZmqFrames => !string.IsNullOrEmpty(FrameEndpoint);
        public string ResultEndpoint { get; private set; } = "";
        public bool UseZmqResults => !string.IsNullOrEmpty(ResultEndpoint);

        // HTTP control-plane bind host (/Alive,/SetParameters). Default loopback;
        // for a REMOTE recorder set the machine's reachable IP or "+".
        public string HttpHost { get; private set; } = "127.0.0.1";

        // Shorthand: server_ip + result_port + stream_port expand into the two
        // endpoints (result=tcp://server_ip:result_port, frame=tcp://server_ip:stream_port),
        // matching the recorder's http_server_port / ai_stream_port respectively.
        public string ServerIp { get; private set; } = "";
        public int ResultPort { get; private set; } = 0;
        public int StreamPort { get; private set; } = 0;

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

                    case "frame_endpoint":
                        if (!val.Contains("://")) { error = $"invalid frame_endpoint: {val} (expected tcp://host:port)"; return false; }
                        parsed.FrameEndpoint = val;
                        break;

                    case "result_endpoint":
                        if (!val.Contains("://")) { error = $"invalid result_endpoint: {val} (expected tcp://host:port)"; return false; }
                        parsed.ResultEndpoint = val;
                        break;

                    case "http_host":
                        if (string.IsNullOrEmpty(val)) { error = "invalid http_host: empty (expected an IP or +)"; return false; }
                        parsed.HttpHost = val;
                        break;

                    case "server_ip":
                        if (string.IsNullOrEmpty(val)) { error = "invalid server_ip: empty (expected the recorder's IP)"; return false; }
                        parsed.ServerIp = val;
                        break;

                    case "result_port":
                        if (!int.TryParse(val, out int rp) || rp <= 0 || rp > 65535) { error = $"invalid result_port: {val}"; return false; }
                        parsed.ResultPort = rp;
                        break;

                    case "stream_port":
                        if (!int.TryParse(val, out int stp) || stp <= 0 || stp > 65535) { error = $"invalid stream_port: {val}"; return false; }
                        parsed.StreamPort = stp;
                        break;

                    default:
                        error = $"unknown key: {key}";
                        return false;
                }
            }

            // server_ip + result_port + stream_port expand into the two endpoints.
            if (!string.IsNullOrEmpty(parsed.ServerIp) || parsed.ResultPort > 0 || parsed.StreamPort > 0)
            {
                if (string.IsNullOrEmpty(parsed.ServerIp) || parsed.ResultPort <= 0 || parsed.StreamPort <= 0)
                {
                    error = "server_ip, result_port and stream_port must be given together";
                    return false;
                }
                if (string.IsNullOrEmpty(parsed.ResultEndpoint))
                    parsed.ResultEndpoint = $"tcp://{parsed.ServerIp}:{parsed.ResultPort}";
                if (string.IsNullOrEmpty(parsed.FrameEndpoint))
                    parsed.FrameEndpoint = $"tcp://{parsed.ServerIp}:{parsed.StreamPort}";
            }

            return true;
        }

        public static string Usage()
        {
            return "Usage: GenericAI.exe [port=<int>] [channel_count=<N>] [mode=single|multi] [detector=motion]" +
                   " [server_ip=<recorderIP> result_port=<port> stream_port=<port>] [http_host=<ip|+>]" +
                   "\n  Motion-only reference wrapper. server_ip+result_port+stream_port switch frames/results to ZMQ" +
                   " (frame=NAL decoded internally, result=PUSH); http_host sets the /Alive,/SetParameters bind" +
                   " address (default 127.0.0.1; use the machine IP or + for a remote recorder).";
        }
    }
}
