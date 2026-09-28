#!/usr/bin/env node
/**
 * apex hooks 冒烟：三端 presync/posttool 契约 + fail-open。
 * 用法：node scripts/smoke_hooks.js            （默认打 http://api.ya.com）
 *       APEX_BASE=http://... node scripts/smoke_hooks.js
 * 退出码：0=全绿；1=有失败。
 */
'use strict';
const { spawnSync } = require('child_process');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
let pass = 0, fail = 0;
function ok(name) { pass++; console.log('PASS  ' + name); }
function bad(name, extra) { fail++; console.log('FAIL  ' + name + (extra ? ' — ' + extra : '')); }

function runHook(script, stdin, env) {
  const r = spawnSync(process.execPath, [path.join(ROOT, 'scripts', script)], {
    input: JSON.stringify(stdin),
    encoding: 'utf8',
    timeout: 15000,
    env: { ...process.env, ...(env || {}) },
  });
  return { code: r.status, out: (r.stdout || '').trim(), err: (r.stderr || '').trim() };
}

function parse(out) { try { return JSON.parse(out || '{}'); } catch (_) { return null; } }

// 1) Codex UserPromptSubmit → additionalContext
{
  const r = runHook('presync.js', { hook_event_name: 'UserPromptSubmit', prompt: '重构认证并发逻辑', cwd: ROOT });
  const j = parse(r.out);
  if (j && j.hookSpecificOutput && j.hookSpecificOutput.additionalContext) ok('codex UserPromptSubmit → additionalContext');
  else bad('codex UserPromptSubmit', r.out.slice(0, 200));
}

// 2) Claude UserPromptSubmit（带 transcript_path）→ additionalContext
{
  const r = runHook('presync.js', { hook_event_name: 'UserPromptSubmit', prompt: '重构认证并发逻辑', transcript_path: '/tmp/t.jsonl', cwd: ROOT });
  const j = parse(r.out);
  if (j && j.hookSpecificOutput && j.hookSpecificOutput.additionalContext) ok('claude UserPromptSubmit → additionalContext');
  else bad('claude UserPromptSubmit', r.out.slice(0, 200));
}

// 3) Cursor beforeSubmitPrompt → continue:true（并暂存上下文）
{
  const r = runHook('presync.js', { prompt: '重构认证并发逻辑', cwd: ROOT });
  const j = parse(r.out);
  if (j && j.continue === true) ok('cursor beforeSubmitPrompt → continue:true');
  else bad('cursor beforeSubmitPrompt', r.out.slice(0, 200));
}

// 4) Cursor postToolUse → additional_context（消费暂存）
{
  const r = runHook('posttool.js', { cwd: ROOT, tool_name: 'Read' });
  const j = parse(r.out);
  if (j && typeof j.additional_context === 'string' && j.additional_context.length > 0) ok('cursor postToolUse → additional_context');
  else bad('cursor postToolUse', r.out.slice(0, 200));
}

// 5) 二次 postToolUse：暂存已消费 → 不重复注入
{
  const r = runHook('posttool.js', { cwd: ROOT, tool_name: 'Read' });
  const j = parse(r.out);
  if (j && !j.additional_context) ok('cursor postToolUse 一次性消费（不重复注入）');
  else bad('cursor postToolUse 消费语义', r.out.slice(0, 200));
}

// 6) fail-open：服务端不可达 → 放行
{
  const env = { APEX_BASE: 'http://127.0.0.1:59999', APEX_PRESYNC_TIMEOUT_MS: '300' };
  const a = runHook('presync.js', { hook_event_name: 'UserPromptSubmit', prompt: 'x', cwd: ROOT }, env);
  const b = runHook('presync.js', { prompt: 'x', cwd: ROOT }, env);
  const ja = parse(a.out), jb = parse(b.out);
  if ((ja && !ja.decision) && jb && jb.continue === true) ok('fail-open（服务端不可达仍放行）');
  else bad('fail-open', (a.out + ' | ' + b.out).slice(0, 200));
}

// 7) 空 prompt → 放行
{
  const a = runHook('presync.js', { hook_event_name: 'UserPromptSubmit', prompt: '', cwd: ROOT });
  const ja = parse(a.out);
  if (ja && !ja.decision) ok('空 prompt 直接放行');
  else bad('空 prompt', a.out.slice(0, 200));
}

console.log('\n======== SUMMARY pass=' + pass + ' fail=' + fail + ' ========');
process.exit(fail === 0 ? 0 : 1);
