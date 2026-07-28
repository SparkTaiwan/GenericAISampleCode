using System;
using System.Collections.Concurrent;
using System.IO;
using System.Text;
using System.Threading;

namespace GenericAI.App
{
    // Asynchronous file logger. Producers only format the line and enqueue it
    // (bounded queue, never blocks, no file I/O on the caller's thread); a
    // single background thread drains the queue in batches into long-lived
    // streams. The exe lives under C:\Program Files where writes are denied,
    // so logs go to the machine-wide app-data folder instead:
    //   %ProgramData%\Spark\GenericAI\Logs\
    //     GenericAI-<basePort>.log   INFO / WARN / ERROR
    //     error-<basePort>.log       ERROR duplicated for quick triage
    //     timing-<basePort>.log      per-frame lines from TimingRecorder
    // File names carry the base port so concurrent instances never contend
    // for the same (exclusively opened) file. The directory is created on
    // first actual write, so a disabled logger leaves no trace on disk.
    // Logging failures are swallowed: they must never crash the caller.
    internal static class FileLogger
    {
        // Runtime switch for INFO/WARN/ERROR file logging, set once at startup from
        // the "GenericAI.Config" log_to_file flag (ConsoleLog.LoadFromConfig) -- no
        // rebuild needed. Off by default. Timing(...) is gated by TimingRecorder.Enabled
        // instead, so timing capture works without turning the general log on.
        public static volatile bool Enabled = false;

        private const long MaxFileBytes = 5 * 1024 * 1024;
        private const int  MaxBackupFiles = 3;
        private const int  MaxQueuedLines = 10000;
        // Also the writer's idle wake-up; bounds how long a line can sit
        // unflushed (ERROR flushes immediately).
        private const int  FlushIntervalMs = 250;

        private struct Entry
        {
            public string Line;
            public bool IsError;
            public bool IsTiming;
        }

        private static readonly object _startLock = new object();
        private static BlockingCollection<Entry> _queue;
        private static Thread _writerThread;
        private static volatile bool _shuttingDown;
        private static long _droppedLines;

        private static string _mainPath   = BuildLogPath("GenericAI.log");
        private static string _errorPath  = BuildLogPath("error.log");
        private static string _timingPath = BuildLogPath("timing.log");

        // Call once at startup before the first log line. Only selects paths;
        // nothing touches the disk until a line is actually written.
        public static void Init(int basePort)
        {
            _mainPath   = BuildLogPath("GenericAI-" + basePort + ".log");
            _errorPath  = BuildLogPath("error-" + basePort + ".log");
            _timingPath = BuildLogPath("timing-" + basePort + ".log");
        }

        private static string BuildLogPath(string fileName)
        {
            try
            {
                return Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
                    @"Spark\GenericAI\Logs",
                    fileName);
            }
            catch
            {
                return fileName;
            }
        }

        public static void Info(string message)  { Write("INFO",  message, null); }
        public static void Warn(string message)  { Write("WARN",  message, null); }
        public static void Error(string message, Exception ex = null) { Write("ERROR", message, ex); }

        // Raw per-frame line from TimingRecorder (already enabled-checked
        // there); written verbatim so the file matches the console output.
        public static void Timing(string line)
        {
            Enqueue(new Entry { Line = line + Environment.NewLine, IsTiming = true });
        }

        // Stops accepting lines, drains what is queued, flushes and closes.
        // Bounded wait so a wedged disk cannot hold up process exit.
        public static void Shutdown()
        {
            Thread writer;
            lock (_startLock)
            {
                _shuttingDown = true;
                if (_queue == null) return;
                try { _queue.CompleteAdding(); } catch { }
                writer = _writerThread;
            }
            try { if (writer != null) writer.Join(TimeSpan.FromSeconds(2)); } catch { }
        }

        private static void Write(string level, string message, Exception ex)
        {
            if (!Enabled) return;
            try
            {
                Enqueue(new Entry
                {
                    Line = FormatLine(level, message, ex),
                    IsError = level == "ERROR",
                });
            }
            catch
            {
                // Logging must never propagate.
            }
        }

        private static void Enqueue(Entry e)
        {
            if (_shuttingDown) return;

            BlockingCollection<Entry> q = _queue;
            if (q == null)
            {
                lock (_startLock)
                {
                    if (_queue == null)
                    {
                        if (_shuttingDown) return;
                        _queue = new BlockingCollection<Entry>(MaxQueuedLines);
                        _writerThread = new Thread(WriterLoop)
                        {
                            IsBackground = true,
                            Name = "FileLogger",
                        };
                        _writerThread.Start();
                    }
                    q = _queue;
                }
            }

            try
            {
                if (!q.TryAdd(e))
                    Interlocked.Increment(ref _droppedLines);
            }
            catch (InvalidOperationException)
            {
                // CompleteAdding raced with this producer during shutdown.
            }
        }

        private static void WriterLoop()
        {
            LogFile main   = new LogFile(_mainPath);
            LogFile error  = new LogFile(_errorPath);
            LogFile timing = new LogFile(_timingPath);
            try
            {
                bool dirty = false;
                int lastFlush = Environment.TickCount;
                while (true)
                {
                    Entry e;
                    bool got;
                    try { got = _queue.TryTake(out e, FlushIntervalMs); }
                    catch { break; }

                    bool forceFlush = false;
                    if (got)
                    {
                        long drops = Interlocked.Read(ref _droppedLines);
                        if (drops > 0)
                        {
                            drops = Interlocked.Exchange(ref _droppedLines, 0);
                            // With the general log off, only timing lines flow,
                            // so the notice belongs in the timing file.
                            (Enabled ? main : timing).Write(FormatLine(
                                "WARN", $"FileLogger queue full: dropped {drops} line(s)", null));
                        }

                        if (e.IsTiming)
                        {
                            timing.Write(e.Line);
                        }
                        else
                        {
                            main.Write(e.Line);
                            if (e.IsError)
                            {
                                error.Write(e.Line);
                                error.Flush();
                                forceFlush = true;
                            }
                        }
                        dirty = true;
                    }
                    else if (_queue.IsCompleted)
                    {
                        break;
                    }

                    if (dirty && (forceFlush ||
                        unchecked(Environment.TickCount - lastFlush) >= FlushIntervalMs))
                    {
                        main.Flush();
                        timing.Flush();
                        dirty = false;
                        lastFlush = Environment.TickCount;
                    }
                }
            }
            catch
            {
                // Writer must never crash the process.
            }
            finally
            {
                main.Close();
                error.Close();
                timing.Close();
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

        // Walk the InnerException chain so root causes (SocketException, etc.)
        // are visible, and flatten AggregateException so we don't see
        // "發生一或多項錯誤" with no detail.
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

        // One log target, used from the writer thread only (no locking).
        // Opened lazily on first write; rotation is judged from a running
        // byte count instead of stat'ing the file per line.
        private sealed class LogFile
        {
            private readonly string _path;
            private StreamWriter _writer;
            private long _bytes;

            public LogFile(string path) { _path = path; }

            public void Write(string line)
            {
                try
                {
                    if (_writer == null && !Open()) return;
                    if (_bytes >= MaxFileBytes)
                    {
                        Rotate();
                        if (_writer == null && !Open()) return;
                    }
                    _writer.Write(line);
                    _bytes += Encoding.UTF8.GetByteCount(line);
                }
                catch
                {
                    // Drop the stream on any write failure; the next line
                    // retries from Open.
                    Close();
                }
            }

            public void Flush()
            {
                try { if (_writer != null) _writer.Flush(); } catch { Close(); }
            }

            public void Close()
            {
                try { if (_writer != null) _writer.Dispose(); } catch { }
                _writer = null;
            }

            private bool Open()
            {
                try
                {
                    string dir = Path.GetDirectoryName(_path);
                    if (!string.IsNullOrEmpty(dir))
                        Directory.CreateDirectory(dir);

                    // FileShare.Read so operators can tail the live log.
                    FileStream fs = new FileStream(
                        _path, FileMode.Append, FileAccess.Write, FileShare.Read);
                    _bytes = fs.Length;
                    _writer = new StreamWriter(fs, new UTF8Encoding(false));
                    return true;
                }
                catch
                {
                    Close();
                    return false;
                }
            }

            private void Rotate()
            {
                Close();
                try
                {
                    string oldest = _path + "." + MaxBackupFiles;
                    if (File.Exists(oldest)) File.Delete(oldest);

                    for (int i = MaxBackupFiles - 1; i >= 1; i--)
                    {
                        string src = _path + "." + i;
                        string dst = _path + "." + (i + 1);
                        if (File.Exists(src)) File.Move(src, dst);
                    }
                    File.Move(_path, _path + ".1");
                }
                catch
                {
                    // If rotation fails we just keep appending to the
                    // oversized file; the next threshold crossing retries.
                }
            }
        }
    }
}
