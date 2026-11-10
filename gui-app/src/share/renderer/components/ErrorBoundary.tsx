import { Component, ErrorInfo, PropsWithChildren } from 'react'
import log from 'share/common/log'
import Style from './ErrorBoundary.module.scss'

const logger = log('renderer')

type Props = PropsWithChildren<{ name?: string }>
type State = { error: Error | null }

// 单个页面渲染崩溃时只坏这一页，工具栏和其他页面继续可用
export default class ErrorBoundary extends Component<Props, State> {
  state: State = { error: null }

  static getDerivedStateFromError(error: Error): State {
    return { error }
  }

  componentDidCatch(error: Error, info: ErrorInfo) {
    logger.error('render crash:', this.props.name, error, info.componentStack)
  }

  render() {
    const { error } = this.state
    if (!error) return this.props.children

    return (
      <div className={Style.container}>
        <div className={Style.title}>此页面出错</div>
        <div className={Style.detail}>{String(error.message || error)}</div>
        <button
          className={Style.retry}
          onClick={() => this.setState({ error: null })}
        >
          重试
        </button>
      </div>
    )
  }
}
