import { contextBridge, ipcRenderer } from 'electron'
import mainObj from './main'
import preloadObj from 'share/preload/preload'

contextBridge.exposeInMainWorld('preload', preloadObj)
contextBridge.exposeInMainWorld('main', mainObj)

declare global {
  const main: typeof mainObj
  const preload: typeof preloadObj
}
