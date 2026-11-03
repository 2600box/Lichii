import type { ReportFields } from './domain.ts';

export const MAX_LOG_BYTES = 512 * 1024;

export function validateLog(text: string): string | null {
  if (new TextEncoder().encode(text).length > MAX_LOG_BYTES) return 'The log must be 512 KB or smaller. Trim it to the relevant session.';
  if (!text.trim()) return 'The log is empty.';
  if (/[\u0000-\u0008\u000b\u000c\u000e-\u001f\ufffd]/u.test(text)) return 'Only UTF-8 plain-text logs are accepted. Archives and binary files are not allowed.';
  if (/<\s*(?:!doctype|html|script|svg|iframe|object|embed|body|img|style|link)\b/i.test(text)) return 'HTML and SVG content is not allowed.';
  if (!/^\[\s*\d+\.\d+\]\s+[\w.]+\s+<(?:Info|Warning|Error|Debug|Critical|Trace)>/m.test(text)) return 'This does not look like a Lichii log. Choose eden_log.txt from the Log Folder.';
  return null;
}

export function extractLogFields(text: string): Partial<ReportFields> {
  const fields: Partial<ReportFields> = {};
  for (const line of text.split(/\r?\n/)) {
    if (!/^\[\s*\d+\.\d+\]\s+[\w.]+\s+<Info>\s/.test(line)) continue;
    const capture = (source: string, label: string) => {
      const pattern = new RegExp(`\\b${source}:[0-9]+:[A-Za-z0-9_:~]+: ${label}: (.{1,160})$`);
      return line.match(pattern)?.[1]?.trim();
    };
    const build = capture('qt_common\\.cpp', '(?:Lichii|Eden) Version');
    const cpu = capture('qt_common\\.cpp', 'Host CPU');
    const os = capture('qt_common\\.cpp', 'Host OS');
    const ram = capture('qt_common\\.cpp', 'Host RAM');
    const gpu = capture('renderer_vulkan\\.cpp', 'Device');
    const driver = capture('renderer_vulkan\\.cpp', 'Driver');
    const vulkan = capture('renderer_vulkan\\.cpp', 'Vulkan');
    const boot = line.match(/\bmain_window\.cpp:[0-9]+:[A-Za-z0-9_:~]+: Booting game: [0-9A-Fa-f]+ \| .+ \| (.{1,60})$/)?.[1];
    if (build) fields.build = build;
    if (cpu) fields.cpu = cpu;
    if (os) fields.os = os;
    if (ram) fields.ram = ram;
    if (gpu) fields.gpu = gpu;
    if (driver) fields.driver = driver;
    if (vulkan) fields.graphicsApi = 'Vulkan';
    if (boot) fields.gameVersion = boot;
  }
  return fields;
}
