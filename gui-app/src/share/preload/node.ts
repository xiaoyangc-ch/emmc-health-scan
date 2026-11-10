import net from 'node:net'
import fs from 'node:fs'
import os from 'node:os'

const sockets = new Map<number, net.Socket>()
let socketSeq = 0

export default {
  connectStream: function (
    port: number,
    handlers: {
      onConnect: () => void
      onData: (data: Uint8Array) => void
      onClose: () => void
      onError: (message: string) => void
    }
  ): number {
    const id = ++socketSeq
    const socket = net.connect(port, '127.0.0.1')
    sockets.set(id, socket)
    socket.on('connect', () => handlers.onConnect())
    socket.on('data', (data: Buffer) => handlers.onData(new Uint8Array(data)))
    socket.on('close', () => {
      sockets.delete(id)
      handlers.onClose()
    })
    socket.on('error', (err: Error) => {
      sockets.delete(id)
      handlers.onError(err && err.message ? err.message : String(err))
    })
    return id
  },
  writeStream: function (id: number, data: Uint8Array) {
    const socket = sockets.get(id)
    if (socket) socket.write(data)
  },
  closeStream: function (id: number) {
    const socket = sockets.get(id)
    sockets.delete(id)
    if (socket) socket.destroy()
  },
  createServer: function (listener: (socket: net.Socket) => void): net.Server {
    const server = net.createServer((socket) => {
      listener({
        on(event: string, listener: (...args: any[]) => void) {
          socket.on(event, listener)
        },
        write(buffer: Uint8Array | string, cb?: (err?: Error | null) => void) {
          return socket.write(buffer, cb)
        },
      } as any)
    })

    return {
      listen(port: number) {
        server.listen(port)
      },
      close() {
        server.close()
      },
    } as any
  },
  writeFile: fs.promises.writeFile,
  readFile: fs.promises.readFile,
  existsSync: fs.existsSync,
  isDir: async function (file: string): Promise<boolean> {
    const stats = await fs.promises.stat(file)
    return stats.isDirectory()
  },
  isEmptyDir: async function (dir: string): Promise<boolean> {
    const files = await fs.promises.readdir(dir)
    return files.length === 0
  },
  mkdir: fs.promises.mkdir,
  rmdir: fs.promises.rmdir,
  tmpdir: () => {
    return os.tmpdir()
  },
}
