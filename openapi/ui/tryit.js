'use strict';

const LIMITS = { response: 1024 * 1024, request: 65536, timeout: 15000 };

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
  } finally {
    await reader.cancel();
  }
}

function requestFor(operation, base, values, body, media) {
  const server = new URL(base);
  if (!['http:', 'https:'].includes(server.protocol) ||
      server.username || server.password || server.search || server.hash)
    throw new Error('服务地址必须为无凭据、查询串和片段的 HTTP(S) URL');
  if (['trace', 'connect'].includes(operation.method))
    throw new Error('浏览器不支持此请求方法');

  let path = operation.path;
  const query = new URLSearchParams(), headers = new Headers();
  for (const [index, p] of operation.parameters.entries()) {
    if (p.$ref || !p.schema ||
        !['string', 'integer', 'number', 'boolean'].includes(p.schema.type) ||
        p.style || p.explode !== undefined || p.content)
      throw new Error('Try it 目前仅支持默认序列化的标量参数；请查看参数 schema');
    if (p.in === 'cookie')
      throw new Error('浏览器管理 Cookie；此版本不支持 Cookie 参数试调');

    const value = values[index] ?? '';
    if (value === '') {
      if (p.required) throw new Error(`${p.name} 为必填参数`);
      continue;
    }

    if (p.in === 'path') {
      if (value === '.' || value === '..')
        throw new Error('路径参数不能为点路径段');
      path = path.split(`{${p.name}}`).join(encodeURIComponent(value));
    } else if (p.in === 'query') {
      query.append(p.name, value);
    } else if (p.in === 'header') {
      if (/^(accept-charset|accept-encoding|access-control-request-.*|connection|content-length|cookie|date|dnt|expect|host|keep-alive|origin|permissions-policy|referer|te|trailer|transfer-encoding|upgrade|via|proxy-.*|sec-.*)$/i.test(p.name))
        throw new Error(`${p.name} 由浏览器控制，无法手动设置`);
      headers.set(p.name, value);
    } else {
      throw new Error('不支持的参数位置');
    }
  }

  if (!path.startsWith('/') || /[{}?#\\\x00-\x20\x7f]/.test(path) ||
      /(^|\/)\.{1,2}(\/|$)/.test(path))
    throw new Error('路径无效或缺少路径参数');

  const url = new URL(server.href.replace(/\/$/, '') + path);
  if (url.origin !== server.origin)
    throw new Error('路径不能改变服务来源');
  url.search = query.toString();

  const init = {
    method: operation.method.toUpperCase(),
    headers,
    credentials: 'omit',
    redirect: 'error'
  };

  if (operation.requestBody?.$ref)
    throw new Error('暂不支持引用请求体试调');
  if (operation.requestBody?.required && !body)
    throw new Error('请求体为必填');

  if (body) {
    if (['get', 'head'].includes(operation.method))
      throw new Error('浏览器不允许 GET/HEAD 请求体');
    if (!media || media.includes('*'))
      throw new Error('请选择具体请求媒体类型');
    if (new TextEncoder().encode(body).length > LIMITS.request)
      throw new Error('请求体超过 64 KiB');
    if (/^application\/(json|[^;]+\+json)(;|$)/i.test(media)) JSON.parse(body);
    headers.set('Content-Type', media);
    init.body = body;
  }
  return { url: url.href, init };
}

function resolveServerUrl(serverUrl, pageUrl) {
  const page = new URL(pageUrl);
  return serverUrl ? new URL(serverUrl, page.href).href : page.origin;
}

function parseJsonAttribute(value, fallback) {
  if (!value) return fallback;
  return JSON.parse(value);
}

function initializeForm(form) {
  if (!form || form.dataset.tryitReady === '1') return;
  form.dataset.tryitReady = '1';
  const base = form.querySelector('[data-tryit-base]');
  if (base && typeof location !== 'undefined') {
    try {
      base.value = resolveServerUrl(base.value, location.href);
    } catch (_) {
      /* Keep the original value; requestFor will reject it before fetch. */
    }
  }

  const requestBody = parseJsonAttribute(form.dataset.tryitRequestBody, null);
  const media = form.querySelector('[data-tryit-media]');
  if (media && requestBody && requestBody.content) {
    for (const type of Object.keys(requestBody.content)) {
      const option = document.createElement('option');
      option.value = type;
      option.textContent = type;
      media.appendChild(option);
    }
  }
}

function operationFromForm(form) {
  const parameters = [...form.querySelectorAll('[data-tryit-parameter]')].map(input => ({
    in: input.dataset.in,
    name: input.dataset.name,
    required: input.dataset.required === 'true',
    schema: parseJsonAttribute(input.dataset.schema, {})
  }));
  return {
    method: form.dataset.method,
    path: form.dataset.path,
    parameters,
    requestBody: parseJsonAttribute(form.dataset.tryitRequestBody, undefined)
  };
}

async function executeForm(form) {
  const errorBox = form.querySelector('[data-tryit-error]');
  const result = form.querySelector('[data-tryit-result]');
  const button = form.querySelector('[data-tryit-submit]');
  if (errorBox) { errorBox.hidden = true; errorBox.textContent = ''; }
  if (result) { result.hidden = true; result.textContent = ''; }

  try {
    const operation = operationFromForm(form);
    const values = {};
    [...form.querySelectorAll('[data-tryit-parameter]')]
      .forEach((input, index) => { values[index] = input.value; });
    const baseInput = form.querySelector('[data-tryit-base]');
    const bodyInput = form.querySelector('[data-tryit-body]');
    const mediaInput = form.querySelector('[data-tryit-media]');
    const request = requestFor(
      operation,
      baseInput ? baseInput.value : form.dataset.serverUrl,
      values,
      bodyInput ? bodyInput.value : '',
      mediaInput ? mediaInput.value : '');

    if (button) button.disabled = true;
    const started = performance.now();
    const response = await fetch(request.url, {
      ...request.init,
      signal: AbortSignal.timeout(LIMITS.timeout)
    });
    const text = await boundedText(response, LIMITS.response);
    if (result) {
      const headers = [...response.headers].map(([k, v]) => `${k}: ${v}`).join('\n');
      result.textContent =
        `HTTP ${response.status} · ${Math.round(performance.now() - started)} ms\n` +
        `${request.url}\n\n${headers}\n\n${text || '（空响应体）'}`;
      result.hidden = false;
    }
  } catch (error) {
    if (errorBox) {
      errorBox.textContent = error instanceof Error ? error.message : String(error);
      errorBox.hidden = false;
    }
  } finally {
    if (button) button.disabled = false;
  }
}

function setOperationSearchError(root, event, hidden) {
  if (!root || event?.target?.id !== 'operation-search') return;
  const box = root.getElementById?.('operation-search-error');
  if (box) box.hidden = hidden;
}

if (typeof document !== 'undefined') {
  const initialize = root => {
    if (root?.matches?.('[data-tryit]')) initializeForm(root);
    root?.querySelectorAll?.('[data-tryit]').forEach(initializeForm);
  };
  document.addEventListener('DOMContentLoaded', () => initialize(document));
  document.addEventListener('htmx:afterSwap', event => initialize(event.target));
  document.addEventListener('htmx:before:request',
    event => setOperationSearchError(document, event, true));
  document.addEventListener('htmx:response:error',
    event => setOperationSearchError(document, event, false));
  document.addEventListener('submit', event => {
    const form = event.target.closest?.('[data-tryit]');
    if (!form) return;
    event.preventDefault();
    void executeForm(form);
  });
}

if (typeof module !== 'undefined') {
  module.exports = { LIMITS, requestFor, boundedText, resolveServerUrl, operationFromForm, setOperationSearchError };
}
