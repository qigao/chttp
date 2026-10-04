#!/usr/bin/env python3
from pathlib import Path

source = Path("http_server/src/chttp_server.c").read_text(encoding="utf-8")

expected = {
    "cnet_listener_accept_detached(": 1,
    "cnet_client_adopt_accepted(": 1,
    "cnet_client_adopt_accepted_tls(": 1,
}
for marker, count in expected.items():
    actual = source.count(marker)
    if actual != count:
        raise SystemExit(f"{marker}: expected {count}, got {actual}")

listener_begin = source.index("static int chttp_server_listener_progress(")
listener_end = source.index(
    "static void chttp_server_connection_activate(", listener_begin
)
listener = source[listener_begin:listener_end]
if listener.count("cnet_listener_accept_detached(") != 1:
    raise SystemExit("detached accept escaped listener admission boundary")

adopt_begin = source.index("static int chttp_server_admission_progress(")
adopt_end = source.index("static int chttp_server_deadlines_progress(", adopt_begin)
adopt = source[adopt_begin:adopt_end]
if adopt.count("cnet_client_adopt_accepted(") != 1:
    raise SystemExit("plain accepted-stream adopt escaped owner admission boundary")
if adopt.count("cnet_client_adopt_accepted_tls(") != 1:
    raise SystemExit("TLS accepted-stream adopt escaped owner admission boundary")

outside = source[:listener_begin] + source[listener_end:adopt_begin] + source[adopt_end:]
for marker in expected:
    if marker in outside:
        raise SystemExit(f"{marker}: unexpected use outside admission control plane")

print(
    "owner handoff boundary: detached accept/adopt is admission-only; "
    "steady-state request/send/receive has zero accepted-stream handoff sites"
)
