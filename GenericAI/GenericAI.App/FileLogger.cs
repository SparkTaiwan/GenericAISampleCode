using System;
using System.IO;
using System.Text;

namespace GenericAI.App
{
    internal static class FileLogger
    {
        private const long MaxFileBytes = 5 * 1024 * 1024;
        private const int  MaxBackupFiles = 3;

        public static bool Enabled = false;

        private static readonly object _lock = new object();
        private static string _logPath      = BuildFallbackLogPath("GenericAI.log");
        private static string _errorLogPath = BuildFallbackLogPath("error.log");

        // Only selects the path - the directory itself is created lazily on first write,
        // so a disabled logger never leaves an empty folder behind.
        public static void Init(int port, string explicitDir = null)
        {
            lock (_lock)
            {
                if (!string.IsNullOrEmpty(explicitDir)) { UseDirectory(explicitDir); return; }
                if (TryUseDirectory(@"D:\SLog-" + port)) return;
                if (TryUseDirectory(@"F:\SLog-" + port)) return;
                // Both preferred drives unavailable - keep LocalApplicationData fallback.
            }
        }

        private static bool TryUseDirectory(string dir)
        {
            try
            {
                // Only accept the target if its drive exists; defer creating the folder
                // until something is actually written.
                if (!Directory.Exists(Path.GetPathRoot(dir))) return false;
                UseDirectory(dir);
                return true;
            }
            catch
            {
                return false;
            }
        }

        private static void UseDirectory(string dir)
        {
            _logPath      = Path.Combine(dir, "GenericAI.log");
            _errorLogPath = Path.Combine(dir, "error.log");
        }

        private static string BuildFallbackLogPath(string fileName)
        {
            try
            {
                string dir = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "GenericAI");
                return Path.Combine(dir, fileName);
            }
            catch
            {
                return fileName;
            }
        }

        public static void Info(string message)  { Write("INFO",  message, null); }
        public static void Warn(string message)  { Write("WARN",  message, null); }
        public static void Error(string message, Exception ex = null) { Write("ERROR", message, ex); }

        private static void Write(string level, string message, Exception ex)
        {
            if (!Enabled) return;
            try
            {
                lock (_lock)
                {
                    string line = FormatLine(level, message, ex);
                    EnsureDirectory(_logPath);
                    RotateIfNeeded(_logPath);
                    File.AppendAllText(_logPath, line, Encoding.UTF8);

                    if (level == "ERROR")
                    {
                        RotateIfNeeded(_errorLogPath);
                        File.AppendAllText(_errorLogPath, line, Encoding.UTF8);
                    }
                }
            }
            catch
            {
                // Logging must never propagate.
            }
        }

        private static string FormatLine(string level, string message, Exception ex)
        {
            StringBuilder sb = new StringBuilder(256);
            sb.Append(DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff"));
            sb.Append(" [").Append(level).Append("] ");
            sb.Append(message);
            if (ex != null) AppendException(sb, ex);
            sb.Append(Environment.NewLine);
            return sb.ToString();
        }

        private static void AppendException(StringBuilder sb, Exception ex)
        {
            Exception cur = ex;
            int depth = 0;
            while (cur != null)
            {
                sb.Append(" | ").Append(cur.GetType().Name).Append(": ").Append(cur.Message);

                AggregateException agg = cur as AggregateException;
                if (agg != null)
                {
                    foreach (Exception inner in agg.Flatten().InnerExceptions)
                    {
                        sb.Append(" | agg[").Append(depth).Append("]: ")
                          .Append(inner.GetType().Name).Append(": ").Append(inner.Message);
                    }
                    break;
                }

                cur = cur.InnerException;
                depth++;
            }

            if (!string.IsNullOrEmpty(ex.StackTrace))
            {
                sb.Append(Environment.NewLine).Append(ex.StackTrace);
            }
        }

        private static void EnsureDirectory(string path)
        {
            string dir = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(dir))
                Directory.CreateDirectory(dir);
        }

        private static void RotateIfNeeded(string path)
        {
            try
            {
                FileInfo fi = new FileInfo(path);
                if (!fi.Exists || fi.Length < MaxFileBytes) return;

                string oldest = path + "." + MaxBackupFiles;
                if (File.Exists(oldest)) File.Delete(oldest);

                for (int i = MaxBackupFiles - 1; i >= 1; i--)
                {
                    string src = path + "." + i;
                    string dst = path + "." + (i + 1);
                    if (File.Exists(src)) File.Move(src, dst);
                }
                File.Move(path, path + ".1");
            }
            catch
            {
                // If rotation fails we just keep appending.
            }
        }
    }
}
