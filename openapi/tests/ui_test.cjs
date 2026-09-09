const { test } = require('node:test');
const assert = require('node:assert/strict');
const { requestFor, operations, boundedText } = require('../ui/app.js');
test('encode paths, query and headers without losing server prefix', () => {
  const op = { method: 'get', path: '/pets/{id}', parameters: [
    { in: 'path', name: 'id', required: true, schema: { type: 'string' } },
    { in: 'query', name: 'q', schema: { type: 'string' } },
    { in: 'header', name: 'X-Key', schema: { type: 'string' } }] };
  const r = requestFor(op, 'https://example.test/api', { 0: 'a/b', 1: 'x&y', 2: 'key' }, '', '');
  assert.equal(r.url, 'https://example.test/api/pets/a%2Fb?q=x%26y');
  assert.equal(r.init.headers.get('X-Key'), 'key');
  assert.equal(r.init.credentials, 'omit');
  assert.throws(() => requestFor(op, 'https://example.test', {}, '', ''), /必填/);
  assert.throws(() => requestFor(op, 'https://example.test', { 0: '..' }, '', ''), /点路径/);
});
test('unsupported input fails before fetch', () => {
  const op = { method: 'post', path: '/pets', parameters: [], requestBody: { required: true } };
  assert.throws(() => requestFor(op, 'https://example.test', {}, '', 'application/json'), /必填/);
  assert.throws(() => requestFor(op, 'https://example.test', {}, '{bad', 'application/json'));
  assert.throws(() => requestFor(op, 'file:///tmp', {}, '{}', 'application/json'), /HTTP/);
  for (const p of [{ in: 'query', schema: { type: 'array' } }, { in: 'cookie', schema: { type: 'string' } }])
    assert.throws(() => requestFor({ ...op, parameters: [p] }, 'https://example.test', {}, '{}', 'application/json'));
});
test('path parameters inherit and operation parameters override', () => {
  const list = operations({ openapi: '3.1.0', info: {}, paths: { '/pets': {
    parameters: [{ name: 'n', in: 'query', required: false }],
    get: { parameters: [{ name: 'n', in: 'query', required: true }] } } } });
  assert.equal(list[0].parameters.length, 1); assert.equal(list[0].parameters[0].required, true);
  assert.throws(() => operations({ openapi: '3.0.0' }));
});
test('bounded response refuses oversize content', async () => {
  assert.equal(await boundedText(new Response('hello'), 5), 'hello');
  await assert.rejects(boundedText(new Response('hello!'), 5), /超过/);
});
