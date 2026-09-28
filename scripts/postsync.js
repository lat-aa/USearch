#!/usr/bin/env node
/**
 * apex postsync：回合结束的沉淀入队（三端）。
 *
 * 实测契约：
 *   Codex/Claude Stop            → 输入 {session_id, transcript_path, cwd, stop_hook_active}
 *   Cursor afterAgentResponse    → 输入 {text: "<assistant 最终文本>"}（无输出字段）
 *   Cursor stop                  → 输入 {status, loop_count}；输出可 followup_message ⚠️ 绝不使用（会自循环）
 *
 * 职责边界（红线）：
 *   1) **只入队**（MCP observe，毫秒级）；蒸馏永远是服务端 Worker 的异步活
 *   2) **fail-open**：任何异常/超时都静默退出 0，绝不阻塞客户端回合结束
 *   3) **绝不输出** followup_message / decision:block（防自循环）
 *   4) 幂等：同一 (client,session,内容哈希) 90s 内只入队一次
 */
'use strict';
const http = require('http');
const https = require('https');
const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const { URL } = require('url');

const TIMEOUT_MS = Number(process.env.APEX_POSTSYNC_TIMEOUT_MS || 1500);
const DEDUPE_TTL_MS = 90000;
const MAX_SUMMARY = 800;
const TAIL_BYTES = 64 * 1024;

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
  if (raw && typeof raw.text === 'string' && raw.hook_event_name === undefined) return 'cursor';
  if (raw && raw.hook_event_name === 'Stop') {
    if (raw.transcript_path || raw.permission_mode) return 'claude';
    return 'codex';
  }
  if (raw && raw.transcript_path) return 'codex';
  return 'cursor';
}

/** 从 transcript 尾部（有界）取最后一条 assistant 文本。 */
function lastAssistantFromTranscript(p) {
  try {
    const st = fs.statSync(p);
    const start = Math.max(0, st.size - TAIL_BYTES);
    const fd = fs.openSync(p, 'r');
    const buf = Buffer.alloc(st.size - start);
    fs.readSync(fd, buf, 0, buf.length, start);
    fs.closeSync(fd);
    const lines = buf.toString('utf8').split(/\r?\n/);
    for (let i = lines.length - 1; i >= 0; i--) {
      const ln = lines[i].trim();
      if (!ln || ln[0] !== '{') continue;
      let o;
      try {
        o = JSON.parse(ln);
      } catch (_) {
        continue;
      }
      const msg = o.message || o;
      const role = msg.role || o.type;
      if (role !== 'assistant') continue;
      const c = msg.content;
      if (typeof c === 'string' && c.trim()) return c;
      if (Array.isArray(c)) {
        const t = c.map((x) => (typeof x === 'string' ? x : x && x.text ? x.text : '')).join('');
        if (t.trim()) return t;
      }
    }
  } catch (_) {}
  return '';
}

function stashPath(key) {
  const safe = String(key || 'default').replace(/[^A-Za-z0-9._-]/g, '_');
  return path.join(os.tmpdir(), 'apex_presync_' + safe + '.json');
}
function takeStash(key) {
  try {
    const p = stashPath(key);
    return JSON.parse(fs.readFileSync(p, 'utf8')) || {};
  } catch (_) {
    return {};
  }
}

function dedupeHit(key) {
  try {
    const p = path.join(os.tmpdir(), 'apex_postsync_' + key + '.mark');
    if (fs.existsSync(p) && Date.now() - fs.statSync(p).mtimeMs < DEDUPE_TTL_MS) return true;
    fs.writeFileSync(p, String(Date.now()), 'utf8');
  } catch (_) {}
  return false;
}

function postMcp(base, token, name, args) {
  return new Promise((resolve, reject) => {
    let url;
    try {
      url = new URL('/mcp', base.endsWith('/') ? base : base + '/');
    } catch (e) {
      return reject(e);
    }
    const body = JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'tools/call', params: { name, arguments: args } });
    const lib = url.protocol === 'https:' ? https : http;
    const req = lib.request(
      {
        hostname: url.hostname,
        port: url.port || (url.protocol === 'https:' ? 443 : 80),
        path: url.pathname,
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          Accept: 'application/json, text/event-stream',
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
          if (data.startsWith('event:')) {
            for (const ln of data.split(/\r?\n/)) if (ln.startsWith('data:')) data = ln.slice(5).trim();
          }
          try {
            resolve(JSON.parse(data));
          } catch (e) {
            reject(e);
          }
        });
      }
    );
    req.on('error', reject);
    req.setTimeout(TIMEOUT_MS, () => req.destroy(new Error('postsync timeout')));
    req.write(body);
    req.end();
  });
}

function getLastcall(base, token) {
  return new Promise((resolve) => {
    let url;
    try {
      url = new URL('/v1/lastcall', base.endsWith('/') ? base : base + '/');
    } catch (_) {
      return resolve(null);
    }
    const lib = url.protocol === 'https:' ? https : http;
    const req = lib.request(
      {
        hostname: url.hostname,
        port: url.port || (url.protocol === 'https:' ? 443 : 80),
        path: url.pathname,
        method: 'GET',
        headers: { Accept: 'application/json', Authorization: 'Bearer ' + token },
      },
      (res) => {
        let data = '';
        res.setEncoding('utf8');
        res.on('data', (c) => (data += c));
        res.on('end', () => {
          try {
            resolve(res.statusCode === 200 ? JSON.parse(data) : null);
          } catch (_) {
            resolve(null);
          }
        });
      }
    );
    req.on('error', () => resolve(null));
    req.setTimeout(TIMEOUT_MS, () => req.destroy(new Error('lastcall timeout')));
    req.end();
  });
}

(async () => {
  let raw = {};
  try {
    const s = await readStdin();
    if (s.trim()) raw = JSON.parse(s.replace(/^\uFEFF/, ''));
  } catch (_) {
    process.stdout.write('{}');
    return;
  }
  // 防自循环：Claude/Codex 的 stop_hook_active；Cursor 的 loop_count>0
  if (raw.stop_hook_active === true || Number(raw.loop_count || 0) > 0) {
    process.stdout.write('{}');
    return;
  }

  const client = detectClient(raw);
  const session = String(raw.session_id || raw.conversation_id || raw.cwd || 'default');

  let answer = typeof raw.text === 'string' ? raw.text : '';
  if (!answer && raw.transcript_path) answer = lastAssistantFromTranscript(String(raw.transcript_path));
  answer = String(answer || '').trim();
  if (!answer) {
    process.stdout.write('{}');
    return;
  }

  const stash = takeStash(raw.cwd || session);
  const task = typeof stash.task === 'string' ? stash.task : '';
  const title = (task || answer).trim().split(/\r?\n/)[0].slice(0, 80) || 'turn';
  const summary = answer.slice(0, MAX_SUMMARY);
  const outcome = raw.status === 'aborted' || raw.status === 'error' ? 'fail' : 'ok';
  const h = crypto.createHash('sha1').update(client + '|' + session + '|' + summary).digest('hex').slice(0, 16);
  if (dedupeHit(h)) {
    process.stdout.write('{}');
    return;
  }

  const base = process.env.APEX_BASE || process.env.USEARCH_BASE || 'http://api.ya.com';
  const token = process.env.APEX_TOKEN || process.env.USEARCH_TOKEN || 'sk-default';
  try {
    const resp = await postMcp(base, token, 'observe', {
      payload: { title, summary, outcome, source: client, session },
    });
    if (process.env.APEX_POSTSYNC_DEBUG) {
      try {
        fs.appendFileSync(path.join(os.tmpdir(), 'apex_postsync.debug'),
          JSON.stringify({ client, session, title, ok: !!(resp && resp.result), raw: resp }) + '\n');
      } catch (_) {}
    }
  } catch (_) {
    // fail-open：入队失败只记日志，不影响客户端
    try {
      fs.appendFileSync(path.join(os.tmpdir(), 'apex_postsync.log'), new Date().toISOString() + ' observe failed\n');
    } catch (_) {}
  }
  // 回合后当轮补块：presync 已注入输入侧块（模型照抄），这里补上"只有回合结束才知道"的
  // 真值：实际输入 token / 输出 token / 省下的主 LLM api 调用。默认开（APEX_POSTSYNC_BLOCK=0 关）。
  try {
    const wantBlock = process.env.APEX_POSTSYNC_BLOCK !== '0';
    const block = typeof stash.block === 'string' ? stash.block : '';
    if (wantBlock) {
      // ① 输出侧：只有回合结束才知道的真值（输入/输出 token + 输出内容）
      const lc = await getLastcall(base, token);
      const hasCall = !!(lc && (lc.inTok || lc.outTok || lc.reply));
      const outLines = [];
      if (hasCall) {
        outLines.push(
          '📝 输入 ' + (lc.inTok || 0) + ' tok · 输出 ' + (lc.outTok || 0) + ' tok' + (lc.real === true ? '' : '(est)')
        );
        outLines.push(
          '♻️ 省主 LLM api 调用 **' +
            (lc.saved || 0) +
            '** 次（缓存 L1 **' +
            (lc.savedL1 || 0) +
            '** · L2 **' +
            (lc.savedL2 || 0) +
            '** · 本地直答 **' +
            (lc.savedLocal || 0) +
            '**）'
        );
      }
      // ② 输入侧：hook 注入的整块（模型漏抄时才补）
      const hasBlock = answer.includes('⚡ 规则');
      const hasOut = answer.includes('♻️ 省主 LLM');
      const addBlock = !hasBlock && !!block;
      const addOut = !hasOut && outLines.length > 0;
      if (addBlock || addOut) {
        const body =
          (addBlock ? block : '') + (addBlock && addOut ? '\n' : '') + (addOut ? outLines.join('\n') : '');
        if (client === 'cursor') {
          process.stdout.write(JSON.stringify({ followup_message: body }));
          return;
        }
        const reason = '请在回复末尾原样附上以下内容（勿改写）：\n' + body;
        process.stdout.write(JSON.stringify({ decision: 'block', reason }));
        return;
      }
    }
  } catch (_) {}
  // 默认：绝不输出 followup_message / decision，避免自循环
  process.stdout.write('{}');
})();
