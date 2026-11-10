import { createRoot } from 'react-dom/client'
import 'luna-toolbar/css'
import 'luna-data-grid/css'
import 'share/renderer/luna.scss'
import 'share/renderer/main.scss'
import './icon.css'
import App from './scan/App'

const container = document.getElementById('app') as HTMLElement
createRoot(container).render(<App />)
