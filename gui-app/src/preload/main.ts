import { ipcRenderer } from 'electron'
import types from 'licia/types'
import { invoke } from 'share/preload/util'

const mainObj = {
  toggleDevTools: () => ipcRenderer.invoke('toggleDevTools'),
  closeWin: () => ipcRenderer.invoke('closeWin'),
  hideWin: () => ipcRenderer.invoke('hideWin'),
  toggleWinMaximize: () => ipcRenderer.invoke('toggleWinMaximize'),
  getLanguage: invoke<() => Promise<string>>('getLanguage'),
  getTheme: invoke<() => Promise<string>>('getTheme'),
  isCustomTitlebar: invoke<() => Promise<boolean>>('isCustomTitlebar'),
  on: (
    channel: 'scanEvent' | 'changeTheme' | 'changeSettingsStore',
    listener: types.AnyFn
  ) => {
    ipcRenderer.on(channel, (_event, ...args) => listener(...args))
  },
  scanDetectSubnets: invoke<() => Promise<string[]>>('scanDetectSubnets'),
  scanStart: invoke<
    (
      opts: {
        subnets: string[]
        hosts: string[]
        port: number
        timeoutMs: number
        keepRemote: boolean
        noReportFallback?: boolean
        adb?: string
      }
    ) => Promise<{ ok: boolean; found: number; okCount: number; reportDir: string; message: string }>
  >('scanStart'),
  scanStop: invoke<() => Promise<boolean>>('scanStop'),
  scanOpenReports: invoke<() => Promise<boolean>>('scanOpenReports'),
}

export default mainObj
