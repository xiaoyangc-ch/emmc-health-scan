using System.Text;

namespace EmmcScan;

internal static class Render
{
    // ---------- 中文对齐 ----------

    public static int Width(string s)
    {
        if (s == null) return 0;
        int w = 0;
        foreach (var c in s)
        {
            if (c >= 0x1100 && (
                    c <= 0x115F ||                      // Hangul Jamo
                    c == 0x2329 || c == 0x232A ||
                    (c >= 0x2E80 && c <= 0xA4CF) ||     // CJK 部首..Yi
                    (c >= 0xAC00 && c <= 0xD7A3) ||     // Hangul
                    (c >= 0xF900 && c <= 0xFAFF) ||     // CJK 兼容
                    (c >= 0xFE30 && c <= 0xFE4F) ||
                    (c >= 0xFF00 && c <= 0xFF60) ||     // 全角
                    (c >= 0xFFE0 && c <= 0xFFE6) ||
                    (c >= 0x20000 && c <= 0x3FFFD)))
                w += 2;
            else
                w += 1;
        }
        return w;
    }

    public static string Pad(string s, int width)
    {
        s ??= "";
        int gap = width - Width(s);
        return gap > 0 ? s + new string(' ', gap) : s;
    }

    public static string PadL(string s, int width)
    {
        s ??= "";
        int gap = width - Width(s);
        return gap > 0 ? new string(' ', gap) + s : s;
    }

    public static string Cut(string s, int width)
    {
        s ??= "";
        if (Width(s) <= width) return s;
        while (s.Length > 0 && Width(s) > width) s = s[..^1];
        return s;
    }

    // ---------- 展示映射 ----------

    public static string PreEolText(int v) => v switch
    {
        0x01 => "正常",
        0x02 => "警告",
        0x03 => "紧急",
        -1 => "无数据",
        _ => "未知",
    };

    public static string LifeText(int v)
    {
        if (v >= 1 && v <= 10) return $"{(v - 1) * 10}-{v * 10}%";
        if (v == 0x0B) return "超寿";
        if (v == -1) return "无数据";
        return "未知";
    }

    public static string VerdictText(string v) => v switch
    {
        "ok" => "正常",
        "warning" => "警告",
        "urgent" => "紧急",
        _ => "未知",
    };

    public static (ConsoleColor fg, bool bright) VerdictColor(string v) => v switch
    {
        "ok" => (ConsoleColor.Green, true),
        "warning" => (ConsoleColor.Yellow, true),
        "urgent" => (ConsoleColor.Red, true),
        _ => (ConsoleColor.Gray, true),
    };

    public static string CapacityText(long bytes)
    {
        if (bytes <= 0) return "未知";
        double gib = bytes / (1024.0 * 1024.0 * 1024.0);
        return gib >= 1 ? $"{gib:F1}G" : $"{bytes / 1048576.0:F0}M";
    }

    // ---------- 汇总表格 ----------

    private static readonly (string Title, int Width)[] Columns =
    {
        ("序号", 4), ("IP", 17), ("设备型号", 14), ("eMMC", 16), ("厂商", 16),
        ("容量", 7), ("预留块", 11), ("寿命A", 8), ("寿命B", 8), ("结论", 6), ("备注", 12),
    };

    private static string TableRow(string[] cells, bool header)
    {
        var sb = new StringBuilder();
        sb.Append(header ? "  " : "  ");
        for (int i = 0; i < Columns.Length; i++)
        {
            string cell = cells[i];
            bool leftAlign = i != 0; // 序号右对齐
            sb.Append(i == 0 ? PadL(Cut(cell, Columns[i].Width), Columns[i].Width)
                             : Pad(Cut(cell, Columns[i].Width), Columns[i].Width));
            sb.Append("  ");
        }
        return sb.ToString();
    }

    public static string TableHeaderLine()
    {
        var sb = new StringBuilder("  ");
        foreach (var (_, w) in Columns) sb.Append(new string('─', w)).Append("  ");
        return sb.ToString();
    }

    public static void PrintResults(IReadOnlyList<DeviceScanResult> results, bool noColor)
    {
        var header = Columns.Select(c => c.Title).ToArray();
        Console.WriteLine();
        Console.WriteLine(TableHeaderLine());
        Console.WriteLine(TableRow(header, true));
        Console.WriteLine(TableHeaderLine());
        int index = 1;
        foreach (var r in results.OrderBy(x => x.Ip, Comparer<string>.Create(Scan.IpOrderCompare)))
        {
            string[] cells;
            if (r.Ok && r.Report != null)
            {
                var d = r.Report;
                string note = "";
                if (r.RootNote == "无 root") note = "需root";
                if (!string.IsNullOrEmpty(d.Error)) note = string.IsNullOrEmpty(note) ? ShortErr(d.Error) : note + "/" + ShortErr(d.Error);
                cells = new[]
                {
                    index.ToString(),
                    $"{r.Ip}:{r.Port}",
                    r.PhoneModel.Length > 0 ? r.PhoneModel : "-",
                    d.Name.Length > 0 ? d.Name : "-",
                    d.Manufacturer,
                    CapacityText(d.CapacityBytes),
                    d.PreEol >= 0 ? $"0x{d.PreEol:x2} {PreEolText(d.PreEol)}" : "无数据",
                    LifeText(d.LifeA),
                    LifeText(d.LifeB),
                    VerdictText(d.Verdict),
                    note.Length > 0 ? note : r.RootNote,
                };
            }
            else
            {
                cells = new[]
                {
                    index.ToString(), $"{r.Ip}:{r.Port}", r.PhoneModel.Length > 0 ? r.PhoneModel : "-",
                    "-", "-", "-", "-", "-", "-", "-", r.Status + (r.ErrorDetail.Length > 0 ? " " + ShortErr(r.ErrorDetail) : ""),
                };
            }
            var line = TableRow(cells, false);
            if (!noColor && r.Ok && r.Report != null)
            {
                var (fg, bright) = VerdictColor(r.Report.Verdict);
                var old = Console.ForegroundColor;
                // 分段着色：结论列
                int prefixLen = 2;
                for (int i = 0; i < 9; i++) prefixLen += Columns[i].Width + 2;
                Console.Write(line.Substring(0, Math.Min(prefixLen, line.Length)));
                Console.ForegroundColor = fg;
                Console.Write(Pad(Cut(cells[9], Columns[9].Width), Columns[9].Width));
                Console.ForegroundColor = old;
                Console.WriteLine(line.Length > prefixLen + Columns[9].Width + 2
                    ? line.Substring(prefixLen + Columns[9].Width + 2)
                    : "");
            }
            else
            {
                Console.WriteLine(line);
            }
            index++;
        }
        Console.WriteLine(TableHeaderLine());
    }

    private static string ShortErr(string e) => e.Length > 18 ? e[..18] : e;

    // ---------- 报告落盘 ----------

    public static void WriteReports(string dir, IReadOnlyList<DeviceScanResult> results,
        int port, long elapsedMs)
    {
        Directory.CreateDirectory(dir);
        var ts = DateTime.Now;
        string stamp = ts.ToString("yyyyMMdd_HHmmss");

        // 汇总 CSV（带 BOM，Excel 直接打开）
        var csv = new StringBuilder();
        csv.AppendLine("序号,IP,端口,设备型号,ABI,Root通道,eMMC,厂商,MANFID,序列号,生产日期,容量字节,容量,CID,ExtCSD版本,MMC规范,预留块(pre_eol),寿命A,寿命B,结论,数据来源,错误,扫描时间");
        int i = 1;
        foreach (var r in results.OrderBy(x => x.Ip, Comparer<string>.Create(Scan.IpOrderCompare)))
        {
            var d = r.Report;
            string esc(string s) => "\"" + (s ?? "").Replace("\"", "\"\"") + "\"";
            csv.AppendLine(string.Join(",",
                i++, esc(r.Ip), r.Port,
                esc(r.PhoneModel), esc(r.Abi), esc(r.RootNote),
                esc(d?.Name ?? ""), esc(d?.Manufacturer ?? ""), esc(d?.Manfid ?? ""),
                esc(d?.Serial ?? ""), esc(d?.Date ?? ""),
                d?.CapacityBytes.ToString() ?? "",
                esc(CapacityText(d?.CapacityBytes ?? -1)),
                esc(d?.Cid ?? ""),
                d?.ExtCsdRev.ToString() ?? "", esc(d?.MmcSpec ?? ""),
                d?.PreEol.ToString() ?? "", d?.LifeA.ToString() ?? "", d?.LifeB.ToString() ?? "",
                esc(d != null ? VerdictText(d.Verdict) : ""),
                esc(d?.Source ?? ""),
                esc(string.IsNullOrEmpty(r.ErrorDetail) ? d?.Error ?? "" : r.ErrorDetail),
                esc(ts.ToString("yyyy-MM-dd HH:mm:ss"))));
        }
        File.WriteAllText(Path.Combine(dir, $"eMMC健康汇总_{stamp}.csv"), csv.ToString(), new UTF8Encoding(true));

        // 每台明细 txt
        foreach (var r in results.OrderBy(x => x.Ip, Comparer<string>.Create(Scan.IpOrderCompare)))
        {
            var sb = new StringBuilder();
            sb.AppendLine($"========== eMMC 健康检测明细：{r.Ip}:{r.Port} ==========");
            sb.AppendLine($"扫描时间: {ts:yyyy-MM-dd HH:mm:ss}");
            sb.AppendLine($"设备型号: {r.PhoneModel}    ABI: {r.Abi}    Root通道: {r.RootNote}");
            sb.AppendLine($"状态: {r.Status}");
            var d = r.Report;
            if (d != null)
            {
                sb.AppendLine();
                sb.AppendLine($"eMMC 设备: {d.Device} ({d.Node})  类型: {d.Type}");
                sb.AppendLine($"型号: {d.Name}    厂商: {d.Manufacturer} (MANFID {d.Manfid}, OEMID {d.Oemid})");
                sb.AppendLine($"序列号: {d.Serial}    生产日期: {d.Date}");
                sb.AppendLine($"容量: {CapacityText(d.CapacityBytes)} ({d.CapacityBytes} 字节)");
                sb.AppendLine($"CID: {d.Cid}");
                sb.AppendLine($"CSD: {d.Csd}");
                sb.AppendLine();
                sb.AppendLine($"Ext CSD 版本: {d.ExtCsdRev} (MMC {d.MmcSpec})");
                sb.AppendLine($"预留块状态 pre_eol_info: 0x{(d.PreEol >= 0 ? d.PreEol : 0):x2}（{PreEolText(d.PreEol)}）");
                sb.AppendLine($"寿命估计 A: 0x{(d.LifeA >= 0 ? d.LifeA : 0):x2}（{LifeText(d.LifeA)}）");
                sb.AppendLine($"寿命估计 B: 0x{(d.LifeB >= 0 ? d.LifeB : 0):x2}（{LifeText(d.LifeB)}）");
                sb.AppendLine($"综合结论: {VerdictText(d.Verdict)}");
                sb.AppendLine($"数据来源: {d.Source}");
                if (!string.IsNullOrEmpty(d.Error)) sb.AppendLine($"设备端提示: {d.Error} {d.OsError}");
            }
            if (!string.IsNullOrEmpty(r.ErrorDetail)) sb.AppendLine($"失败详情: {r.ErrorDetail}");
            sb.AppendLine();
            sb.AppendLine("[原始 JSON]");
            sb.AppendLine(r.RawJson);
            string safeIp = r.Ip.Replace('.', '_');
            File.WriteAllText(Path.Combine(dir, $"emmc_{safeIp}_{stamp}.txt"), sb.ToString(), new UTF8Encoding(true));
            if (!string.IsNullOrEmpty(r.RawJson))
                File.WriteAllText(Path.Combine(dir, $"raw_{safeIp}_{stamp}.json"), r.RawJson + Environment.NewLine, new UTF8Encoding(false));
        }

        // 运行摘要
        var meta = new StringBuilder();
        meta.AppendLine($"eMMC健康扫描 汇总");
        meta.AppendLine($"时间: {ts:yyyy-MM-dd HH:mm:ss}    耗时: {elapsedMs / 1000.0:F1}s    adb端口: {port}");
        meta.AppendLine($"设备数: {results.Count}    成功: {results.Count(x => x.Ok)}    失败: {results.Count(x => !x.Ok)}");
        var counts = results.Where(x => x.Ok).GroupBy(x => x.Report?.Verdict)
            .ToDictionary(g => g.Key ?? "", g => g.Count());
        meta.AppendLine($"结论分布: " + string.Join("  ", counts.Select(kv => $"{VerdictText(kv.Key)}={kv.Value}")));
        File.WriteAllText(Path.Combine(dir, $"运行摘要_{stamp}.txt"), meta.ToString(), new UTF8Encoding(true));
    }
}
