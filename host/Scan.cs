using System.Net;
using System.Net.Sockets;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace EmmcScan;

internal sealed class DeviceReport
{
    [JsonPropertyName("device")] public string Device { get; set; } = "";
    [JsonPropertyName("node")] public string Node { get; set; } = "";
    [JsonPropertyName("type")] public string Type { get; set; } = "";
    [JsonPropertyName("manfid")] public string Manfid { get; set; } = "";
    [JsonPropertyName("manufacturer")] public string Manufacturer { get; set; } = "";
    [JsonPropertyName("name")] public string Name { get; set; } = "";
    [JsonPropertyName("oemid")] public string Oemid { get; set; } = "";
    [JsonPropertyName("serial")] public string Serial { get; set; } = "";
    [JsonPropertyName("date")] public string Date { get; set; } = "";
    [JsonPropertyName("cid")] public string Cid { get; set; } = "";
    [JsonPropertyName("csd")] public string Csd { get; set; } = "";
    [JsonPropertyName("capacity_bytes")] public long CapacityBytes { get; set; } = -1;
    [JsonPropertyName("ext_csd_rev")] public int ExtCsdRev { get; set; } = -1;
    [JsonPropertyName("mmc_spec")] public string MmcSpec { get; set; } = "";
    [JsonPropertyName("pre_eol")] public int PreEol { get; set; } = -1;
    [JsonPropertyName("life_a")] public int LifeA { get; set; } = -1;
    [JsonPropertyName("life_b")] public int LifeB { get; set; } = -1;
    [JsonPropertyName("source")] public string Source { get; set; } = "";
    [JsonPropertyName("verdict")] public string Verdict { get; set; } = "unknown";
    [JsonPropertyName("error")] public string Error { get; set; } = "";
    [JsonPropertyName("os_error")] public string OsError { get; set; } = "";
}

internal sealed class DeviceScanResult
{
    public string Ip { get; set; } = "";
    public int Port;
    public string PhoneModel { get; set; } = "";
    public string Abi { get; set; } = "";
    public string RootNote { get; set; } = "";
    public string Status { get; set; } = "";   // 中文状态：连接失败 / 离线 / 未授权 / 扫描成功 ...
    public bool Ok;
    public DeviceReport? Report { get; set; }
    public string RawJson { get; set; } = "";
    public string ErrorDetail { get; set; } = "";
}

/// <summary>网段扫描 + 单台设备的 adb 编排流水线。</summary>
internal static class Scan
{
    // ---------- 本机网段探测 ----------

    public static List<string> DetectLocalSubnets()
    {
        var subnets = new SortedSet<string>(Comparer<string>.Create(IpOrderCompare));
        foreach (var ni in System.Net.NetworkInformation.NetworkInterface.GetAllNetworkInterfaces())
        {
            if (ni.OperationalStatus != System.Net.NetworkInformation.OperationalStatus.Up) continue;
            if (ni.NetworkInterfaceType == System.Net.NetworkInformation.NetworkInterfaceType.Loopback) continue;
            foreach (var ua in ni.GetIPProperties().UnicastAddresses)
            {
                if (ua.Address.AddressFamily != AddressFamily.InterNetwork) continue;
                var ip = ua.Address;
                var bytes = ip.GetAddressBytes();
                if (bytes[0] == 127) continue;      // loopback
                if (bytes[0] == 169 && bytes[1] == 254) continue; // APIPA
                subnets.Add($"{bytes[0]}.{bytes[1]}.{bytes[2]}.0/24");
            }
        }
        return subnets.ToList();
    }

    public static int IpOrderCompare(string a, string b)
    {
        var pa = ParseIpOrdinal(a);
        var pb = ParseIpOrdinal(b);
        for (int i = 0; i < 4; i++)
        {
            if (pa[i] != pb[i]) return pa[i].CompareTo(pb[i]);
        }
        return string.CompareOrdinal(a, b);
    }

    private static int[] ParseIpOrdinal(string s)
    {
        // 支持 "1.2.3.4" 与 "1.2.3.0/24"，取 IP 部分比较
        var ipPart = s.Split('/')[0];
        var parts = ipPart.Split('.');
        var result = new int[4];
        for (int i = 0; i < 4; i++)
        {
            result[i] = i < parts.Length && int.TryParse(parts[i], out var v) ? v : 0;
        }
        return result;
    }

    /// <summary>CIDR 或 a.b.c. 前缀 展开 IP 列表。仅支持 /24 与 /32。</summary>
    public static List<string> ExpandSubnet(string subnet, int port)
    {
        subnet = subnet.Trim();
        var ips = new List<string>();
        if (subnet.EndsWith(".0/24"))
        {
            var prefix = subnet.Substring(0, subnet.Length - 5); // a.b.c
            for (int i = 1; i < 255; i++) ips.Add($"{prefix}.{i}");
        }
        else if (subnet.EndsWith("/32") && IPAddress.TryParse(subnet.Substring(0, subnet.Length - 3), out _))
        {
            ips.Add(subnet.Substring(0, subnet.Length - 3));
        }
        else if (IPAddress.TryParse(subnet, out var single))
        {
            ips.Add(single.ToString());
        }
        else if (subnet.EndsWith("."))
        {
            // "77.77.0." 前缀写法 → /24
            for (int i = 1; i < 255; i++) ips.Add($"{subnet}{i}");
        }
        return ips;
    }

    // ---------- 端口探测 ----------

    public static List<string> SweepPort(List<string> ips, int port, int timeoutMs,
        Action<int, int, int>? progress)
    {
        var found = new System.Collections.Concurrent.ConcurrentBag<string>();
        int done = 0;
        int total = ips.Count;
        Parallel.ForEach(ips, new ParallelOptions { MaxDegreeOfParallelism = 256 }, ip =>
        {
            if (PortOpen(ip, port, timeoutMs)) found.Add(ip);
            int d = Interlocked.Increment(ref done);
            progress?.Invoke(d, total, found.Count);
        });
        return found.OrderBy(ip => ip, Comparer<string>.Create(IpOrderCompare)).ToList();
    }

    public static bool PortOpen(string ip, int port, int timeoutMs)
    {
        using var client = new TcpClient();
        try
        {
            var connectTask = client.ConnectAsync(ip, port);
            if (!connectTask.Wait(timeoutMs)) return false;
            return client.Connected;
        }
        catch
        {
            return false;
        }
    }

    // ---------- 单台设备流水线 ----------

    private static string Clean(string s) => (s ?? "").Replace("\r", "").Replace("\n", " ").Trim();

    public static DeviceScanResult ScanDevice(string adbPath, string ip, int port,
        string binV7a, string binV8a, bool keepRemote)
    {
        var r = new DeviceScanResult { Ip = ip, Port = port };
        string serial = $"{ip}:{port}";
        string remote = "/data/local/tmp/emmc_health";

        // 1) connect
        var conn = Adb.Run(adbPath, new[] { "connect", serial }, 8000);
        var connText = Clean(conn.Stdout + " " + conn.Stderr);
        if (conn.TimedOut)
        {
            r.Status = "连接超时";
            return r;
        }
        if (connText.Contains("failed", StringComparison.OrdinalIgnoreCase) ||
            connText.Contains("cannot", StringComparison.OrdinalIgnoreCase))
        {
            r.Status = "连接失败";
            r.ErrorDetail = connText;
            return r;
        }

        // 2) get-state（adb 客户端本地命令，不带 shell；隧道下 adbd 可能未就绪，空响应等 2s 重试一次）
        var state = Clean(Adb.Run(adbPath, new[] { "-s", serial, "get-state" }, 8000).Stdout);
        if (state.Length == 0)
        {
            Thread.Sleep(2000);
            state = Clean(Adb.Run(adbPath, new[] { "-s", serial, "get-state" }, 8000).Stdout);
        }
        if (state == "unauthorized") { r.Status = "未授权"; return r; }
        if (state == "offline") { r.Status = "离线"; return r; }
        if (state != "device") { r.Status = "状态异常(" + (state.Length > 0 ? state : "无响应") + ")"; return r; }

        // 3) 型号与 ABI
        r.PhoneModel = Clean(Adb.Shell(adbPath, serial, "getprop ro.product.model", 6000));
        if (r.PhoneModel.Length == 0)
            r.PhoneModel = Clean(Adb.Shell(adbPath, serial, "getprop ro.product.device", 6000));
        r.Abi = Clean(Adb.Shell(adbPath, serial, "getprop ro.product.cpu.abi", 6000));

        // 4) 推送设备端工具（慢盒子/新开的 adbd 推送可能很慢，放宽超时）
        string bin = r.Abi.Contains("arm64") ? binV8a : binV7a;
        var push = Adb.Run(adbPath, new[] { "-s", serial, "push", bin, remote }, 60000);
        if (!push.Ok)
        {
            r.Status = "推送失败";
            r.ErrorDetail = Clean(push.Stderr);
            return r;
        }
        Adb.Shell(adbPath, serial, "chmod 755 " + remote, 5000);

        try
        {
            // 5) root 通道探测
            string rootProbe = Clean(Adb.Shell(adbPath, serial, "su 0 id -u", 6000));
            bool su0 = rootProbe == "0";
            if (!su0)
            {
                Adb.Run(adbPath, new[] { "-s", serial, "root" }, 8000);
                rootProbe = Clean(Adb.Shell(adbPath, serial, "id -u", 6000));
                bool adbRoot = rootProbe == "0";
                r.RootNote = adbRoot ? "adb root" : "无 root";
            }
            else
            {
                r.RootNote = "su 0";
            }

            // 6) 执行扫描（命令按参数拆开传；adb 客户端会拼接，语义不变）
            string command = su0 ? $"su 0 {remote} --json" : $"{remote} --json";
            var scanArgs = new List<string> { "-s", serial, "shell" };
            scanArgs.AddRange(command.Split(' ', StringSplitOptions.RemoveEmptyEntries));
            var scan = Adb.Run(adbPath, scanArgs.ToArray(), 45000);
            string output = Clean(scan.Stdout);
            if (output.Length == 0 && scan.Stderr.Trim().Length > 0)
                output = Clean(scan.Stderr);
            if (output.Length == 0)
            {
                r.Status = "设备无输出";
                r.ErrorDetail = scan.TimedOut ? "执行超时" : Clean(scan.Stderr);
                return r;
            }

            // 7) 解析 JSON（容忍 adb 的 CRLF 与其他行输出）
            var report = ParseDeviceJson(output, out string rawJson);
            r.RawJson = rawJson;
            if (report == null)
            {
                r.Status = "输出解析失败";
                r.ErrorDetail = output.Length > 200 ? output.Substring(0, 200) : output;
                return r;
            }
            r.Report = report;
            r.Ok = true;
            r.Status = "扫描成功";
            return r;
        }
        finally
        {
            // 8) 清理远端临时文件
            if (!keepRemote)
                Adb.Shell(adbPath, serial, "rm -f " + remote, 5000);
        }
    }

    public static DeviceReport? ParseDeviceJson(string output, out string rawJson)
    {
        rawJson = output;
        int start = output.IndexOf('{');
        int end = output.LastIndexOf('}');
        if (start < 0 || end <= start) return null;
        var json = output.Substring(start, end - start + 1);
        try
        {
            var report = JsonSerializer.Deserialize<DeviceReport>(json);
            rawJson = json;
            return report;
        }
        catch
        {
            return null;
        }
    }
}
