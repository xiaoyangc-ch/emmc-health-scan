import { app, BrowserWindow, session } from 'electron'
import once from 'licia/once'
import { handleEvent } from 'share/main/lib/util'
import * as window from 'share/main/lib/window'
import { getMainStore } from '../lib/store'
import { getSettingsStore } from '../lib/store'
import log from 'share/common/log'
import { IpcGetStore, IpcSetStore } from 'share/common/types'

const logger = log('mainWin')

const store = getMainStore()

let win: BrowserWindow | null = null

export function showWin() {
  logger.info('show')

  if (win && !win.isDestroyed()) {
    win.focus()
    return
  }

  init()
  initIpc()

  win = window.create({
    name: 'main',
    minWidth: 900,
    minHeight: 560,
    width: 1000,
    height: 660,
    menu: false,
  })

  const w = win
  w.on('close', () => {
    app.quit()
  })
  w.on('closed', () => {
    if (win === w) win = null
  })

  window.loadPage(w)
}

const init = once(() => {
  const settingsStore = getSettingsStore()
  window.setDefaultOptions({
    customTitlebar: !settingsStore.get('useNativeTitlebar'),
  })
  if (process.env.NODE_ENV === 'development') {
    session.defaultSession.webRequest.onBeforeSendHeaders(
      { urls: ['ws://*/*'] },
      (details, callback) => {
        delete details.requestHeaders['Origin']
        callback({ requestHeaders: details.requestHeaders })
      }
    )
  }
})

const initIpc = once(() => {
  handleEvent('setMainStore', <IpcSetStore>((name, val) => store.set(name, val)))
  handleEvent('getMainStore', <IpcGetStore>((name) => store.get(name)))
  store.on('change', (name, val) => {
    window.sendAll('changeMainStore', name, val)
  })
})
