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

        // ZMQ frame plane endpoint, e.g. "tcp://127.0.0.1:5556". When set, frames
        // arrive over ZMQ (NAL -> decode) instead of MMF: the native side connects
        // a PULL socket here. Empty => MMF mode (default, unchanged).
        public string FrameEndpoint { get; private set; } = "";
        public bool UseZmqFrames => !string.IsNullOrEmpty(FrameEndpoint);

        // ZMQ result plane endpoint, e.g. "tcp://127.0.0.1:5557". When set, analytics
        // results are PUSHed over ZMQ instead of HTTP POSTed to analytics_event_api_url.
        public string ResultEndpoint { get; private set; } = "";
        public bool UseZmqResults => !string.IsNullOrEmpty(ResultEndpoint);

        // HTTP control-plane bind host for /Alive, /SetParameters, /GetLicense.
        // Default loopback (local-only). For a REMOTE wrapper the recorder must be
        // able to reach these, so pass this host the machine's reachable IP
        // (http_host=172.20.1.18) or "+"/"*" for all interfaces. Binding a
        // non-loopback host needs admin or a URL reservation (netsh http add urlacl).
        public string HttpHost { get; private set; } = "127.0.0.1";

        // Convenience shorthand that expands into the two ZMQ endpoints so a human
        // does not type two full tcp:// strings:
        //   result_endpoint = tcp://{server_ip}:{result_port}   (recorder http_server_port)
        //   frame_endpoint  = tcp://{server_ip}:{stream_port}   (recorder ai_stream_port)
        // The two ports are independent (the recorder assigns ai_stream_port
        // separately, not http_server_port+1). Explicit frame_endpoint/
        // result_endpoint (used by the recorder's own spawn) take precedence.
        public string ServerIp { get; private set; } = "";
        public int ResultPort { get; private set; } = 0;   // recorder http_server_port
        public int StreamPort { get; private set; } = 0;   // recorder ai_stream_port

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

                    case "frame_endpoint":
                        // Minimal validation; libzmq parses the full endpoint.
                        if (!val.Contains("://"))
                        {
                            error = $"invalid frame_endpoint: {val} (expected e.g. tcp://host:port)";
                            return false;
                        }
                        parsed.FrameEndpoint = val;
                        break;

                    case "result_endpoint":
                        if (!val.Contains("://"))
                        {
                            error = $"invalid result_endpoint: {val} (expected e.g. tcp://host:port)";
                            return false;
                        }
                        parsed.ResultEndpoint = val;
                        break;

                    case "http_host":
                        if (string.IsNullOrEmpty(val))
                        {
                            error = "invalid http_host: empty (expected e.g. 127.0.0.1, an interface IP, or + )";
                            return false;
                        }
                        parsed.HttpHost = val;
                        break;

                    case "server_ip":
                        if (string.IsNullOrEmpty(val))
                        {
                            error = "invalid server_ip: empty (expected the recorder's IP, e.g. 172.20.1.36)";
                            return false;
                        }
                        parsed.ServerIp = val;
                        break;

                    case "result_port":
                        if (!int.TryParse(val, out int rp) || rp <= 0 || rp > 65535)
                        {
                            error = $"invalid result_port: {val} (1..65535; the recorder's http_server_port)";
                            return false;
                        }
                        parsed.ResultPort = rp;
                        break;

                    case "stream_port":
                        if (!int.TryParse(val, out int stp) || stp <= 0 || stp > 65535)
                        {
                            error = $"invalid stream_port: {val} (1..65535; the recorder's ai_stream_port)";
                            return false;
                        }
                        parsed.StreamPort = stp;
                        break;

                    default:
                        error = $"unknown key: {key}";
                        return false;
                }
            }

            // Expand the server_ip + result_port + stream_port shorthand into the
            // two endpoints. They go together; explicit frame_endpoint/result_endpoint
            // (e.g. from the recorder's own spawn) take precedence if also present.
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
            return "Usage: GenericAI.exe port=<int> channel_count=<N> mode=single|multi" +
                   " detector=motion|objectdetection server_ip=<recorderIP>" +
                   " result_port=<port> stream_port=<port>" +
                   " [http_host=<ip|+>] [encode_workers=<N>] [send_workers=<M>]" +
                   "\n  server_ip + result_port + stream_port => result=tcp://server_ip:result_port," +
                   " frame=tcp://server_ip:stream_port. Use the recorder's http_server_port for" +
                   " result_port and its ai_stream_port for stream_port (the two are independent)." +
                   "\n  http_host = the /Alive,/SetParameters bind address (default 127.0.0.1;" +
                   " use the machine's IP or + for a remote recorder)." +
                   "\n  Advanced: frame_endpoint=tcp://host:port result_endpoint=tcp://host:port" +
                   " override the pair above; detector defaults to gai_config.h when omitted.";
        }
    }
}
