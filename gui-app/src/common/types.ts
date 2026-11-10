export interface IDevice {
  id: string
  name: string
  serialno: string
  androidVersion: string
  sdkVersion: string
  type: 'emulator' | 'device' | 'offline' | 'unauthorized' | 'unknown'
}

export interface IPackageInfo {
  icon: string
  label: string
  enabled: boolean
  packageName: string
  versionName: string
  apkPath: string
  apkSize: number
  system: boolean
  firstInstallTime: number
  lastUpdateTime: number
  minSdkVersion?: number
  targetSdkVersion?: number
  dataSize: number
  cacheSize: number
  appSize: number
  signatures: string[]
}

export interface IFileStat {
  size?: number
  mtime: Date
  directory: boolean
  mode: string
}

export interface IFile extends IFileStat {
  name: string
  mime?: string
}

export interface IProcess {
  name: string
  pid: string
}

export enum TransferType {
  Upload,
  Download,
}

export type IpcGetFps = (deviceId: string, pkg: string) => Promise<number>
export type IpcGetDevices = () => Promise<IDevice[]>
export type IpcListForwards = (
  deviceId: string
) => Promise<Array<{ local: string; remote: string }>>
export type IpcListReverses = IpcListForwards
export type IpcForward = (
  deviceId: string,
  local: string,
  remote: string
) => void
export type IpcReverse = (
  deviceId: string,
  remote: string,
  local: string
) => void

export type IpcMeasureLatency = (deviceId: string) => Promise<{ ms: number }>
export type IpcGetPackageInfos = (
  deviceId: string,
  packageNames: string[]
) => Promise<IPackageInfo[]>
export type IpcPairDevice = (
  host: string,
  port: number,
  password: string
) => Promise<void>
export type IpcCreateShell = (deviceId: string) => Promise<string>
export type IpcWriteShell = (sessionId: string, data: string) => void
export type IpcResizeShell = (
  sessionId: string,
  cols: number,
  rows: number
) => void
export type IpcKillShell = (sessionId: string) => void
export type IpcScreencap = (deviceId: string) => Promise<string>
export type IpcOpenLogcat = (deviceId: string) => Promise<string>
export type IpcCloseLogcat = (logcatId: string) => Promise<void>
export type IpcPauseLogcat = IpcCloseLogcat
export type IpcResumeLogcat = IpcCloseLogcat
export type IpcInputKey = (deviceId: string, keyCode: number) => Promise<void>
export type IpcReverseTcp = (
  deviceId: string,
  remote: string
) => Promise<number>
export type IpcConnectDevice = (host: string, port?: number) => Promise<void>
export type IpcDisconnectDevice = IpcConnectDevice
export type IpcMoveFile = (
  deviceId: string,
  src: string,
  dest: string
) => Promise<void>
export type IpcStatFile = (deviceId: string, path: string) => Promise<IFileStat>
export type IpcReadDir = (deviceId: string, path: string) => Promise<IFile[]>
export type IpcReadDirPage = (
  deviceId: string,
  path: string,
  offset: number,
  limit: number
) => Promise<{ files: IFile[]; total: number }>
export type IpcSearchDir = (
  deviceId: string,
  path: string,
  keyword: string
) => Promise<{ files: IFile[]; total: number }>
export type IpcCreateDir = (deviceId: string, path: string) => Promise<void>
export type IpcDeleteDir = IpcCreateDir
export type IpcDeleteFile = IpcCreateDir
export type IpcOpenFile = IpcCreateDir
export type IpcPushFile = (
  deviceId: string,
  src: string,
  dest: string
) => Promise<void>
export type IpcPullFile = (
  deviceId: string,
  src: string,
  dest: string
) => Promise<void>
export type IpcEnablePackage = (deviceId: string, pkg: string) => Promise<void>
export type IpcDisablePackage = IpcEnablePackage
export type IpcGetPackages = (
  deviceId: string,
  system?: boolean
) => Promise<string[]>
export type IpcInstallPackage = (
  deviceId: string,
  apkPath: string
) => Promise<void>
export type IpcUninstallPackage = (
  deviceId: string,
  pkg: string
) => Promise<void>
export type IpcStartPackage = (deviceId: string, pkg: string) => Promise<void>
export type IpcStopPackage = IpcStartPackage
export type IpcClearPackage = IpcStartPackage
export type IpcGetTopPackage = (deviceId: string) => Promise<{
  name: string
  pid: number
}>

export type IpcGetProcesses = (deviceId: string) => Promise<IProcess[]>
export type IpcGetFileUrl = (
  deviceId: string,
  path: string,
  port?: number
) => Promise<string>

export type IpcApmState = () => Promise<{
  loggedIn: boolean
  user: string
  phonenum: string
  frps: string
}>

export type IpcApmLogin = (
  user: string,
  pwd: string
) => Promise<{ ok: boolean; user?: string; error?: string }>

export type IpcApmSearch = (
  mac: string
) => Promise<{ macs: string[]; detail?: any; error?: string }>

export type IpcApmConnect = (
  mac: string,
  frps: string,
  isPm: boolean,
  pkg?: string
) => Promise<{ taskId: number }>

export type IpcApmCloseAdb = (mac: string) => Promise<{ taskId: number }>

export type IpcApmTask = (
  taskId: number
) => Promise<{ lines: string[]; done: boolean; ok: boolean | null }>

export type IpcScanLanDevices = () => Promise<{
  scanned: number
  found: { id: string; connected: boolean }[]
}>

export type IpcReverseWebCast = (
  deviceId: string,
  remote: string
) => Promise<{ ok: boolean; port: number; error?: string }>
export type IpcStartWebCast = (
  deviceId: string,
  args: string[]
) => Promise<{ ok: boolean; error?: string }>
export type IpcStopWebCast = (deviceId: string) => Promise<void>
export type IpcShowWebCast = (deviceId: string, name: string) => Promise<void>
export type IpcWaitWebCast = (
  deviceId: string,
  scid: string
) => Promise<{ ok: boolean }>
export type IpcWebCastDisplays = (
  deviceId: string
) => Promise<{ id: number; name: string }[]>
export type IpcInstallWebCastApk = (
  deviceId: string,
  apkPath: string
) => Promise<{ ok: boolean; message: string }>
export type IpcWebCastOnTop = (
  deviceId: string,
  onTop: boolean
) => Promise<void>
export type IpcResizeWebCast = (
  deviceId: string,
  videoWidth: number,
  videoHeight: number
) => Promise<void>

export type IpcRecordingStart = (
  deviceId: string
) => Promise<{ id: string; path: string }>
export type IpcRecordingChunk = (
  id: string,
  data: Uint8Array
) => Promise<void>
export type IpcRecordingStop = (id: string) => Promise<{ path: string }>

export type IpcRebootDevice = (deviceId: string) => Promise<void>

export type IpcDiskPrepare = (
  serial: string
) => Promise<{ ok: boolean; out: string }>

export type IpcDiskRun = (
  serial: string,
  args: string[]
) => Promise<{ ok: boolean; out: string }>

export type IpcDiskScanAll = (
  serial: string
) => Promise<{ ok: boolean; disks?: any[]; out?: string }>
export type IpcGetInputMethod = (deviceId: string) => Promise<string>
