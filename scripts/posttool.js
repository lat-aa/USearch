#!/usr/bin/env node
/**
 * apex posttool：Cursor 专用注入桥。
 *
 * Cursor 的 beforeSubmitPrompt 只能 continue/user_message（不能注入上下文），
 * 但 postToolUse 支持 additional_context —— 因此：
 *   presync(beforeSubmitPrompt) 把上下文暂存 → 本脚本在**首次工具调用后**注入并消费。
 *
 * 契约：输入含 cwd；输出 { additional_context }。fail-open（无暂存/异常一律空输出）。
 */
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');

const STASH_TTL_MS = 120000;

function readStdin() {
  return new Promise((resolve) => {
    const chunks = [];
    process.stdin.setEncoding('utf8');
    process.stdin.on('data', (c) => chunks.push(c));
    process.stdin.on('end', () => resolve(chunks.join('')));
    process.stdin.on('error', () => resolve(''));
  });
}

function stashPath(key) {
  const safe = String(key || 'default').replace(/[^A-Za-z0-9._-]/g, '_');
  return path.join(os.tmpdir(), 'apex_presync_' + safe + '.json');
}

(async () => {
  let raw = {};
  try {
    const s = await readStdin();
    raw = JSON.parse(s.replace(/^\uFEFF/, ''));
  } catch (_) {}
  const key = raw.cwd || raw.session_id || raw.conversation_id || 'default';
  const p = stashPath(key);
  try {
    const st = JSON.parse(fs.readFileSync(p, 'utf8'));
    fs.unlinkSync(p); // 一次性消费
    if (!st || !st.ctx) throw new Error('empty');
    if (Date.now() - Number(st.ts || 0) > STASH_TTL_MS) throw new Error('stale');
    process.stdout.write(JSON.stringify({ additional_context: st.ctx }));
  } catch (_) {
    process.stdout.write('{}');
  }
})();
