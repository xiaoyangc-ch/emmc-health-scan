import { BrowserWindow } from 'electron'
import * as window from '../lib/window'

let win: BrowserWindow | null = null

export function showWin() {
  if (win) {
    win.focus()
    return
  }

  win = window.create({
    name: 'about',
    resizable: false,
    minWidth: 460,
    minHeight: 520,
    width: 460,
    height: 560,
  })

  win.on('close', () => {
    win?.destroy()
    win = null
  })

  window.loadPage(win, {
    page: 'about',
  })
}
