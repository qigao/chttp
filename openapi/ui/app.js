'use strict';
const LIMITS = { document: 2 * 1024 * 1024, response: 1024 * 1024, request: 65536, operations: 4096, timeout: 15000 };
const METHODS = ['get', 'post', 'put', 'patch', 'delete', 'head', 'options', 'trace'];

async function boundedText(response, limit) {
  if (!response.body) return '';
  const reader = response.body.getReader(), decoder = new TextDecoder();
  let size = 0, text = '';
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      size += value.byteLength;
      if (size > limit) throw new Error(`内容超过 ${limit} 字节限制`);
      text += decoder.decode(value, { stream: true });
    }
    return text + decoder.decode();
  } finally { await reader.cancel(); }
}

function operations(document) {
  if (!/^3\.1\.\d+$/.test(document.openapi) || !document.info || !document.paths)
    throw new Error('需要包含 info、paths 的 OpenAPI 3.1 JSON 文档');
  const result = [];
  for (const [path, item] of Object.entries(document.paths)) {
    for (const method of METHODS) {
      if (!item[method]) continue;
      if (result.length === LIMITS.operations) throw new Error('接口数量超过限制');
      const operation = item[method];
      const parameters = new Map((item.parameters || []).map(p => [`${p.in}:${p.name}`, p]));
      for (const p of operation.parameters || []) parameters.set(`${p.in}:${p.name}`, p);
      result.push({ ...operation, path, method, key: `${method} ${path}`, parameters: [...parameters.values()] });
    }
  }
  return result;
}

function requestFor(operation, base, values, body, media) {
  const server = new URL(base);
  if (!['http:', 'https:'].includes(server.protocol) || server.username || server.password || server.search || server.hash)
    throw new Error('服务地址必须为无凭据、查询串和片段的 HTTP(S) URL');
  if (['trace', 'connect'].includes(operation.method)) throw new Error('浏览器不支持此请求方法');
  let path = operation.path;
  const query = new URLSearchParams(), headers = new Headers();
  for (const [index, p] of operation.parameters.entries()) {
    if (p.$ref || !p.schema || !['string', 'integer', 'number', 'boolean'].includes(p.schema.type) || p.style || p.explode !== undefined || p.content)
      throw new Error('Try it 目前仅支持默认序列化的标量参数；请查看参数 schema');
    if (p.in === 'cookie') throw new Error('浏览器管理 Cookie；此版本不支持 Cookie 参数试调');
    const value = values[index] ?? '';
    if (value === '') { if (p.required) throw new Error(`${p.name} 为必填参数`); continue; }
    if (p.in === 'path') {
      if (value === '.' || value === '..') throw new Error('路径参数不能为点路径段');
      path = path.split(`{${p.name}}`).join(encodeURIComponent(value));
    } else if (p.in === 'query') query.append(p.name, value);
    else if (p.in === 'header') {
      if (/^(accept-charset|accept-encoding|access-control-request-.*|connection|content-length|cookie|date|dnt|expect|host|keep-alive|origin|permissions-policy|referer|te|trailer|transfer-encoding|upgrade|via|proxy-.*|sec-.*)$/i.test(p.name))
        throw new Error(`${p.name} 由浏览器控制，无法手动设置`);
      headers.set(p.name, value);
    } else throw new Error('不支持的参数位置');
  }
  if (!path.startsWith('/') || /[{}?#\\\x00-\x20\x7f]/.test(path) || /(^|\/)\.{1,2}(\/|$)/.test(path)) throw new Error('路径无效或缺少路径参数');
  const url = new URL(server.href.replace(/\/$/, '') + path);
  if (url.origin !== server.origin) throw new Error('路径不能改变服务来源');
  url.search = query.toString();
  const init = { method: operation.method.toUpperCase(), headers, credentials: 'omit', redirect: 'error' };
  if (operation.requestBody?.$ref) throw new Error('暂不支持引用请求体试调');
  if (operation.requestBody?.required && !body) throw new Error('请求体为必填');
  if (body) {
    if (['get', 'head'].includes(operation.method)) throw new Error('浏览器不允许 GET/HEAD 请求体');
    if (!media || media.includes('*')) throw new Error('请选择具体请求媒体类型');
    if (new TextEncoder().encode(body).length > LIMITS.request) throw new Error('请求体超过 64 KiB');
    if (/^application\/(json|[^;]+\+json)(;|$)/i.test(media)) JSON.parse(body);
    headers.set('Content-Type', media); init.body = body;
  }
  return { url: url.href, init };
}

function apiExplorer() {
  return {
    document: null, list: [], search: '', active: null, values: {}, body: '', media: '',
    base: location.origin, error: '', busy: false, loading: true, result: null,
    pretty(value) { return JSON.stringify(value, null, 2); },
    get filtered() { const q = this.search.toLowerCase(); return this.list.filter(o => `${o.method} ${o.path} ${o.summary || ''} ${(o.tags || []).join(' ')}`.toLowerCase().includes(q)); },
    async init() {
      try {
        const response = await fetch('/openapi.json', { signal: AbortSignal.timeout(LIMITS.timeout), credentials: 'omit' });
        if (!response.ok) throw new Error(`文档加载失败：HTTP ${response.status}`);
        this.document = JSON.parse(await boundedText(response, LIMITS.document));
        this.list = operations(this.document);
        if (this.document.servers?.[0]?.url) this.base = new URL(this.document.servers[0].url, location.href).href;
        if (this.list.length) this.select(this.list[0]);
      } catch (error) { this.error = error.message; }
      finally { this.loading = false; }
    },
    select(operation) {
      if (this.busy) return;
      this.active = operation; this.values = {}; this.body = ''; this.result = null; this.error = '';
      this.media = Object.keys(operation.requestBody?.content || {})[0] || '';
    },
    async execute() {
      if (this.busy) return;
      this.error = ''; this.result = null;
      try {
        const request = requestFor(this.active, this.base, this.values, this.body, this.media);
        this.busy = true;
        const start = performance.now();
        const response = await fetch(request.url, { ...request.init, signal: AbortSignal.timeout(LIMITS.timeout) });
        const text = await boundedText(response, LIMITS.response);
        this.result = { status: response.status, time: Math.round(performance.now() - start), url: request.url,
          headers: [...response.headers].map(([k, v]) => `${k}: ${v}`).join('\n'), text };
      } catch (error) { this.error = error.message; }
      finally { this.busy = false; }
    }
  };
}
if (typeof document !== 'undefined') document.addEventListener('alpine:init', () => Alpine.data('apiExplorer', apiExplorer));
if (typeof module !== 'undefined') module.exports = { operations, requestFor, boundedText };
