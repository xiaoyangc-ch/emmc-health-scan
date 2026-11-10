const pkg = await fs.readJson('package.json')
const electron = pkg.devDependencies.electron
delete pkg.devDependencies
pkg.devDependencies = {
  electron,
}
delete pkg.scripts
pkg.scripts = {
  start: 'electron main/index.js',
}
pkg.main = 'main/index.js'

await $`npm run build:main`
await $`npm run build:preload`
await $`npm run build:renderer`

// 只清要复制的两个目录：dist 里还有 vite 刚产出的 main/preload/renderer，
// 对整个 dist 做 emptyDir 会把本轮构建结果一起删掉
await fs.emptyDir('dist/build')
await fs.emptyDir('dist/resources')
await fs.copy('build', 'dist/build')
await fs.copy('resources', 'dist/resources')
cd('dist')

await fs.writeJson('package.json', pkg, {
  spaces: 2,
})

// --production 已弃用；并且在 dist 里生成 lockfile 没有意义
await $`npm i --omit=dev --no-audit --no-fund --no-package-lock`
