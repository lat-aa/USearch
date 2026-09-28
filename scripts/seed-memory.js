#!/usr/bin/env node
/**
 * apex 记忆播种：把知识目录里的 markdown 切片灌入 memory（MCP upsert，kind=memory）。
 *
 * 为什么需要：检索（recall / presync 的 memory 部分）只认 kind=memory；
 * 手工 upsert 曾因缺 meta.kind 被过滤（已修）——本脚本是"文档进索引"的官方入口。
 *
 * 用法：
 *   node scripts/seed-memory.js                     # 默认 .config/knowledge
 *   node scripts/seed-memory.js <dir> [<dir>...]
 *   APEX_BASE=http://api.ya.com APEX_TOKEN=sk-default node scripts/seed-memory.js
 */
'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const http = require('http');
const https = require('https');
const { URL } = require('url');

const BASE = process.env.APEX_BASE || process.env.USEARCH_BASE || 'http://api.ya.com';
const TOKEN = process.env.APEX_TOKEN || process.env.USEARCH_TOKEN || 'sk-default';
const MAX_CHARS = Number(process.env.APEX_SEED_CHUNK || 900);

function walk(dir, out) {
  // 文件路径也支持（直接传单个 .md）
  try {
    const st = fs.statSync(dir);
    if (st.isFile()) { if (/\.(md|mdx|txt)$/i.test(dir)) out.push(dir); return out; }
  } catch (_) { return out; }
  let ents = [];
  try { ents = fs.readdirSync(dir, { withFileTypes: true }); } catch (_) { return out; }
  for (const e of ents) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p, out);
    else if (/\.(md|mdx|txt)$/i.test(e.name)) out.push(p);
  }
  return out;
}

/** 按二级标题切片，超长再按段落兜底切。 */
function chunk(text) {
  const lines = text.split(/\r?\n/);
  const out = [];
  let cur = [];
  const flush = () => {
    const s = cur.join('\n').trim();
    if (s) out.push(s);
    cur = [];
  };
  for (const ln of lines) {
    if (/^##\s+/.test(ln) && cur.length) flush();
    cur.push(ln);
  }
  flush();
  const res = [];
  for (let s of out) {
    while (s.length > MAX_CHARS) {
      let cut = s.lastIndexOf('\n', MAX_CHARS);
      if (cut <= 0) cut = MAX_CHARS;
      res.push(s.slice(0, cut).trim());
      s = s.slice(cut).trim();
    }
    if (s) res.push(s);
  }
  return res;
}

function post(payload) {
  return new Promise((resolve, reject) => {
    const url = new URL('/mcp', BASE.endsWith('/') ? BASE : BASE + '/');
    const body = JSON.stringify(payload);
    const lib = url.protocol === 'https:' ? https : http;
    const req = lib.request({
      hostname: url.hostname, port: url.port || (url.protocol === 'https:' ? 443 : 80), path: url.pathname,
      method: 'POST',
      headers: { 'Content-Type': 'application/json', Accept: 'application/json, text/event-stream',
                 Authorization: 'Bearer ' + TOKEN, 'Content-Length': Buffer.byteLength(body) },
    }, (res) => {
      let d = '';
      res.setEncoding('utf8');
      res.on('data', (c) => (d += c));
      res.on('end', () => {
        if (d.startsWith('event:')) for (const ln of d.split(/\r?\n/)) if (ln.startsWith('data:')) d = ln.slice(5).trim();
        try { resolve(JSON.parse(d)); } catch (e) { reject(e); }
      });
    });
    req.on('error', reject);
    req.setTimeout(20000, () => req.destroy(new Error('timeout')));
    req.write(body); req.end();
  });
}

(async () => {
  const dirs = process.argv.slice(2).length ? process.argv.slice(2)
    : [path.resolve(__dirname, '..', '.config', 'knowledge')];
  const files = [];
  for (const d of dirs) walk(path.resolve(d), files);
  if (!files.length) {
    console.log('no knowledge files in: ' + dirs.join(', '));
    process.exit(0);
  }
  let ok = 0, fail = 0;
  for (const f of files) {
    const text = fs.readFileSync(f, 'utf8');
    const rel = path.relative(process.cwd(), f).replace(/\\/g, '/');
    const parts = chunk(text);
    for (let i = 0; i < parts.length; i++) {
      const id = 'know-' + crypto.createHash('sha1').update(rel + '#' + i).digest('hex').slice(0, 16);
      const title = (parts[i].split(/\r?\n/)[0] || rel).replace(/^#+\s*/, '').slice(0, 60);
      try {
        const r = await post({ jsonrpc: '2.0', id: 1, method: 'tools/call',
          params: { name: 'upsert', arguments: { id, text: parts[i], kind: 'memory',
                                                 meta: { source: rel, title } } } });
        const isErr = !!(r && r.result && r.result.isError);
        if (isErr) { fail++; console.log('FAIL ' + id + ' ' + rel); }
        else { ok++; }
      } catch (e) { fail++; console.log('FAIL ' + id + ' ' + e.message); }
    }
    console.log('  ' + rel + ' → ' + parts.length + ' chunks');
  }
  console.log('\nseeded ok=' + ok + ' fail=' + fail);
  process.exit(fail === 0 ? 0 : 1);
})().catch((e) => { console.error(e.message); process.exit(1); });
