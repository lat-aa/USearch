#!/usr/bin/env node
/**
 * apex presync：主 LLM 之前的统一前置钩子（三端）。
 *
 * 实测/官方契约：
 *   Codex  UserPromptSubmit   → { hookSpecificOutput: { hookEventName, additionalContext } }   ✅ 可注入
 *   Claude UserPromptSubmit   → 同上                                                          ✅ 可注入
 *   Cursor beforeSubmitPrompt → { continue, user_message(仅阻断时) }                            ❌ 不能注入
 *        → Cursor 的注入改由 postToolUse.additional_context 完成（见 scripts/posttool.js）
 *
 * 红线：fail-open。任何异常都必须放行，绝不阻塞用户输入。
 */
'use strict';
const http = require('http');
const https = require('https');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { URL } = require('url');

const TIMEOUT_MS = Number(process.env.APEX_PRESYNC_TIMEOUT_MS || 4000);
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

function detectClient(raw) {
  if (raw && raw.hook_event_name === 'UserPromptSubmit') {
    // Claude 带 session_id + transcript_path；Codex 带 session_id/cwd
    if (raw.transcript_path || raw.permission_mode) return 'claude';
    return 'codex';
  }
  if (raw && raw.prompt !== undefined) return 'cursor';
  return 'codex';
}

function extractTask(raw) {
  if (raw && typeof raw.prompt === 'string') return raw.prompt;
  if (raw && typeof raw.user_prompt === 'string') return raw.user_prompt;
  if (raw && typeof raw.text === 'string') return raw.text;
  return '';
}

function extractFiles(raw) {
  const files = [];
  const atts = raw && raw.attachments;
  if (Array.isArray(atts)) for (const a of atts) if (a && a.file_path) files.push(String(a.file_path));
  return files;
}

function extractActual(raw) {
  if (raw && typeof raw.model === 'string' && raw.model) return raw.model;
  if (raw && typeof raw.model_id === 'string' && raw.model_id) return raw.model_id;
  return '';
}

function stashPath(raw) {
  const key = String((raw && (raw.cwd || raw.session_id || raw.conversation_id)) || 'default').replace(/[^A-Za-z0-9._-]/g, '_');
  return path.join(os.tmpdir(), 'apex_presync_' + key + '.json');
}

function post(base, token, payload) {
  return new Promise((resolve, reject) => {
    let url;
    try {
      url = new URL('/v1/presync', base.endsWith('/') ? base : base + '/');
    } catch (e) {
      return reject(e);
    }
    const body = JSON.stringify(payload);
    const lib = url.protocol === 'https:' ? https : http;
    const req = lib.request(
      {
        hostname: url.hostname,
        port: url.port || (url.protocol === 'https:' ? 443 : 80),
        path: url.pathname,
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          Accept: 'application/json',
          Authorization: 'Bearer ' + token,
          'Content-Length': Buffer.byteLength(body),
        },
      },
      (res) => {
        let data = '';
        res.setEncoding('utf8');
        res.on('data', (c) => (data += c));
        res.on('end', () => {
          if (res.statusCode !== 200) return reject(new Error('HTTP ' + res.statusCode));
          try {
            resolve(JSON.parse(data));
          } catch (e) {
            reject(e);
          }
        });
      }
    );
    req.on('error', reject);
    req.setTimeout(TIMEOUT_MS, () => req.destroy(new Error('presync timeout')));
    req.write(body);
    req.end();
  });
}

function buildInjection(r) {
  const parts = [];
  const mem = Array.isArray(r.memory) ? r.memory : [];
  const rules = Array.isArray(r.rules) ? r.rules : [];
  if (rules.length || mem.length) {
    parts.push('本地知识（apex 服务端检索，本轮参考，勿逐字复述）：');
    if (rules.length) {
      parts.push('【规则】');
      for (const x of rules) parts.push('### ' + x.name + '\n' + (x.body || ''));
    }
    if (mem.length) {
      parts.push('【历史经验】');
      for (const m of mem) parts.push('- (' + m.id + ') ' + String(m.text || '').slice(0, 400));
    }
  }
  if (r.block) parts.push('【统计块】本轮回复末尾请原样附上以下内容：\n' + r.block);
  return parts.join('\n');
}

async function main() {
  let raw = {};
  try {
    const stdin = await readStdin();
    raw = JSON.parse(stdin.replace(/^\uFEFF/, ''));
  } catch (_) {
    raw = {};
  }
  const client = detectClient(raw);
  const task = extractTask(raw);

  // 无任务：放行
  if (!task) {
    process.stdout.write(client === 'cursor' ? JSON.stringify({ continue: true }) : '{}');
    return;
  }

  const base = process.env.APEX_BASE || process.env.USEARCH_BASE || 'http://api.ya.com';
  const token = process.env.APEX_TOKEN || process.env.USEARCH_TOKEN || 'sk-default';

  let r;
  try {
    r = await post(base, token, {
      task,
      files: extractFiles(raw),
      client,
      actual_model: extractActual(raw),
    });
  } catch (_) {
    // fail-open：服务端不可用/超时 → 直接放行
    process.stdout.write(client === 'cursor' ? JSON.stringify({ continue: true }) : '{}');
    return;
  }

  const ctx = buildInjection(r);

  // 三端都暂存：Cursor 用它做 postToolUse 注入；所有端都用它给 postsync 提供 block（回合后补块）
  try {
    fs.writeFileSync(stashPath(raw), JSON.stringify({ ts: Date.now(), ctx, task, block: r.block || '' }), 'utf8');
  } catch (_) {}

  if (client === 'cursor') {
    // Cursor 此处不能注入：上下文交给 postToolUse
    process.stdout.write(JSON.stringify({ continue: true }));
    return;
  }

  // Codex / Claude：直接注入
  process.stdout.write(
    JSON.stringify({
      hookSpecificOutput: { hookEventName: 'UserPromptSubmit', additionalContext: ctx },
    })
  );
}

main().catch(() => {
  process.stdout.write('{}');
});
