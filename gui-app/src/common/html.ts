const HTML_ESC: Record<string, string> = {
  '&': '&amp;',
  '<': '&lt;',
  '>': '&gt;',
  '"': '&quot;',
  "'": '&#39;',
}

/**
 * 转义要交给 innerHTML / LunaNotification(.html()) 的文本。
 * 设备名、包名、文件路径都来自设备端，属于不可信输入。
 */
export function escapeHtml(s: string) {
  return String(s).replace(/[&<>"']/g, (c) => HTML_ESC[c])
}
