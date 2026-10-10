#!/usr/bin/env python3
from pathlib import Path
import sys

I32 = 0x7F

def uleb(value: int) -> bytes:
    out = bytearray()
    while True:
        b = value & 0x7F
        value >>= 7
        if value:
            b |= 0x80
        out.append(b)
        if not value:
            return bytes(out)

def sleb(value: int) -> bytes:
    out = bytearray()
    more = True
    while more:
        b = value & 0x7F
        value >>= 7
        sign = (b & 0x40) != 0
        more = not ((value == 0 and not sign) or (value == -1 and sign))
        if more:
            b |= 0x80
        out.append(b)
    return bytes(out)

def vec(items) -> bytes:
    return uleb(len(items)) + b"".join(items)

def name(text: str) -> bytes:
    raw = text.encode("utf-8")
    return uleb(len(raw)) + raw

def section(section_id: int, payload: bytes) -> bytes:
    return bytes([section_id]) + uleb(len(payload)) + payload

def ftype(params, results) -> bytes:
    return b"\x60" + vec([bytes([p]) for p in params]) + vec([bytes([r]) for r in results])

def local_get(index: int) -> bytes:
    return b"\x20" + uleb(index)

def global_get(index: int) -> bytes:
    return b"\x23" + uleb(index)

def global_set(index: int) -> bytes:
    return b"\x24" + uleb(index)

def i32_const(value: int) -> bytes:
    return b"\x41" + sleb(value)

def load(offset: int = 0) -> bytes:
    return b"\x28" + uleb(2) + uleb(offset)

def store(offset: int = 0) -> bytes:
    return b"\x36" + uleb(2) + uleb(offset)

def store_const(address: int, value: int) -> bytes:
    return i32_const(address) + i32_const(value) + store()

def pair(length: int) -> bytes:
    return store_const(192, 256) + store_const(196, length) + i32_const(192)

def body(code: bytes) -> bytes:
    payload = b"\x00" + code
    return uleb(len(payload)) + payload

symbol = "databind_9_CHttpWasm_4_Calc_3_Add"

types = section(1, vec([
    ftype([I32, I32, I32, I32], [I32]),
    ftype([I32, I32], [I32]),
]))
functions = section(3, vec([uleb(0), uleb(1)]))
memory = section(5, vec([b"\x00" + uleb(1)]))
globals_section = section(6, vec([
    bytes([I32, 0x01]) + i32_const(1024) + b"\x0b"
]))
exports = section(7, vec([
    name("memory") + b"\x02" + uleb(0),
    name("cabi_realloc") + b"\x00" + uleb(0),
    name(symbol) + b"\x00" + uleb(1),
]))

realloc_code = (
    global_get(0)
    + global_get(0)
    + local_get(3)
    + b"\x6a"
    + global_set(0)
    + b"\x0b"
)

native_error = store_const(256, 7) + pair(4)

success = (
    store_const(256, 0)
    + i32_const(260)
    + local_get(0) + load(0)
    + local_get(0) + load(4)
    + local_get(0) + load(8)
    + b"\x6c"
    + b"\x6a"
    + store()
    + pair(8)
)

operation_code = (
    local_get(0) + load(0)
    + i32_const(99)
    + b"\x46"
    + b"\x04" + bytes([I32])
      + native_error
    + b"\x05"
      + success
    + b"\x0b"
    + b"\x0b"
)

codes = section(10, vec([
    body(realloc_code),
    body(operation_code),
]))

module = (
    b"\x00asm\x01\x00\x00\x00"
    + types
    + functions
    + memory
    + globals_section
    + exports
    + codes
)

if len(sys.argv) != 2:
    raise SystemExit("usage: chttp_service_wasm_core.py <output.wasm>")
path = Path(sys.argv[1])
path.parent.mkdir(parents=True, exist_ok=True)
path.write_bytes(module)
