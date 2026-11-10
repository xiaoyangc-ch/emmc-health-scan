using System.Reflection;
using System.Text;

namespace EmmcScan;

internal static class Program
{
    public const string Version = "1.0.0";
    private const int ExitOk = 0;
    private const int ExitUsage = 1;
    private const int ExitNoneFound = 2;
    private const int ExitAllFailed = 3;

    private static int Main(string[] args)
    {
        try { Console.OutputEncoding = Encoding.UTF8; } catch { }
        if (args.Any(a => a is "-h" or "--help" or "help"))
        {
            PrintHelp();
            return ExitOk;
        }
        var opt = ParseArgs(args);
        if (opt == null) return ExitUsage;

        Console.WriteLine($"eMMC健康扫描 v{Version}");
        Console.WriteLine();

        // ---- 资源准备（内置 adb 与设备端工具）----
        string adbPath;
        string binV7a, binV8a;
        try
        {
            (adbPath, binV7a, binV8a) = PrepareResources(opt.AdbOverride);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[错误] 内置资源准备失败：{ex.Message}");
            return ExitUsage;
        }

        // ---- 目标 IP 集合 ----
        List<string> targets;
        var sw = System.Diagnostics.Stopwatch.StartNew();
        var hostPorts = new Dictionary<string, int>();
        if (opt.Hosts.Count > 0)
        {
            targets = new List<string>();
            foreach (var h in opt.Hosts)
            {
                // 支持 ip:port 写法：端口作为该目标的 --port 覆盖不做全局替换，直接展开为单台扫描
                var parts = h.Split(':');
                if (parts.Length == 2 && int.TryParse(parts[1], out var p))
                {
                    hostPorts[parts[0]] = p;
                    targets.Add(parts[0]);
                }
                else
                {
                    hostPorts[h] = opt.Port;
                    targets.Add(h);
                }
            }
            Console.WriteLine($"跳过网段扫描，直接探测 {targets.Count} 个目标");
        }
        else
        {
            var subnets = opt.Subnets.Count > 0 ? opt.Subnets : DetectSubnets();
            if (subnets.Count == 0)
            {
                Console.WriteLine("[错误] 未探测到本机 IPv4 网段，请用 --subnet 指定，例如 --subnet 77.77.0.0/24");
                return ExitUsage;
            }
            Console.WriteLine($"扫描网段: {string.Join("  ", subnets)}");
            var ips = new List<string>();
            foreach (var s in subnets) ips.AddRange(Scan.ExpandSubnet(s, opt.Port));
            ips = ips.Distinct().ToList();
            Console.WriteLine($"共 {ips.Count} 个地址，端口 {opt.Port}，并发探测中...");
            int shown = -1;
            targets = Scan.SweepPort(ips, opt.Port, opt.TimeoutMs, (done, total, found) =>
            {
                int step = done * 10 / Math.Max(total, 1);
                // 多线程回调：CAS 保证进度只按单调递增顺序打印
                while (true)
                {
                    int prev = Volatile.Read(ref shown);
                    if (step <= prev) break;
                    if (Interlocked.CompareExchange(ref shown, step, prev) == prev)
                    {
                        Console.Write($"\r  进度 {step * 10}%   已发现 {found} 台 ");
                        break;
                    }
                }
            });
            Console.WriteLine($"\r  探测完成，发现 {targets.Count} 台设备          ");
        }
        Console.WriteLine();

        if (targets.Count == 0)
        {
            Console.WriteLine("网段内未发现 adb 设备（端口未开放）。");
            Console.WriteLine("提示：确认盒子已开启网络 ADB；或用 --hosts / --subnet 指定范围。");
            return ExitNoneFound;
        }

        // ---- 逐台扫描（并行 4）----
        Console.WriteLine($"开始扫描 {targets.Count} 台设备（并发 4）...");
        var results = new System.Collections.Concurrent.ConcurrentBag<DeviceScanResult>();
        int finished = 0;
        int totalN = targets.Count;
        Parallel.ForEach(targets, new ParallelOptions { MaxDegreeOfParallelism = 4 }, ip =>
        {
            int port = hostPorts.TryGetValue(ip, out var p) ? p : opt.Port;
            var r = Scan.ScanDevice(adbPath, ip, port, binV7a, binV8a, opt.KeepRemote);
            results.Add(r);
            int n = Interlocked.Increment(ref finished);
            var old = Console.ForegroundColor;
            if (!opt.NoColor && r.Ok && r.Report != null)
            {
                var (fg, _) = Render.VerdictColor(r.Report.Verdict);
                Console.ForegroundColor = fg;
            }
            Console.WriteLine($"  [{n}/{totalN}] {r.Ip}:{port}  {r.Status}" +
                              (r.Ok && r.Report != null ? $"  结论={Render.VerdictText(r.Report.Verdict)}" : ""));
            Console.ForegroundColor = old;
        });
        sw.Stop();

        var list = results.ToList();
        Render.PrintResults(list, opt.NoColor);

        // ---- 汇总 ----
        int ok = list.Count(x => x.Ok);
        var counts = list.Where(x => x.Ok).GroupBy(x => x.Report?.Verdict)
            .ToDictionary(g => g.Key ?? "", g => g.Count());
        Console.WriteLine();
        Console.WriteLine($"完成: 成功 {ok}/{list.Count}，耗时 {sw.ElapsedMilliseconds / 1000.0:F1}s");
        if (counts.Count > 0)
        {
            Console.WriteLine("结论分布: " + string.Join("   ", counts.Select(kv =>
            {
                var (fg, _) = Render.VerdictColor(kv.Key);
                var old = Console.ForegroundColor;
                if (!opt.NoColor) Console.ForegroundColor = fg;
                var text = $"{Render.VerdictText(kv.Key)} {kv.Value} 台";
                Console.ForegroundColor = old;
                return text;
            })));
            var urgent = counts.GetValueOrDefault("urgent", 0) + counts.GetValueOrDefault("warning", 0);
            if (urgent > 0)
                Console.WriteLine($"注意: 有 {urgent} 台设备 eMMC 存在警告或紧急状态，建议尽快处理。");
        }

        // ---- 报告 ----
        if (!opt.NoReport)
        {
            string outDir = Path.Combine(opt.Out, $"eMMC扫描_{DateTime.Now:yyyyMMdd_HHmmss}");
            Render.WriteReports(outDir, list, opt.Port, sw.ElapsedMilliseconds);
            Console.WriteLine($"报告已保存: {outDir}");
        }

        // 双击运行时停留窗口查看结果；输出被重定向（脚本/测试）时不停留
        if (!Console.IsOutputRedirected && !opt.NoPause)
        {
            Console.Write("按回车键退出...");
            try { Console.ReadLine(); } catch { }
        }

        return ok > 0 ? ExitOk : ExitAllFailed;
    }

    // ---------- 参数 ----------

    private sealed class Options
    {
        public List<string> Subnets = new();
        public List<string> Hosts = new();
        public int Port = 5555;
        public int TimeoutMs = 400;
        public string AdbOverride = "";
        public string Out = "";
        public bool KeepRemote;
        public bool NoColor;
        public bool NoReport;
        public bool NoPause;
    }

    private static void PrintHelp()
    {
        Console.WriteLine($"eMMC健康扫描 v{Version} — 扫描整个网段内的 Android 设备并读取 eMMC 健康度");
        Console.WriteLine();
        Console.WriteLine("用法: eMMC健康扫描.exe [选项]");
        Console.WriteLine("  --subnet CIDR    指定扫描网段（如 77.77.0.0/24 或 192.168.1.），可重复");
        Console.WriteLine("                   默认：自动探测本机所有网卡的 /24 网段");
        Console.WriteLine("  --hosts a,b,c    跳过网段扫描，直接扫描指定 IP 列表");
        Console.WriteLine("  --port N         adb 端口（默认 5555）");
        Console.WriteLine("  --timeout-ms N   端口探测超时毫秒（默认 400）");
        Console.WriteLine("  --adb PATH       指定 adb.exe 路径（默认使用内置 adb）");
        Console.WriteLine("  --out DIR        报告输出目录（默认：exe 所在目录下 reports）");
        Console.WriteLine("  --keep-remote    保留设备上的临时文件（调试用）");
        Console.WriteLine("  --no-report      不生成报告文件");
        Console.WriteLine("  --no-color       关闭彩色输出");
        Console.WriteLine("  --no-pause       结束后不停留（脚本调用用）");
        Console.WriteLine("  -h / --help      显示本帮助");
        Console.WriteLine();
        Console.WriteLine("说明：健康字段（预留块/寿命）需要设备 root（su 0 或 adb root）才能读取；");
        Console.WriteLine("      无 root 时仅展示 eMMC 身份信息（型号/厂商/容量/CID）。");
    }

    private static Options? ParseArgs(string[] args)
    {
        var opt = new Options();
        for (int i = 0; i < args.Length; i++)
        {
            string a = args[i];
            switch (a)
            {
                case "-h":
                case "--help":
                case "help":
                    PrintHelp();
                    return null;
                case "--subnet":
                    if (i + 1 >= args.Length) { PrintHelp(); return null; }
                    opt.Subnets.Add(args[++i]);
                    break;
                case "--hosts":
                    if (i + 1 >= args.Length) { PrintHelp(); return null; }
                    opt.Hosts.AddRange(args[++i].Split(',', ';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries));
                    break;
                case "--port":
                    if (i + 1 >= args.Length || !int.TryParse(args[++i], out opt.Port)) { PrintHelp(); return null; }
                    break;
                case "--timeout-ms":
                    if (i + 1 >= args.Length || !int.TryParse(args[++i], out opt.TimeoutMs)) { PrintHelp(); return null; }
                    break;
                case "--adb":
                    if (i + 1 >= args.Length) { PrintHelp(); return null; }
                    opt.AdbOverride = args[++i];
                    break;
                case "--out":
                    if (i + 1 >= args.Length) { PrintHelp(); return null; }
                    opt.Out = args[++i];
                    break;
                case "--keep-remote": opt.KeepRemote = true; break;
                case "--no-report": opt.NoReport = true; break;
                case "--no-color": opt.NoColor = true; break;
                case "--no-pause": opt.NoPause = true; break;
                default:
                    Console.WriteLine($"未知参数: {a}");
                    PrintHelp();
                    return null;
            }
        }
        if (string.IsNullOrEmpty(opt.Out))
        {
            string exeDir = AppContext.BaseDirectory;
            opt.Out = Path.Combine(exeDir, "reports");
        }
        return opt;
    }

    // ---------- 内置资源 ----------

    private static (string adb, string v7a, string v8a) PrepareResources(string adbOverride)
    {
        string v7a, v8a;
        if (!string.IsNullOrEmpty(adbOverride))
        {
            v7a = ExtractEmbedded(ResName("device.emmc_health_v7a"), "emmc_health_v7a");
            v8a = ExtractEmbedded(ResName("device.emmc_health_v8a"), "emmc_health_v8a");
            return (adbOverride, v7a, v8a);
        }
        string dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "EmmcScan", Version);
        string adbDir = Path.Combine(dir, "adb");
        Directory.CreateDirectory(adbDir);
        string adb = ExtractTo(ResName("adb.adb.exe"), Path.Combine(adbDir, "adb.exe"));
        ExtractTo(ResName("adb.AdbWinApi.dll"), Path.Combine(adbDir, "AdbWinApi.dll"));
        ExtractTo(ResName("adb.AdbWinUsbApi.dll"), Path.Combine(adbDir, "AdbWinUsbApi.dll"));
        string deviceDir = Path.Combine(dir, "device");
        Directory.CreateDirectory(deviceDir);
        v7a = ExtractTo(ResName("device.emmc_health_v7a"), Path.Combine(deviceDir, "emmc_health_v7a"));
        v8a = ExtractTo(ResName("device.emmc_health_v8a"), Path.Combine(deviceDir, "emmc_health_v8a"));
        return (adb, v7a, v8a);
    }

    /// <summary>按后缀解析嵌入资源全名（默认逻辑名带 RootNamespace 前缀，避免依赖具体值）。</summary>
    private static string ResName(string suffix)
    {
        var asm = Assembly.GetExecutingAssembly();
        return asm.GetManifestResourceNames()
                   .FirstOrDefault(n => n.EndsWith(suffix, StringComparison.OrdinalIgnoreCase))
               ?? throw new InvalidOperationException($"嵌入资源缺失: {suffix}");
    }

    private static string ExtractEmbedded(string resourceName, string fileName)
    {
        string dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "EmmcScan", Version, "device");
        Directory.CreateDirectory(dir);
        return ExtractTo(resourceName, Path.Combine(dir, fileName));
    }

    private static string ExtractTo(string resourceName, string targetPath)
    {
        var asm = Assembly.GetExecutingAssembly();
        using var stream = asm.GetManifestResourceStream(resourceName)
            ?? throw new InvalidOperationException($"嵌入资源缺失: {resourceName}");
        var existing = new FileInfo(targetPath);
        if (existing.Exists && existing.Length == stream.Length) return targetPath; // 已就位
        var tmp = targetPath + ".tmp";
        using (var fs = File.Create(tmp)) stream.CopyTo(fs);
        try
        {
            File.Delete(targetPath);
        }
        catch
        {
            // 目标可能被占用（adb server），先改名再替换
            try { File.Move(targetPath, targetPath + ".old", true); } catch { }
        }
        File.Move(tmp, targetPath, true);
        return targetPath;
    }

    private static List<string> DetectSubnets() => Scan.DetectLocalSubnets();
}
