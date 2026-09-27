#!/usr/bin/env node
/**
 * Cursor / Codex PreToolUse：把本轮真实模型写入 cost 入参。
 * Cursor 权威源是 hook 的 model / model_id；Codex 权威源是 hook 的 model（config 选择器）。
 * 禁止读本地库、禁止发明 UI 营销名。
 */
'use strict';

function readStdin() {
  return new Promise((resolve, reject) => {
    const chunks = [];
    process.stdin.setEncoding('utf8');
    process.stdin.on('data', (c) => chunks.push(c));
    process.stdin.on('end', () => resolve(chunks.join('')));
    process.stdin.on('error', reject);
  });
}

function formatActual(model, modelId, params) {
  const label = String(model || modelId || '').trim();
  if (!label) return '';
  const extras = [];
  const lower = label.toLowerCase();
  for (const p of params || []) {
    if (!p || typeof p !== 'object') continue;
    const id = String(p.id || '');
    const val = String(p.value || '').trim();
    if (!val) continue;
    if (id !== 'effort' && id !== 'thinking' && id !== 'mode') continue;
    if (lower.includes(val.toLowerCase())) continue;
    extras.push(val);
  }
  return extras.length ? `${label} (${extras.join(', ')})` : label;
}

function parseToolInput(raw) {
  if (raw == null) return {};
  if (typeof raw === 'object' && !Array.isArray(raw)) return { ...raw };
  if (typeof raw === 'string') {
    try {
      const j = JSON.parse(raw);
      return j && typeof j === 'object' && !Array.isArray(j) ? j : {};
    } catch {
      return {};
    }
  }
  return {};
}

function isCodex(payload) {
  const ev = String(payload.hook_event_name || '');
  return ev === 'PreToolUse' || ev === 'pre_tool_use';
}

function isCostName(n) {
  return n === 'cost' || n === 'MCP:cost' || /(?:^|[\/:])cost$/i.test(n) || /__cost$/i.test(n);
}

function costTarget(toolName, toolInput) {
  const n = String(toolName || '');
  if (isCostName(n)) return { kind: 'flat', input: toolInput };
  const dyn = /^(CallDynamicTool|CallMcpTool|call_mcp_tool)$/i.test(n);
  if (!dyn) return null;
  const tn = String(toolInput.toolName || toolInput.tool_name || '');
  const ns = String(toolInput.namespace || '');
  if (tn !== 'cost') return null;
  if (ns && ns !== 'user-apex' && ns !== 'apex') return null;
  const args =
    toolInput.arguments && typeof toolInput.arguments === 'object' ? { ...toolInput.arguments } : {};
  return { kind: 'dyn', input: toolInput, args };
}

(async () => {
  let payload = {};
  try {
    const text = await readStdin();
    payload = text ? JSON.parse(text) : {};
  } catch {
    process.stdout.write(JSON.stringify({ permission: 'allow' }));
    return;
  }

  const toolInput = parseToolInput(payload.tool_input);
  const target = costTarget(payload.tool_name, toolInput);
  if (!target) {
    process.stdout.write(JSON.stringify({ permission: 'allow' }));
    return;
  }

  const actual = formatActual(payload.model, payload.model_id, payload.model_params);
  if (!actual) {
    process.stdout.write(JSON.stringify({ permission: 'allow' }));
    return;
  }

  const source = isCodex(payload) ? 'codex-config' : 'cursor-state';
  let updated;
  if (target.kind === 'dyn') {
    updated = {
      ...target.input,
      arguments: { ...target.args, actual_model: actual, actual_model_source: source },
    };
  } else {
    updated = { ...target.input, actual_model: actual, actual_model_source: source };
  }

  if (isCodex(payload)) {
    // Codex：updatedInput 就是 MCP 参数对象（不是 Cursor 的整包 tool_input）。
    const args = target.kind === 'dyn' ? updated.arguments : updated;
    process.stdout.write(
      JSON.stringify({
        hookSpecificOutput: {
          hookEventName: 'PreToolUse',
          permissionDecision: 'allow',
          updatedInput: args,
        },
      }),
    );
    return;
  }

  process.stdout.write(JSON.stringify({ permission: 'allow', updated_input: updated }));
})().catch(() => {
  process.stdout.write(JSON.stringify({ permission: 'allow' }));
});
