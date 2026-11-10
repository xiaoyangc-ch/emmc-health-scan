import contain from 'licia/contain'
import h from 'licia/h'
import isArrBuffer from 'licia/isArrBuffer'
import convertBin from 'licia/convertBin'
import { isObservable, toJS } from 'mobx'
import LunaNotification, { INotifyOptions } from 'luna-notification'
// LunaNotification 内部用 .html() 渲染 content（webSecurity:false 下等于 XSS），
// 而 content 里经常拼设备名/包名/文件路径，必须转义
import { escapeHtml } from 'common/html'

let notification: LunaNotification | null = null

export function notify(content: string, options?: INotifyOptions) {
  if (!notification) {
    const div = h('div')
    document.body.appendChild(div)
    notification = new LunaNotification(div, {
      position: {
        x: 'center',
        y: 'top',
      },
    })
  }

  notification.notify(escapeHtml(content), options)
}

export async function setMemStore(name: string, val: any) {
  await main.setMemStore(name, isObservable(val) ? toJS(val) : val)
}

export function isFileDrop(e: React.DragEvent) {
  return contain(e.dataTransfer.types, 'Files')
}

export function copyData(buf: any, mime: string) {
  if (!isArrBuffer(buf)) {
    buf = convertBin(buf, 'ArrayBuffer')
  }
  navigator.clipboard.write([
    new ClipboardItem({
      [mime]: new Blob([buf], {
        type: mime,
      }),
    }),
  ])
}

