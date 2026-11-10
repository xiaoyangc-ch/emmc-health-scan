import { app, Menu, session } from 'electron'
import isMac from 'licia/isMac'
import isWindows from 'licia/isWindows'
import * as theme from 'share/main/lib/theme'
import * as language from 'share/main/lib/language'
import * as ipc from 'share/main/lib/ipc'
import pkg from '../../package.json'
import { setupTitlebar } from 'custom-electron-titlebar/main'
import * as mainWin from './window/main'
import * as scan from './lib/scan'
import { getSettingsStore } from './lib/store'
import log from 'share/common/log'

const logger = log('main')
logger.info('start')

if (!app.requestSingleInstanceLock()) {
  app.quit()
  process.exit(0)
}

app.setName(pkg.productName)
// 单功能工具：不挂应用菜单（custom titlebar 会镜像渲染应用菜单，置空后标题栏只剩标题）
Menu.setApplicationMenu(null)
// 本工具无主题设置界面：锁定深色（每次启动写回，兼容旧版安装残留的 theme:"system"）
const settingsStore = getSettingsStore()
settingsStore.set('theme', 'dark')

app.on('ready', () => {
  logger.info('app ready')
  if (!isMac && !isWindows) {
    session.defaultSession.setSpellCheckerLanguages([])
  }
  setupTitlebar()
  language.init()
  theme.init()
  ipc.init()
  scan.init()
  mainWin.showWin()
})

app.on('before-quit', () => {
  scan.stopAll()
})
