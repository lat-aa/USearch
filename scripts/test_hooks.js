#!/usr/bin/env node
/**
 * L3：无 IDE 模拟 Cursor / Codex / Claude 的 presync + injectmodel。
 * Usage: node scripts/test_hooks.js
 *
 * presync 默认走 APEX_GATE_FIXTURE（避免部分环境子进程连 localhost 挂起）；
 * 设 USE_REAL_APEX=1 且配置 APEX_BASE 时打真实 /v1/gate。
 */
'use strict';

const { spawnSync } = require('child_process');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const PRESYNC = path.join(ROOT, 'scripts', 'presync.js');
const INJECT = path.join(ROOT, 'scripts', 'injectmodel.js');

let failed = 0;
let passed = 0;

function pass(name) {
  console.log('PASS  ' + name);
  passed++;
}
function bad(name, detail) {
  console.log('FAIL  ' + name + ' — ' + detail);
  failed++;
}

function runNode(script, stdinObj, envExtra) {
  const r = spawnSync(process.execPath, [script], {
    input: JSON.stringify(stdinObj),
    encoding: 'utf8',
    env: { ...process.env, ...envExtra },
    timeout: 15000,
  });
  if (r.error) throw r.error;
  if (r.status !== 0) {
    throw new Error('exit ' + r.status + ' stderr=' + (r.stderr || '').slice(0, 300));
  }
  const out = (r.stdout || '').trim();
  try {
    return JSON.parse(out || '{}');
  } catch (e) {
    throw new Error('non-json stdout: ' + out.slice(0, 200));
  }
}

function main() {
  const real = process.env.USE_REAL_APEX === '1';
  const base = process.env.APEX_BASE || process.env.USEARCH_BASE || 'http://127.0.0.1:8088';
  const token = process.env.APEX_TOKEN || process.env.USEARCH_TOKEN || process.env.TOKEN || 'sk-default';

  function envFor(fixture) {
    const e = { APEX_BASE: base, APEX_TOKEN: token, USEARCH_BASE: base, USEARCH_TOKEN: token };
    if (!real) e.APEX_GATE_FIXTURE = fixture;
    return e;
  }

  // --- presync: Cursor pack → continue ---
  try {
    const out = runNode(PRESYNC, { prompt: 'hello pack please', continue: true }, envFor('pack'));
    if (out.continue === true && !out.user_message) pass('presync cursor pack → continue');
    else bad('presync cursor pack', JSON.stringify(out));
  } catch (e) {
    bad('presync cursor pack', String(e.message || e));
  }

  // --- presync: Cursor answered → continue:false ---
  try {
    const out = runNode(PRESYNC, { prompt: 'answer me', attachments: [] }, envFor('answered'));
    if (out.continue === false && String(out.user_message || '').includes('hook-answered'))
      pass('presync cursor answered → block');
    else bad('presync cursor answered', JSON.stringify(out));
  } catch (e) {
    bad('presync cursor answered', String(e.message || e));
  }

  // --- presync: Codex refuse → decision block ---
  try {
    const out = runNode(
      PRESYNC,
      { hook_event_name: 'UserPromptSubmit', prompt: 'refuse me' },
      envFor('refuse'),
    );
    if (out.decision === 'block' && String(out.reason || '').includes('refuse'))
      pass('presync codex refuse → block');
    else bad('presync codex refuse', JSON.stringify(out));
  } catch (e) {
    bad('presync codex refuse', String(e.message || e));
  }

  // --- presync: Claude pack → additionalContext ---
  try {
    const out = runNode(
      PRESYNC,
      { hook_event_name: 'UserPromptSubmit', session_id: 's1', cwd: '/tmp', prompt: 'pack path' },
      envFor('pack'),
    );
    const ctx = out.hookSpecificOutput && out.hookSpecificOutput.additionalContext;
    if (ctx && String(ctx).includes('gate pack')) pass('presync claude pack → context');
    else bad('presync claude pack', JSON.stringify(out));
  } catch (e) {
    bad('presync claude pack', String(e.message || e));
  }

  // --- presync: fail-open ---
  try {
    const out = runNode(PRESYNC, { prompt: 'x', continue: true }, envFor('error'));
    if (out.continue === true) pass('presync cursor fail-open');
    else bad('presync fail-open', JSON.stringify(out));
  } catch (e) {
    bad('presync fail-open', String(e.message || e));
  }

  // --- injectmodel: Cursor cost ---
  try {
    const out = runNode(INJECT, {
      tool_name: 'cost',
      model: 'Composer',
      model_id: 'composer-1',
      tool_input: { task: 'x', files: [] },
    });
    if (
      out.permission === 'allow' &&
      out.updated_input &&
      out.updated_input.actual_model === 'Composer' &&
      out.updated_input.actual_model_source === 'cursor-state'
    )
      pass('injectmodel cursor cost');
    else bad('injectmodel cursor cost', JSON.stringify(out));
  } catch (e) {
    bad('injectmodel cursor cost', String(e.message || e));
  }

  // --- injectmodel: Codex PreToolUse ---
  try {
    const out = runNode(INJECT, {
      hook_event_name: 'PreToolUse',
      tool_name: 'mcp__apex__cost',
      model: 'deepseek-flash',
      tool_input: { task: 'y' },
    });
    const ui = out.hookSpecificOutput && out.hookSpecificOutput.updatedInput;
    if (
      out.hookSpecificOutput &&
      out.hookSpecificOutput.permissionDecision === 'allow' &&
      ui &&
      ui.actual_model === 'deepseek-flash' &&
      ui.actual_model_source === 'codex-config'
    )
      pass('injectmodel codex cost');
    else bad('injectmodel codex cost', JSON.stringify(out));
  } catch (e) {
    bad('injectmodel codex cost', String(e.message || e));
  }

  // --- injectmodel: CallDynamicTool cost ---
  try {
    const out = runNode(INJECT, {
      tool_name: 'CallDynamicTool',
      model: 'Grok 4.6',
      tool_input: { namespace: 'user-apex', toolName: 'cost', arguments: { task: 'z' } },
    });
    if (
      out.updated_input &&
      out.updated_input.arguments &&
      out.updated_input.arguments.actual_model === 'Grok 4.6' &&
      out.updated_input.arguments.actual_model_source === 'cursor-state'
    )
      pass('injectmodel CallDynamicTool cost');
    else bad('injectmodel CallDynamicTool', JSON.stringify(out));
  } catch (e) {
    bad('injectmodel CallDynamicTool', String(e.message || e));
  }

  // --- injectmodel: non-cost → allow 原样 ---
  try {
    const out = runNode(INJECT, { tool_name: 'rules', model: 'x', tool_input: { task: 't' } });
    if (out.permission === 'allow' && !out.updated_input) pass('injectmodel non-cost allow');
    else bad('injectmodel non-cost', JSON.stringify(out));
  } catch (e) {
    bad('injectmodel non-cost', String(e.message || e));
  }

  console.log('');
  console.log('======== SUMMARY pass=' + passed + ' fail=' + failed + ' ========');
  if (failed) process.exit(1);
  console.log('OK');
}

main();
