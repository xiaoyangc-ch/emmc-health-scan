import { defineConfig } from 'vite'
import { resolve } from 'path'
import { builtinModules } from 'node:module'
import { alias } from './vite.config'

const builtins = builtinModules.filter((e) => !e.startsWith('_'))
builtins.push('electron', ...builtins.map((m) => `node:${m}`))

export default defineConfig({
  build: {
    // 库里没开压缩时 main 会到 3.8 MB；压缩后 Electron 启动解析更快、asar 更小
    minify: 'esbuild',
    target: 'node20',
    outDir: 'dist/main',
    lib: {
      entry: resolve(__dirname, 'src/main/index.ts'),
      name: 'Main',
      fileName: 'index',
      formats: ['cjs'],
    },
    rollupOptions: {
      external: builtins,
    },
  },
  resolve: {
    mainFields: ['main', 'module'],
    alias,
  },
})
