import { useCallback, useEffect, useRef, useState } from 'react'
import LunaToolbar, {
  LunaToolbarInput,
  LunaToolbarSpace,
  LunaToolbarText,
} from 'luna-toolbar/react'
import LunaDataGrid from 'luna-data-grid/react'
import DataGrid from 'luna-data-grid'
import { useResizeSensor } from 'share/renderer/lib/hooks'
import ToolbarIcon from 'share/renderer/components/ToolbarIcon'
import Style from './App.module.scss'

interface DeviceResult {
  ip: string
  port: number
  model: string
  abi: string
  rootNote: string
  status: string
  ok: boolean
  report: any
  errorDetail: string
}

interface ScanStartResult {
  ok: boolean
  found: number
  okCount: number
  reportDir: string
  message: string
}

const VERDICT_COLOR: Record<string, string> = {
  ok: '#52c41a',
  warning: '#faad14',
  urgent: '#ff4d4f',
  unknown: '#8c8c8c',
}

function verdictZh(v: string): string {
  return v === 'ok' ? '正常' : v === 'warning' ? '警告' : v === 'urgent' ? '紧急' : '未知'
}

function preEolZh(v: number): string {
  if (v === 1) return '正常'
  if (v === 2) return '警告'
  if (v === 3) return '紧急'
  return v >= 0 ? '未知' : '无数据'
}

function lifeText(v: number): string {
  if (v >= 1 && v <= 10) return `${(v - 1) * 10}%-${v * 10}%`
  if (v === 11) return '超寿'
  if (v === 0) return '未定义'
  if (v === -1) return '无数据'
  return '未知'
}

function capText(bytes: number): string {
  if (!bytes || bytes <= 0) return '-'
  const gib = bytes / 1024 / 1024 / 1024
  return gib >= 1 ? gib.toFixed(1) + 'G' : (bytes / 1048576).toFixed(0) + 'M'
}

/** 带彩色圆点的结论单元格（luna-data-grid 支持 HTMLElement 值） */
function coloredCell(text: string, color: string): HTMLElement {
  const wrap = document.createElement('span')
  wrap.className = Style.badge
  const dot = document.createElement('i')
  dot.className = Style.dot
  dot.style.background = color
  const label = document.createElement('span')
  label.textContent = text
  wrap.appendChild(dot)
  wrap.appendChild(label)
  return wrap
}

const columns = [
  { id: 'ip', title: 'IP', sortable: true, weight: 12 },
  { id: 'model', title: '设备型号', sortable: true, weight: 11 },
  { id: 'name', title: 'eMMC', sortable: true, weight: 10 },
  { id: 'manufacturer', title: '厂商', sortable: true, weight: 12 },
  { id: 'cap', title: '容量', sortable: true, weight: 6 },
  { id: 'date', title: '生产日期', sortable: true, weight: 9 },
  { id: 'preeol', title: '预留块', sortable: true, weight: 11 },
  { id: 'lifeA', title: '寿命A', sortable: true, weight: 8 },
  { id: 'lifeB', title: '寿命B', sortable: true, weight: 8 },
  { id: 'verdict', title: '结论', sortable: true, weight: 8 },
  { id: 'note', title: '备注', sortable: true, weight: 12 },
]

export default function App() {
  const [rows, setRows] = useState<any[]>([])
  const [results, setResults] = useState<DeviceResult[]>([])
  const [sel, setSel] = useState<any>(null)
  const [busy, setBusy] = useState(false)
  const [subnet, setSubnet] = useState('')
  const [hosts, setHosts] = useState('')
  const [summary, setSummary] = useState('')
  const [counts, setCounts] = useState({ ok: 0, warning: 0, urgent: 0, unknown: 0, fail: 0 })
  const [hasScanned, setHasScanned] = useState(false)
  const [reportDir, setReportDir] = useState('')
  const gridRef = useRef<DataGrid>(null)
  const containerRef = useRef<HTMLDivElement>(null)
  const summaryRef = useRef<HTMLPreElement>(null)

  useResizeSensor(containerRef, () => {
    gridRef.current?.fit()
  })

  useEffect(() => {
    main.on('scanEvent', (ev: any) => {
      if (ev.type === 'progress') {
        setSummary(`正在扫描网段… 进度 ${ev.done}/${ev.total}，已发现 ${ev.found} 台`)
      } else if (ev.type === 'device') {
        const r: DeviceResult = ev.result
        setResults((prev) => {
          const next = [...prev, r]
          setRows(next.map(toRow))
          recount(next)
          return next
        })
      } else if (ev.type === 'done') {
        setBusy(false)
        if ((ev as any).stopped) {
          setSummary('已停止扫描。')
          return
        }
        if (ev.total === 0) {
          setSummary(
            '网段内未发现 adb 设备（端口未开放）。\n确认设备已开启网络 ADB，或在「IP 列表」中直接填入目标 IP。'
          )
          return
        }
        setResults((prev) => {
          const c = { ok: 0, warning: 0, urgent: 0, unknown: 0, fail: 0 }
          for (const r of prev) {
            if (!r.ok) c.fail++
            else {
              const v = r.report?.verdict
              if (v === 'ok') c.ok++
              else if (v === 'warning') c.warning++
              else if (v === 'urgent') c.urgent++
              else c.unknown++
            }
          }
          setCounts(c)
          setSummary(
            `完成：成功 ${ev.ok}/${ev.total}，耗时 ${(ev.elapsedMs / 1000).toFixed(1)} 秒。` +
              (c.warning + c.urgent > 0
                ? `\n注意：${c.warning + c.urgent} 台存在警告或紧急状态，建议尽快备份并安排更换。`
                : '\n全部设备 eMMC 状态健康。') +
              (ev.reportDir ? `\n报告已保存：${ev.reportDir}` : '')
          )
          gridRef.current?.fit()
          return prev
        })
        setReportDir(ev.reportDir || '')
      }
    })
  }, [])

  function recount(list: DeviceResult[]) {
    const c = { ok: 0, warning: 0, urgent: 0, unknown: 0, fail: 0 }
    for (const r of list) {
      if (!r.ok) c.fail++
      else {
        const v = r.report?.verdict
        if (v === 'ok') c.ok++
        else if (v === 'warning') c.warning++
        else if (v === 'urgent') c.urgent++
        else c.unknown++
      }
    }
    setCounts(c)
  }

  function toRow(r: DeviceResult) {
    const d: any = r.report || {}
    const id = `${r.ip}:${r.port}`
    if (!r.ok) {
      return {
        id,
        ip: id,
        model: r.model || '-',
        name: '-',
        manufacturer: '-',
        cap: '-',
        date: '-',
        preeol: '-',
        lifeA: '-',
        lifeB: '-',
        verdict: '-',
        note: r.status + (r.errorDetail ? ' ' + r.errorDetail : ''),
      }
    }
    const vText = verdictZh(d.verdict)
    return {
      id,
      ip: id,
      model: r.model || '-',
      name: d.name || '-',
      manufacturer: `${d.manufacturer || '-'} (${d.manfid || '-'})`,
      cap: capText(d.capacity_bytes),
      date: d.date || '-',
      preeol: `0x${d.pre_eol.toString(16)} ${preEolZh(d.pre_eol)}`,
      lifeA: lifeText(d.life_a),
      lifeB: lifeText(d.life_b),
      verdict: vText,
      note: r.rootNote + (d.error ? ' / ' + d.error : ''),
    }
  }

  const startScan = useCallback(async () => {
    if (busy) return
    setBusy(true)
    setRows([])
    setResults([])
    setSel(null)
    setHasScanned(true)
    const hostList = hosts
      .split(/[,;\s]+/)
      .map((s) => s.trim())
      .filter(Boolean)
    if (hostList.length > 0 && !/^\d+\.\d+\.\d+\.\d+$/.test(hostList[0])) {
      setSummary('IP 列表格式不正确，例如：192.168.20.103,192.168.20.124')
      setBusy(false)
      return
    }
    setSummary(hostList.length > 0 ? `正在扫描 ${hostList.length} 个目标…` : '正在探测网段…')
    try {
      const r: ScanStartResult = await main.scanStart({
        subnets: subnet.trim() ? [subnet.trim()] : [],
        hosts: hostList,
        port: 5555,
        timeoutMs: 1500,
        keepRemote: false,
      })
      if (!r.ok) {
        setSummary(r.message)
        setBusy(false)
      } else if (r.found === 0) {
        setSummary(
          '网段内未发现 adb 设备（端口未开放）。\n确认设备已开启网络 ADB，或在「IP 列表」中直接填入目标 IP。'
        )
        setBusy(false)
      }
      // 'done' 事件负责收尾文案
    } catch (e: any) {
      setSummary('错误: ' + (e && e.message ? e.message : e))
      setBusy(false)
    }
  }, [busy, subnet, hosts])

  const stopScan = useCallback(async () => {
    await main.scanStop()
  }, [])

  const detail = useCallback(
    (row?: any) => {
      const target = row || sel
      if (!target) return
      const r = results.find((x) => `${x.ip}:${x.port}` === target.id)
      if (!r) return
      if (!r.ok) {
        setSummary(
          `========== ${r.ip}:${r.port} ==========\n状态: ${r.status}` +
            (r.errorDetail ? `\n失败详情: ${r.errorDetail}` : '')
        )
        return
      }
      const d: any = r.report
      const lines = [
        `========== eMMC 健康检测明细：${r.ip}:${r.port} ==========`,
        `设备型号: ${r.model}    ABI: ${r.abi}    Root通道: ${r.rootNote}`,
        '',
        `eMMC 设备: ${d.device} (${d.node})  类型: ${d.type}`,
        `型号: ${d.name}    厂商: ${d.manufacturer} (MANFID ${d.manfid}, OEMID ${d.oemid})`,
        `序列号: ${d.serial}    生产日期: ${d.date}`,
        `容量: ${capText(d.capacity_bytes)} (${d.capacity_bytes} 字节)`,
        `CID: ${d.cid}`,
        '',
        `Ext CSD 版本: ${d.ext_csd_rev} (MMC ${d.mmc_spec})`,
        `预留块状态 pre_eol_info: 0x${d.pre_eol.toString(16)}（${preEolZh(d.pre_eol)}）`,
        `寿命估计 A: 0x${d.life_a.toString(16)}（${lifeText(d.life_a)}）`,
        `寿命估计 B: 0x${d.life_b.toString(16)}（${lifeText(d.life_b)}）`,
        `综合结论: ${verdictZh(d.verdict)}`,
        `数据来源: ${d.source}`,
        d.error ? `设备端提示: ${d.error} ${d.os_error || ''}` : '',
        '',
        '[原始 JSON]',
        JSON.stringify(d),
      ]
      setSummary(lines.filter((l) => l !== '').join('\n'))
      if (summaryRef.current) summaryRef.current.scrollTop = 0
    },
    [sel, results]
  )

  const empty = hasScanned && !busy && rows.length === 0

  return (
    <div className={Style.container} ref={containerRef}>
      <LunaToolbar className={Style.toolbar}>
        <ToolbarIcon
          icon="play"
          className={busy ? Style.btnDisabled : Style.btnPlay}
          disabled={busy}
          onClick={startScan}
        >
          开始扫描
        </ToolbarIcon>
        <ToolbarIcon
          icon="power"
          className={Style.btnStop}
          disabled={!busy}
          onClick={stopScan}
        >
          停止
        </ToolbarIcon>
        <LunaToolbarSpace />
        <LunaToolbarText text="网段" />
        <LunaToolbarInput
          keyName="subnet"
          className={Style.inputSubnet}
          value={subnet}
          onChange={(value: string) => setSubnet(value)}
          placeholder="例如 192.168.1.1"
        />
        <LunaToolbarText text="IP列表" />
        <LunaToolbarInput
          keyName="hosts"
          className={Style.inputHosts}
          value={hosts}
          onChange={(value: string) => setHosts(value)}
          placeholder="192.168.20.103,192.168.20.124"
        />
        <LunaToolbarSpace />
        <ToolbarIcon icon="open-file" disabled={!reportDir} onClick={() => main.scanOpenReports()}>
          报告
        </ToolbarIcon>
      </LunaToolbar>
      <div className={Style.gridWrap}>
        <LunaDataGrid
          theme="auto"
          onSelect={(node: any) => {
            setSel(node.data)
            detail(node.data)
          }}
          onDeselect={() => setSel(null)}
          className={Style.grid}
          data={rows}
          columns={columns}
          selectable={true}
          uniqueId="id"
          onCreate={(dataGrid) => {
            gridRef.current = dataGrid
            dataGrid.fit()
          }}
        />
        {empty && (
          <div className={Style.empty}>
            <div className={Style.emptyIcon}>
              <span className="icon icon-grid" />
            </div>
            <div className={Style.emptyText}>未发现设备</div>
            <div className={Style.emptyHint}>
              点「开始扫描」自动探测本机网段，或在上方填入 IP 列表后重新扫描
            </div>
          </div>
        )}
      </div>
      <div className={Style.reportHeader}>
        <span className={Style.reportTitle}>扫描报告</span>
        <span className={Style.badges}>
          {hasScanned && (
            <>
              <span className={Style.badgeOk}>正常 {counts.ok}</span>
              <span className={Style.badgeWarn}>警告 {counts.warning}</span>
              <span className={Style.badgeUrgent}>紧急 {counts.urgent}</span>
              <span className={Style.badgeUnknown}>未知 {counts.unknown}</span>
              {counts.fail > 0 && <span className={Style.badgeFail}>失败 {counts.fail}</span>}
            </>
          )}
        </span>
      </div>
      <pre className={Style.report} ref={summaryRef}>
        {summary ||
          '点「开始扫描」自动探测本机网段并扫描所有设备的 eMMC 健康度。\n指定网段填入如 192.168.1.x；指定单台填入 IP 列表如 192.168.20.103,192.168.20.124。\n选中表格行可查看该设备的完整检测明细。'}
      </pre>
      <div className={Style.statusbar}>
        <span>eMMC健康扫描 v{VERSION}</span>
        <span>adb 端口 5555</span>
        {reportDir && <span className={Style.statusDir}>{reportDir}</span>}
      </div>
    </div>
  )
}
