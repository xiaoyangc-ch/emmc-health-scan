import memoize from 'licia/memoize'
import FileStore from 'licia/FileStore'
import { getUserDataPath } from 'share/main/lib/util'

export const getMainStore = memoize(function () {
  return new FileStore(getUserDataPath('data/main.json'), {})
})

export const getSettingsStore = memoize(function () {
  return new FileStore(getUserDataPath('data/settings.json'), {
    language: 'system',
    theme: 'dark',
    useNativeTitlebar: false,
  })
})
