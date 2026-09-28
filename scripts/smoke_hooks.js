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

/** 冒烟隔离：清掉上一轮残留的去重标记 / 暂存 / 调试日志（否则幂等与补块用例会串扰）。 */
function cleanHooksTmp() {
  const dir = require('os').tmpdir();
  for (const f of require('fs').readdirSync(dir)) {
    if (/^apex_postsync_.*\.mark$/.test(f) || /^apex_presync_.*\.json$/.test(f) || f === 'apex_postsync.debug') {
      try { require('fs').unlinkSync(require('path').join(dir, f)); } catch (_) {}
    }
  }
}
cleanHooksTmp();

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

function tfPath(kind) {
  const f = path.join(require('os').tmpdir(), 'apex_smoke_tf_' + kind + '.jsonl');
  const txt = kind === 'with-block'
    ? '答复如下。\n⚡ 规则 **1**/**2** 命中 · token **313 → 111**'
    : '答复如下：已把 presync 接到三端。';
  require('fs').writeFileSync(f, JSON.stringify({ type: 'assistant', message: { role: 'assistant', content: [{ type: 'text', text: txt }] } }) + '\n', 'utf8');
  return f;
}

// ---- postsync（回合结束入队）----
const os2 = require('os');
const fsm = require('fs');
const debugLog = path.join(os2.tmpdir(), 'apex_postsync.debug');
function debugReset() { try { fsm.unlinkSync(debugLog); } catch (_) {} }
function debugTail() { try { return fsm.readFileSync(debugLog, 'utf8').trim().split('\n').filter(Boolean); } catch (_) { return []; } }

// 8) Cursor afterAgentResponse → observe 入队
{
  debugReset();
  const r = runHook('postsync.js', { text: '这是本轮的最终答复：已把 gate 删除并接入三端 hook。' }, { APEX_POSTSYNC_DEBUG: '1' });
  const j = parse(r.out);
  const n = debugTail().length;
  if (j && n === 1) ok('cursor afterAgentResponse → observe 入队');
  else bad('cursor afterAgentResponse', 'out=' + r.out.slice(0, 80) + ' log=' + n);
}

// 9) 幂等：同内容二次调用不入队
{
  const before = debugTail().length;
  runHook('postsync.js', { text: '这是本轮的最终答复：已把 gate 删除并接入三端 hook。' }, { APEX_POSTSYNC_DEBUG: '1' });
  if (debugTail().length === before) ok('postsync 幂等（90s 内同内容只入队一次）');
  else bad('postsync 幂等', 'log grew');
}

// 10) Codex Stop + transcript_path → 从尾部取 assistant 文本并入队
{
  debugReset();
  const tf = path.join(os2.tmpdir(), 'apex_smoke_transcript.jsonl');
  fsm.writeFileSync(tf, [
    JSON.stringify({ type: 'user', message: { role: 'user', content: '重构认证并发逻辑' } }),
    JSON.stringify({ type: 'assistant', message: { role: 'assistant', content: [{ type: 'text', text: '已重构：抽出 TokenBucket 并把限流前移。' }] } }),
  ].join('\n') + '\n', 'utf8');
  const r = runHook('postsync.js', { hook_event_name: 'Stop', session_id: 'smoke-sess', transcript_path: tf, cwd: ROOT }, { APEX_POSTSYNC_DEBUG: '1' });
  const j = parse(r.out);
  const n = debugTail().length;
  if (j && n === 1) ok('codex Stop → transcript 尾部解析并入队');
  else bad('codex Stop', 'out=' + r.out.slice(0, 80) + ' log=' + n);
}

// 11) 防自循环：loop_count>0 / stop_hook_active 直接跳过
{
  debugReset();
  const a = runHook('postsync.js', { status: 'completed', loop_count: 1 }, { APEX_POSTSYNC_DEBUG: '1' });
  const b = runHook('postsync.js', { hook_event_name: 'Stop', stop_hook_active: true, transcript_path: '/nope' }, { APEX_POSTSYNC_DEBUG: '1' });
  const ja = parse(a.out), jb = parse(b.out);
  if (ja && jb && !('followup_message' in ja) && debugTail().length === 0) ok('防自循环（loop_count / stop_hook_active 跳过且不发 followup_message）');
  else bad('防自循环', (a.out + '|' + b.out).slice(0, 120));
}

// 12) fail-open：服务端不可达仍静默退出
{
  debugReset();
  const r = runHook('postsync.js', { text: 'x' }, { APEX_BASE: 'http://127.0.0.1:59999', APEX_POSTSYNC_TIMEOUT_MS: '300', APEX_POSTSYNC_DEBUG: '1' });
  const j = parse(r.out);
  if (r.code === 0 && j && Object.keys(j).length === 0 && debugTail().length === 0) ok('postsync fail-open（服务端不可达仍静默 exit 0）');
  else bad('postsync fail-open', 'code=' + r.code + ' out=' + r.out.slice(0, 80));
}

// ---- 回合后补块（opt-in，路线②）----
// 13) presync 暂存块 → postsync(Codex) 发现回复漏块 → decision:block + reason
{
  runHook('presync.js', { hook_event_name: 'UserPromptSubmit', prompt: '重构认证并发逻辑', cwd: ROOT });
  const r = runHook('postsync.js',
    { hook_event_name: 'Stop', session_id: 'smoke-block-1', cwd: ROOT, transcript_path: tfPath('no-block') },
    { APEX_POSTSYNC_BLOCK: '1' });
  const j = parse(r.out);
  if (j && j.decision === 'block' && typeof j.reason === 'string' && j.reason.includes('⚡ 规则')) ok('回合后补块 · Codex → decision:block + reason(含块)');
  else bad('回合后补块 Codex', r.out.slice(0, 160));
}

// 14) 模型已带块 → 只可能追加 📝/📥/📤，绝不重复贴整块
{
  const r = runHook('postsync.js',
    { hook_event_name: 'Stop', session_id: 'smoke-block-2', cwd: ROOT, transcript_path: tfPath('with-block') },
    { APEX_POSTSYNC_BLOCK: '1' });
  const j = parse(r.out);
  const blocked = !!(j && j.decision === 'block');
  const noDup = !blocked || typeof j.reason !== 'string' || !j.reason.includes('⚡ 规则');
  if (!blocked || noDup) ok('回合后补块 · 已含块则不重复块');
  else bad('回合后补块 已含', r.out.slice(0, 160));
}

// 15) Cursor → followup_message 带块
{
  runHook('presync.js', { prompt: '重构认证并发逻辑', cwd: ROOT });
  const r = runHook('postsync.js', { text: '本轮答复：已经完成重构。', cwd: ROOT }, { APEX_POSTSYNC_BLOCK: '1' });
  const j = parse(r.out);
  if (j && typeof j.followup_message === 'string' && j.followup_message.includes('⚡ 规则')) ok('回合后补块 · Cursor → followup_message(含块)');
  else bad('回合后补块 Cursor', r.out.slice(0, 160));
}

// 16) 显式关（APEX_POSTSYNC_BLOCK=0）：不发任何追加/新回合
{
  const r = runHook('postsync.js',
    { hook_event_name: 'Stop', session_id: 'smoke-block-3', cwd: ROOT, transcript_path: tfPath('no-block') },
    { APEX_POSTSYNC_BLOCK: '0' });
  const j = parse(r.out);
  if (j && !j.decision && !j.followup_message) ok('回合后补块 · 显式关（不产生额外回合）');
  else bad('回合后补块 关闭', r.out.slice(0, 160));
}

console.log('\n======== SUMMARY pass=' + pass + ' fail=' + fail + ' ========');
process.exit(fail === 0 ? 0 : 1);
