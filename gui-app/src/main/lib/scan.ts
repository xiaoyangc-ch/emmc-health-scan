import { app, BrowserWindow, ipcMain, shell } from 'electron'
import { spawn, ChildProcess } from 'child_process'
import net from 'net'
import os from 'os'
import fs from 'fs-extra'
import path from 'path'
import { resolveResources } from 'share/main/lib/util'
import * as window from 'share/main/lib/window'
import log from 'share/common/log'

const logger = log('scan')
const REMOTE = '/data/local/tmp/emmc_health'
const MEM_REMOTE = '/data/local/tmp/memtest'

/** 扫描事件推给渲染层 */
type ScanEvent =
  | { type: 'progress'; done: number; total: number; found: number }
  | { type: 'device'; result: DeviceResult }
  | { type: 'done'; ok: number; total: number; elapsedMs: number; reportDir: string }

export interface MemTestResult {
  status: string
  testedMB: number
  errors: number
  firstError: string
}

export interface DeviceResult {
  ip: string
  port: number
  model: string
  abi: string
  rootNote: string
  status: string
  ok: boolean
  report: any | null
  errorDetail: string
  mem: MemTestResult | null
}

interface ScanOptions {
  subnets: string[]
  port: number
  timeoutMs: number
}

let running = false
let stopRequested = false
let adbOverride = ''

// ---------- adb 执行（每次调用带超时，超时杀进程防挂死） ----------

interface AdbResult {
  code: number
  stdout: string
  stderr: string
  timedOut: boolean
  durationMs: number
}

function adbPath(): string {
  if (adbOverride) return adbOverride
  return resolveResources(path.join('adb', 'adb.exe'))
}

function runAdb(args: string[], timeoutMs: number): Promise<AdbResult> {
  return new Promise((resolve) => {
    const started = Date.now()
    const child: ChildProcess = spawn(adbPath(), args, {
      windowsHide: true,
      timeout: timeoutMs,
      killSignal: 'SIGKILL',
    })
    let stdout = ''
    let stderr = ''
    let timedOut = false
    child.stdout?.on('data', (d) => (stdout += d.toString('utf8')))
    child.stderr?.on('data', (d) => (stderr += d.toString('utf8')))
    child.on('error', (e: any) => {
      resolve({
        code: -1,
        stdout,
        stderr: String(e && e.message),
        timedOut,
        durationMs: Date.now() - started,
      })
    })
    child.on('close', (code) => {
      resolve({
        code: code ?? -1,
        stdout,
        stderr,
        timedOut,
        durationMs: Date.now() - started,
      })
    })
    child.on('exit', () => {
      /* noop */
    })
    // spawn 的 timeout 选项在某些版本下不杀子树，这里兜底强杀
    setTimeout(() => {
      if (!child.killed && child.exitCode === null) {
        timedOut = true
        try {
          child.kill('SIGKILL')
        } catch {
          /* ignore */
        }
      }
    }, timeoutMs)
    logger.info('adb', args.join(' '))
  })
}

const clean = (s: string) => (s || '').replace(/\r/g, '').replace(/\n/g, ' ').trim()

async function shell(serial: string, command: string, timeoutMs: number): Promise<string> {
  const parts = ['-s', serial, 'shell', ...command.split(' ').filter((x) => x)]
  const r = await runAdb(parts, timeoutMs)
  return r.stdout
}

// ---------- 网段探测与端口扫描 ----------

export function detectSubnets(): string[] {
  const subnets = new Set<string>()
  const ifaces = os.networkInterfaces()
  for (const list of Object.values(ifaces)) {
    for (const ua of list || []) {
      if (ua.family !== 'IPv4' || ua.internal) continue
      if (ua.address.startsWith('127.')) continue
      if (ua.address.startsWith('169.254.')) continue
      const b = ua.address.split('.')
      subnets.add(`${b[0]}.${b[1]}.${b[2]}.0/24`)
    }
  }
  return Array.from(subnets).sort((a, b) => {
    const pa = a.split('.').map(Number)
    const pb = b.split('.').map(Number)
    for (let i = 0; i < 4; i++) if (pa[i] !== pb[i]) return pa[i] - pb[i]
    return 0
  })
}

export function expandSubnet(subnet: string): string[] {
  subnet = subnet.trim()
  if (subnet.endsWith('.0/24')) {
    const prefix = subnet.slice(0, -5)
    return Array.from({ length: 254 }, (_, i) => `${prefix}.${i + 1}`)
  }
  // "192.168.1.x" 标准示例写法
  if (/^\d+\.\d+\.\d+\.x$/i.test(subnet)) {
    const prefix = subnet.slice(0, -2)
    return Array.from({ length: 254 }, (_, i) => `${prefix}.${i + 1}`)
  }
  if (/^\d+\.\d+\.\d+\.$/.test(subnet)) {
    return Array.from({ length: 254 }, (_, i) => `${subnet}${i + 1}`)
  }
  // 具体IP也按"所在网段"处理：输入 192.168.1.1 即扫描 192.168.1.0/24 全段
  // （精确单台扫描用「IP 列表」）
  if (/^\d+\.\d+\.\d+\.\d+$/.test(subnet)) {
    const b = subnet.split('.')
    return Array.from({ length: 254 }, (_, i) => `${b[0]}.${b[1]}.${b[2]}.${i + 1}`)
  }
  return []
}

function portOpen(ip: string, port: number, timeoutMs: number): Promise<boolean> {
  return new Promise((resolve) => {
    const socket = new net.Socket()
    let settled = false
    const done = (v: boolean) => {
      if (settled) return
      settled = true
      socket.destroy()
      resolve(v)
    }
    socket.setTimeout(timeoutMs)
    socket.once('connect', () => done(true))
    socket.once('timeout', () => done(false))
    socket.once('error', () => done(false))
    try {
      socket.connect(port, ip)
    } catch {
      done(false)
    }
  })
}

async function sweep(
  ips: string[],
  port: number,
  timeoutMs: number,
  win: BrowserWindow,
  totalAll: number,
  doneBase: number
): Promise<string[]> {
  const found: string[] = []
  let done = 0
  const workers = 200
  let next = 0
  async function worker() {
    while (!stopRequested) {
      const i = next++
      if (i >= ips.length) return
      const ip = ips[i]
      if (await portOpen(ip, port, timeoutMs)) found.push(ip)
      done++
      if (done % 20 === 0 || done === ips.length) {
        sendProgress(win, doneBase + done, totalAll, found.length)
      }
    }
  }
  await Promise.all(Array.from({ length: workers }, worker))
  const ord = (s: string) => s.split('.').map(Number)
  return found.sort((a, b) => {
    const pa = ord(a)
    const pb = ord(b)
    for (let i = 0; i < 4; i++) if (pa[i] !== pb[i]) return pa[i] - pb[i]
    return 0
  })
}

// ---------- 单台设备流水线 ----------

function binFor(abi: string): string {
  return resolveResources(
    path.join('emmc', abi.includes('arm64') ? 'emmc_health_v8a' : 'emmc_health_v7a')
  )
}

function verdictZh(v: string): string {
  return v === 'ok' ? '正常' : v === 'warning' ? '警告' : v === 'urgent' ? '紧急' : '未知'
}

/** 内存快测结果的中文摘要 */
function memZh(m: MemTestResult | null | undefined): string {
  if (!m) return ''
  if (m.status === 'pass') return `通过 ${m.testedMB}M`
  if (m.status === 'partial') return `通过(部分) ${m.testedMB}M`
  if (m.status === 'fail')
    return `失败 ${m.errors}处${m.firstError ? ' @' + m.firstError : ''}`
  return m.status
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
  if (v === -1) return '无数据'
  return '未知'
}

function capText(bytes: number): string {
  if (!bytes || bytes <= 0) return '-'
  const gib = bytes / 1024 / 1024 / 1024
  return gib >= 1 ? gib.toFixed(1) + 'G' : (bytes / 1048576).toFixed(0) + 'M'
}

function parseDeviceJson(output: string): any | null {
  const start = output.indexOf('{')
  const end = output.lastIndexOf('}')
  if (start < 0 || end <= start) return null
  try {
    return JSON.parse(output.slice(start, end + 1))
  } catch {
    return null
  }
}

async function scanDevice(
  adb: string,
  ip: string,
  port: number,
  keepRemote: boolean,
  memEnabled: boolean
): Promise<DeviceResult> {
  const r: DeviceResult = {
    ip,
    port,
    model: '',
    abi: '',
    rootNote: '',
    status: '',
    ok: false,
    report: null,
    errorDetail: '',
    mem: null,
  }
  const serial = `${ip}:${port}`

  // 1) connect
  const conn = await runAdb(['connect', serial], 15000)
  const connText = clean(conn.stdout + ' ' + conn.stderr)
  if (conn.timedOut) {
    r.status = '连接超时'
    return r
  }
  if (/failed|cannot/i.test(connText)) {
    r.status = '连接失败'
    r.errorDetail = connText
    return r
  }

  // 2) get-state（客户端命令，不带 shell；无响应等 2s 重试一次）
  let state = clean((await runAdb(['-s', serial, 'get-state'], 10000)).stdout)
  if (!state) {
    await new Promise((res) => setTimeout(res, 2000))
    state = clean((await runAdb(['-s', serial, 'get-state'], 10000)).stdout)
  }
  if (state === 'unauthorized') {
    r.status = '未授权'
    return r
  }
  if (state === 'offline') {
    r.status = '离线'
    return r
  }
  if (state !== 'device') {
    r.status = '状态异常(' + (state || '无响应') + ')'
    return r
  }

  // 3) 型号与 ABI
  r.model = clean(await shell(serial, 'getprop ro.product.model', 8000))
  if (!r.model) r.model = clean(await shell(serial, 'getprop ro.product.device', 8000))
  r.abi = clean(await shell(serial, 'getprop ro.product.cpu.abi', 8000))

  // 4) 推送设备端工具
  const bin = binFor(r.abi)
  if (!fs.existsSync(bin)) {
    r.status = '本地缺少工具: ' + bin
    return r
  }
  const push = await runAdb(['-s', serial, 'push', bin, REMOTE], 60000)
  if (push.code !== 0 || push.timedOut) {
    r.status = '推送失败'
    r.errorDetail = clean(push.stderr) || clean(push.stdout)
    return r
  }
  await shell(serial, 'chmod 755 ' + REMOTE, 8000)

  try {
    // 5) root 通道探测
    const suProbe = clean(await shell(serial, 'su 0 id -u', 10000))
    const su0 = suProbe === '0'
    if (!su0) {
      const root = await runAdb(['-s', serial, 'root'], 10000)
      const rootProbe = clean(await shell(serial, 'id -u', 10000))
      r.rootNote = rootProbe === '0' ? 'adb root' : '无 root'
      logger.info('root fallback', serial, clean(root.stdout), rootProbe)
    } else {
      r.rootNote = 'su 0'
    }

    // 6) 执行扫描
    const command = su0 ? `su 0 ${REMOTE} --json` : `${REMOTE} --json`
    const scan = await runAdb(
      ['-s', serial, 'shell', ...command.split(' ').filter((x) => x)],
      60000
    )
    let output = clean(scan.stdout)
    if (!output && scan.stderr.trim()) output = clean(scan.stderr)
    if (!output) {
      r.status = '设备无输出'
      r.errorDetail = scan.timedOut ? '执行超时' : clean(scan.stderr)
      return r
    }
    const report = parseDeviceJson(output)
    if (!report) {
      r.status = '输出解析失败'
      r.errorDetail = output.slice(0, 200)
      return r
    }
    r.report = report
    r.ok = true
    r.status = '扫描成功'

    // 6.5) 可选内存快测（独立于 eMMC 结果；写读校验无需 root）
    if (memEnabled) {
      const memBin = resolveResources(
        path.join('emmc', r.abi.includes('arm64') ? 'memtest_v8a' : 'memtest_v7a')
      )
      const mpush = await runAdb(['-s', serial, 'push', memBin, MEM_REMOTE], 60000)
      if (mpush.code === 0 && !mpush.timedOut) {
        await shell(serial, 'chmod 755 ' + MEM_REMOTE, 8000)
        const margs = ['-s', serial, 'shell', ...`${MEM_REMOTE} --mb auto --seconds 60 --json`.split(' ')]
        const mrun = await runAdb(margs, 150000)
        const m = parseDeviceJson(clean(mrun.stdout))
        if (m) {
          r.mem = {
            status: m.status === 'fail' ? 'fail' : m.stopped_by_deadline ? 'partial' : 'pass',
            testedMB: Math.round((m.tested_bytes || 0) / 1048576),
            errors: m.errors || 0,
            firstError: m.first_error_offset >= 0 ? '0x' + m.first_error_offset.toString(16) : '',
          }
        } else {
          r.mem = { status: '无输出', testedMB: 0, errors: 0, firstError: '' }
        }
      } else {
        r.mem = { status: '推送失败', testedMB: 0, errors: 0, firstError: '' }
      }
    }
    return r
  } finally {
    // 7) 清理远端临时文件
    if (!keepRemote) {
      await shell(serial, 'rm -f ' + REMOTE + ' ' + MEM_REMOTE, 8000)
    }
  }
}

// ---------- 报告落盘 ----------

function escapeCsv(s: string): string {
  return '"' + String(s ?? '').replace(/"/g, '""') + '"'
}

async function writeReports(
  dir: string,
  results: DeviceResult[],
  elapsedMs: number
): Promise<void> {
  await fs.ensureDir(dir)
  const ts = new Date()
  const stamp = ts.toISOString().replace(/[-:T]/g, '').slice(0, 14)
  const sorted = [...results].sort((a, b) => {
    const pa = a.ip.split('.').map(Number)
    const pb = b.ip.split('.').map(Number)
    for (let i = 0; i < 4; i++) if (pa[i] !== pb[i]) return pa[i] - pb[i]
    return 0
  })

  const head =
    '序号,IP,端口,设备型号,ABI,Root通道,eMMC,厂商,序列号,生产日期,容量字节,容量,CID,ExtCSD版本,MMC规范,预留块(pre_eol),寿命A,寿命B,结论,内存快测,数据来源,错误,扫描时间'
  const lines = [head]
  sorted.forEach((r, i) => {
    const d: any = r.report || {}
    lines.push(
      [
        i + 1,
        r.ip,
        r.port,
        escapeCsv(r.model),
        escapeCsv(r.abi),
        escapeCsv(r.rootNote),
        escapeCsv(d.name || ''),
        escapeCsv(d.manufacturer || ''),
        escapeCsv(d.serial || ''),
        escapeCsv(d.date || ''),
        d.capacity_bytes ?? '',
        escapeCsv(capText(d.capacity_bytes ?? -1)),
        escapeCsv(d.cid || ''),
        d.ext_csd_rev ?? '',
        escapeCsv(d.mmc_spec || ''),
        d.pre_eol ?? '',
        d.life_a ?? '',
        d.life_b ?? '',
        escapeCsv(r.ok ? verdictZh(d.verdict) : ''),
        escapeCsv(memZh(r.mem)),
        escapeCsv(d.source || ''),
        escapeCsv(r.errorDetail || d.error || ''),
        escapeCsv(ts.toLocaleString('zh-CN')),
      ].join(',')
    )
  })
  await fs.writeFile(
    path.join(dir, `eMMC健康汇总_${stamp}.csv`),
    '\uFEFF' + lines.join('\r\n'),
    'utf-8'
  )

  for (const r of sorted) {
    const d: any = r.report || {}
    const sb: string[] = []
    sb.push(`========== eMMC 健康检测明细：${r.ip}:${r.port} ==========`)
    sb.push(`扫描时间: ${ts.toLocaleString('zh-CN')}`)
    sb.push(`设备型号: ${r.model}    ABI: ${r.abi}    Root通道: ${r.rootNote}`)
    sb.push(`状态: ${r.status}`)
    if (r.mem) sb.push(`内存快测: ${memZh(r.mem)}`)
    if (r.ok && r.report) {
      sb.push('')
      sb.push(`eMMC 设备: ${d.device} (${d.node})  类型: ${d.type}`)
      sb.push(`型号: ${d.name}    厂商: ${d.manufacturer} (MANFID ${d.manfid}, OEMID ${d.oemid})`)
      sb.push(`序列号: ${d.serial}    生产日期: ${d.date}`)
      sb.push(`容量: ${capText(d.capacity_bytes)} (${d.capacity_bytes} 字节)`)
      sb.push(`CID: ${d.cid}`)
      sb.push(`CSD: ${d.csd}`)
      sb.push('')
      sb.push(`Ext CSD 版本: ${d.ext_csd_rev} (MMC ${d.mmc_spec})`)
      sb.push(
        `预留块状态 pre_eol_info: 0x${(d.pre_eol >= 0 ? d.pre_eol : 0).toString(16)}（${preEolZh(d.pre_eol)}）`
      )
      sb.push(`寿命估计 A: 0x${(d.life_a >= 0 ? d.life_a : 0).toString(16)}（${lifeText(d.life_a)}）`)
      sb.push(`寿命估计 B: 0x${(d.life_b >= 0 ? d.life_b : 0).toString(16)}（${lifeText(d.life_b)}）`)
      sb.push(`综合结论: ${verdictZh(d.verdict)}`)
      sb.push(`数据来源: ${d.source}`)
      if (d.error) sb.push(`设备端提示: ${d.error} ${d.os_error || ''}`)
    }
    if (r.errorDetail) sb.push(`失败详情: ${r.errorDetail}`)
    sb.push('')
    sb.push('[原始 JSON]')
    sb.push(r.ok ? JSON.stringify(r.report) : '')
    const safeIp = r.ip.replace(/\./g, '_')
    await fs.writeFile(
      path.join(dir, `emmc_${safeIp}_${stamp}.txt`),
      sb.join('\r\n'),
      'utf-8'
    )
  }
}

// ---------- IPC 编排 ----------

function sendToMain(channel: string, ...args: any[]) {
  const win = window.getFocusedWin() || (BrowserWindow.getAllWindows()[0] ?? null)
  if (win && !win.isDestroyed()) {
    win.webContents.send(channel, ...args)
  }
}

function sendProgress(win: BrowserWindow | null, done: number, total: number, found: number) {
  if (win && !win.isDestroyed()) {
    win.webContents.send('scanEvent', { type: 'progress', done, total, found })
  }
}

export function init() {
  ipcMain.handle('scanDetectSubnets', () => detectSubnets())
  ipcMain.handle('scanOpenReports', async () => {
    const dir = path.join(app.getPath('userData'), 'reports')
    await fs.ensureDir(dir)
    shell.openPath(dir)
    return true
  })
  ipcMain.handle(
    'scanStart',
    async (
      _event,
      opts: ScanOptions & {
        hosts: string[]
        keepRemote: boolean
        adb: string
        memoryTest: boolean
      }
    ): Promise<{ ok: boolean; found: number; okCount: number; reportDir: string; message: string }> => {
      if (running) {
        return { ok: false, found: 0, okCount: 0, reportDir: '', message: '已有扫描在进行中' }
      }
      running = true
      stopRequested = false
      adbOverride = opts.adb || ''
      const started = Date.now()
      const win = BrowserWindow.getAllWindows()[0] ?? null
      let results: DeviceResult[] = []
      let reportDir = ''
      try {
        let targets: string[] = []
        const hostPorts = new Map<string, number>()
        if (opts.hosts && opts.hosts.length > 0) {
          for (const h of opts.hosts) {
            const m = h.split(':')
            const port = m.length === 2 && /^\d+$/.test(m[1]) ? Number(m[1]) : opts.port
            hostPorts.set(m[0], port)
            targets.push(m[0])
          }
        } else {
          const subnets = opts.subnets.length > 0 ? opts.subnets : detectSubnets()
          if (subnets.length === 0) {
            return { ok: false, found: 0, okCount: 0, reportDir: '', message: '未探测到本机网段' }
          }
          const ips: string[] = []
          for (const s of subnets) ips.push(...expandSubnet(s))
          targets = await sweep(
            Array.from(new Set(ips)),
            opts.port,
            opts.timeoutMs,
            win,
            ips.length,
            0
          )
        }

        if (targets.length === 0) {
          sendToMain('scanEvent', {
            type: 'done',
            ok: 0,
            total: 0,
            elapsedMs: Date.now() - started,
            reportDir: '',
          })
          return {
            ok: true,
            found: 0,
            okCount: 0,
            reportDir: '',
            message: '网段内未发现 adb 设备（端口未开放）',
          }
        }

        // 逐台扫描，并发 4
        let finished = 0
        const total = targets.length
        const all: DeviceResult[] = []
        let nextIdx = 0
        async function deviceWorker() {
          while (!stopRequested && nextIdx < total) {
            const idx = nextIdx++
            const ip = targets[idx]
            const port = hostPorts.get(ip) ?? opts.port
            const r = await scanDevice(adbPath(), ip, port, opts.keepRemote, !!opts.memoryTest)
            all.push(r)
            finished++
            sendToMain('scanEvent', { type: 'device', result: r })
            logger.info(`[${finished}/${total}] ${r.ip}:${port} ${r.status}`)
          }
        }
        await Promise.all(
          Array.from({ length: Math.min(4, total) }, deviceWorker)
        )
        results = all

        const okCount = results.filter((x) => x.ok).length
        reportDir = path.join(
          app.getPath('userData'),
          'reports',
          `eMMC扫描_${new Date().toISOString().replace(/[-:T]/g, '').slice(0, 14)}`
        )
        if (!opts.noReportFallback) await writeReports(reportDir, results, Date.now() - started)
        sendToMain('scanEvent', {
          type: 'done',
          ok: okCount,
          total: results.length,
          elapsedMs: Date.now() - started,
          reportDir,
        })
        return {
          ok: true,
          found: results.length,
          okCount,
          reportDir,
          message: `完成: 成功 ${okCount}/${results.length}`,
        }
      } finally {
        running = false
      }
    }
  )
  ipcMain.handle('scanStop', () => {
    stopRequested = true
    if (running) {
      sendToMain('scanEvent', {
        type: 'done',
        ok: -1,
        total: 0,
        elapsedMs: 0,
        reportDir: '',
        stopped: true,
      })
    }
    return running
  })
}

export function stopAll() {
  stopRequested = true
}
