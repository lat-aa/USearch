#!/usr/bin/env node
/**
 * 三端 Hook(presync)：主 LLM 前调用 MCP/HTTP gate。
 * Cursor beforeSubmitPrompt → continue/user_message
 * Codex UserPromptSubmit → decision/additionalContext
 * Claude UserPromptSubmit → continue/stopReason 或 additionalContext
 */
'use strict';

const http = require('http');
const https = require('https');
const { URL } = require('url');

function readStdin() {
  return new Promise((resolve, reject) => {
    const chunks = [];
    process.stdin.setEncoding('utf8');
    process.stdin.on('data', (c) => chunks.push(c));
    process.stdin.on('end', () => resolve(chunks.join('')));
    process.stdin.on('error', reject);
  });
}

function detectClient(raw) {
  if (raw.hook_event_name === 'UserPromptSubmit' || raw.hookEventName === 'UserPromptSubmit') {
    if (raw.session_id || raw.cwd) return 'claude';
    return 'codex';
  }
  if (raw.prompt !== undefined && (raw.continue !== undefined || raw.attachments)) return 'cursor';
  if (raw.prompt !== undefined) return 'cursor';
  return 'codex';
}

function extractPrompt(raw) {
  return String(raw.prompt || raw.user_prompt || raw.text || '').trim();
}

function postGate(base, token, task) {
  const url = new URL('/v1/gate', base.endsWith('/') ? base : base + '/');
  const body = JSON.stringify({ task });
  const lib = url.protocol === 'https:' ? https : http;
  const headers = {
    'Content-Type': 'application/json',
    Accept: 'application/json',
    'Content-Length': Buffer.byteLength(body),
  };
  if (token) headers.Authorization = 'Bearer ' + token;
  return new Promise((resolve, reject) => {
    const req = lib.request(
      {
        hostname: url.hostname,
        port: url.port || (url.protocol === 'https:' ? 443 : 80),
        path: url.pathname,
        method: 'POST',
        headers,
        timeout: 25000,
      },
      (res) => {
        const chunks = [];
        res.on('data', (d) => chunks.push(d));
        res.on('end', () => {
          const text = Buffer.concat(chunks).toString('utf8');
          try {
            resolve(JSON.parse(text));
          } catch (e) {
            reject(new Error('gate non-json: ' + text.slice(0, 200)));
          }
        });
      }
    );
    req.on('error', reject);
    req.on('timeout', () => {
      req.destroy();
      reject(new Error('gate timeout'));
    });
    req.write(body);
    req.end();
  });
}

function emit(obj) {
  process.stdout.write(JSON.stringify(obj));
}

async function main() {
  let raw = {};
  try {
    const stdin = await readStdin();
    if (stdin.trim()) raw = JSON.parse(stdin);
  } catch (_) {
    raw = {};
  }
  const prompt = extractPrompt(raw);
  if (!prompt) {
    // 无 prompt：放行
    const client = detectClient(raw);
    if (client === 'cursor') emit({ continue: true });
    else emit({});
    return;
  }
  const base = process.env.APEX_BASE || process.env.USEARCH_BASE || 'http://api.ya.com';
  const token = process.env.APEX_TOKEN || process.env.USEARCH_TOKEN || 'sk-default';
  let gated;
  try {
    gated = await postGate(base, token, prompt);
  } catch (e) {
    // 失败开放：不阻断主 LLM
    const client = detectClient(raw);
    if (client === 'cursor') emit({ continue: true, user_message: String(e.message || e) });
    else
      emit({
        hookSpecificOutput: {
          hookEventName: 'UserPromptSubmit',
          additionalContext: 'gate unavailable: ' + String(e.message || e),
        },
      });
    return;
  }

  const status = gated.status || 'pack';
  const reply = gated.reply || '';
  const pack = gated.pack || {};
  const client = detectClient(raw);

  if (status === 'answered' && reply) {
    if (client === 'cursor') {
      emit({ continue: false, user_message: reply });
      return;
    }
    if (client === 'claude') {
      emit({ continue: false, stopReason: reply });
      return;
    }
    // Codex：block + reason 展示给用户
    emit({ decision: 'block', reason: reply });
    return;
  }

  if (status === 'refuse') {
    const why = JSON.stringify(gated.conflicts || gated.reason || 'policy conflict');
    if (client === 'cursor') {
      emit({ continue: false, user_message: 'gate refuse: ' + why });
      return;
    }
    if (client === 'claude') {
      emit({ continue: false, stopReason: 'gate refuse: ' + why });
      return;
    }
    emit({ decision: 'block', reason: 'gate refuse: ' + why });
    return;
  }

  // pack：注入上下文后放行主 LLM
  const ctx = 'gate pack JSON follows:\n' + JSON.stringify(pack).slice(0, 9000);
  if (client === 'cursor') {
    // Cursor beforeSubmitPrompt 不能注入 additional_context；放行并由 apex 首调 gate
    emit({ continue: true });
    return;
  }
  emit({
    hookSpecificOutput: {
      hookEventName: 'UserPromptSubmit',
      additionalContext: ctx,
    },
  });
}

main().catch((e) => {
  process.stderr.write(String(e.stack || e) + '\n');
  process.stdout.write(JSON.stringify({ continue: true }));
});
