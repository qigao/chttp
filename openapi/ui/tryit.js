'use strict';

const LIMITS = { response: 1024 * 1024, request: 65536, timeout: 15000 };

function notImplemented() {
  throw new Error('Try-it extraction is not implemented');
}

async function boundedText() {
  return notImplemented();
}

function requestFor() {
  return notImplemented();
}

if (typeof module !== 'undefined') {
  module.exports = { LIMITS, requestFor, boundedText };
}
