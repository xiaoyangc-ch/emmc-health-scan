import builder from 'electron-builder'
import fs from 'fs-extra'
import { cd } from 'zx'

cd('dist')

const pkg = await fs.readJson('package.json')

const config = {
  appId: pkg.appId,
  productName: pkg.productName,
  directories: {
    output: `../release/${pkg.version}`,
  },
  files: ['main', 'preload', 'renderer'],
  artifactName: '${productName}-${version}-${os}-${arch}.${ext}',
  extraResources: {
    from: 'resources',
    to: './',
    filter: ['**/*'],
  },
  nsis: {
    allowToChangeInstallationDirectory: true,
    oneClick: false,
    installerSidebar: 'build/installerSidebar.bmp',
  },
  win: {
    icon: 'build/icon.ico',
    target: [
      {
        target: 'nsis',
      },
    ],
  },
}

await builder.build({
  config,
})
