using System.Diagnostics;
using System.Text;

namespace EmmcScan;

/// <summary>
/// adb 命令执行器：每次调用带独立超时，超时杀进程（adb 客户端自身无超时，会永久挂起）。
/// </summary>
internal static class Adb
{
    public sealed class Result
    {
        public int ExitCode;
        public string Stdout = "";
        public string Stderr = "";
        public bool TimedOut;
        public int DurationMs;
        public bool Ok => ExitCode == 0 && !TimedOut;
    }

    public static Result Run(string adbPath, string[] args, int timeoutMs)
    {
        var sw = Stopwatch.StartNew();
        var result = new Result();
        try
        {
            var psi = new ProcessStartInfo
            {
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8,
            };
            // .NET 无法直接启动 .bat/.cmd，需要经 cmd.exe 转发
            if (adbPath.EndsWith(".bat", StringComparison.OrdinalIgnoreCase) ||
                adbPath.EndsWith(".cmd", StringComparison.OrdinalIgnoreCase))
            {
                psi.FileName = Path.Combine(Environment.SystemDirectory, "cmd.exe");
                psi.ArgumentList.Add("/d");
                psi.ArgumentList.Add("/c");
                psi.ArgumentList.Add(adbPath);
                foreach (var a in args) psi.ArgumentList.Add(a);
            }
            else
            {
                psi.FileName = adbPath;
                foreach (var a in args) psi.ArgumentList.Add(a);
            }

            using var proc = Process.Start(psi);
            if (proc == null)
            {
                result.Stderr = "failed to start adb process";
                return result;
            }
            var stdoutTask = proc.StandardOutput.ReadToEndAsync();
            var stderrTask = proc.StandardError.ReadToEndAsync();
            if (!proc.WaitForExit(timeoutMs))
            {
                try { proc.Kill(true); } catch { /* 进程可能已退出 */ }
                result.TimedOut = true;
                result.DurationMs = (int)sw.ElapsedMilliseconds;
                return result;
            }
            // WaitForExit(true) 语义：等异步输出排空
            try { Task.WaitAll(new[] { stdoutTask, stderrTask }, 3000); } catch { }
            result.ExitCode = proc.ExitCode;
            result.Stdout = stdoutTask.Status == TaskStatus.RanToCompletion ? stdoutTask.Result : "";
            result.Stderr = stderrTask.Status == TaskStatus.RanToCompletion ? stderrTask.Result : "";
        }
        catch (Exception ex)
        {
            result.Stderr = ex.Message;
        }
        result.DurationMs = (int)sw.ElapsedMilliseconds;
        if (Environment.GetEnvironmentVariable("EMMC_DEBUG") == "1")
        {
            Console.Error.WriteLine($"[adb] {adbPath} {string.Join(' ', args)} -> rc={result.ExitCode} timeout={result.TimedOut} out=\"{result.Stdout.Trim()}\" err=\"{result.Stderr.Trim()}\"");
        }
        return result;
    }

    public static string Shell(string adbPath, string serial, string command, int timeoutMs)
    {
        // adb 客户端会把 shell 后面的所有参数拼接成一条远端命令；这里拆开传，
        // 既兼容真实 adb（拼接语义不变），也让测试替身能按参数位置解析。
        var parts = new List<string> { "-s", serial, "shell" };
        parts.AddRange(command.Split(' ', StringSplitOptions.RemoveEmptyEntries));
        var r = Run(adbPath, parts.ToArray(), timeoutMs);
        return r.Stdout;
    }
}
