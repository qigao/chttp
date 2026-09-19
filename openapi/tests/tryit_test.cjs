const { test } = require('node:test');
const assert = require('node:assert/strict');
const { requestFor, boundedText, resolveServerUrl, LIMITS } = require('../ui/tryit.js');

test('resolve relative OpenAPI server URLs against the docs page', () => {
  assert.equal(resolveServerUrl('/v1', 'https://docs.example.test/docs'),
               'https://docs.example.test/v1');
  assert.equal(resolveServerUrl('v1', 'https://docs.example.test/docs'),
               'https://docs.example.test/v1');
  assert.equal(resolveServerUrl('', 'https://docs.example.test/docs'),
               'https://docs.example.test');
});

test('encode paths, query and headers without losing server prefix', () => {
  const op = { method: 'get', path: '/pets/{id}', parameters: [
    { in: 'path', name: 'id', required: true, schema: { type: 'string' } },
    { in: 'query', name: 'q', schema: { type: 'string' } },
    { in: 'header', name: 'X-Key', schema: { type: 'string' } }] };
  const r = requestFor(op, 'https://example.test/api',
    { 0: 'a/b', 1: 'x&y', 2: 'key' }, '', '');
  assert.equal(r.url, 'https://example.test/api/pets/a%2Fb?q=x%26y');
  assert.equal(r.init.headers.get('X-Key'), 'key');
  assert.equal(r.init.credentials, 'omit');
  assert.equal(r.init.redirect, 'error');
  assert.throws(() => requestFor(op, 'https://example.test', {}, '', ''), /必填/);
  assert.throws(() => requestFor(op, 'https://example.test', { 0: '..' }, '', ''), /点路径/);
});

test('unsupported or unsafe input fails before fetch', () => {
  const post = { method: 'post', path: '/pets', parameters: [],
                 requestBody: { required: true } };
  assert.throws(() => requestFor(post, 'https://example.test', {}, '',
                                 'application/json'), /必填/);
  assert.throws(() => requestFor(post, 'https://example.test', {}, '{bad',
                                 'application/json'));
  for (const base of [
    'file:///tmp',
    'https://u:p@example.test/',
    'https://example.test/?q=x',
    'https://example.test/#frag'
  ]) assert.throws(() => requestFor(post, base, {}, '{}', 'application/json'));

  assert.throws(() => requestFor(
    { method: 'trace', path: '/pets', parameters: [] },
    'https://example.test', {}, '', ''), /方法/);

  for (const p of [
    { in: 'query', name: 'a', schema: { type: 'array' } },
    { in: 'cookie', name: 'c', schema: { type: 'string' } },
    { in: 'header', name: 'Cookie', schema: { type: 'string' } },
    { in: 'header', name: 'Sec-Fetch-Site', schema: { type: 'string' } }
  ]) {
    assert.throws(() => requestFor(
      { ...post, requestBody: undefined, parameters: [p] },
      'https://example.test', { 0: 'x' }, '', ''));
  }
});

test('request bodies stay bounded and JSON is validated', () => {
  const op = { method: 'post', path: '/pets', parameters: [] };
  assert.throws(() => requestFor(op, 'https://example.test', {},
    'x'.repeat(LIMITS.request + 1), 'text/plain'), /64 KiB/);
  assert.throws(() => requestFor(op, 'https://example.test', {},
    '{bad', 'application/json'));
  assert.throws(() => requestFor(
    { method: 'get', path: '/pets', parameters: [] },
    'https://example.test', {}, '{}', 'application/json'), /GET\/HEAD/);
});

test('bounded response refuses oversize content', async () => {
  assert.equal(await boundedText(new Response('hello'), 5), 'hello');
  await assert.rejects(boundedText(new Response('hello!'), 5), /超过/);
});
