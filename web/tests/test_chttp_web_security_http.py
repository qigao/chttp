import re
import subprocess
import sys
import urllib.error
import urllib.request

binary = sys.argv[1]
layout_path = sys.argv[2]

EXPECTED_CSP = (
    "default-src 'self'; "
    "base-uri 'self'; "
    "object-src 'none'; "
    "frame-ancestors 'none'; "
    "form-action 'self'; "
    "script-src 'self'; "
    "style-src 'self'; "
    "img-src 'self' data:; "
    "connect-src 'self'"
)

proc = subprocess.Popen(
    [binary],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
)

def open_request(request):
    return urllib.request.urlopen(request, timeout=5)

def assert_security_headers(response, referrer="no-referrer"):
    assert response.headers["Content-Security-Policy"] == EXPECTED_CSP
    assert response.headers["X-Content-Type-Options"] == "nosniff"
    assert response.headers["Referrer-Policy"] == referrer
    assert response.headers["X-Frame-Options"] == "DENY"
    assert (
        response.headers["Strict-Transport-Security"]
        == "max-age=31536000; includeSubDomains"
    )
    assert response.headers["Cache-Control"] == "private, no-store"

try:
    line = proc.stdout.readline().strip()
    assert line.startswith(
        "CHttp::Web security example: http://127.0.0.1:"
    ), line
    base = line.split("CHttp::Web security example: ", 1)[1]
    if not base.endswith("/"):
        base += "/"

    with open_request(base + "token") as response:
        token = response.read().decode("ascii")
        assert response.status == 200
        assert_security_headers(response)
    assert token.count(".") == 2

    unauthorized = urllib.request.Request(base + "secure", method="GET")
    try:
        open_request(unauthorized)
        raise AssertionError("expected JWT-protected route to reject")
    except urllib.error.HTTPError as exc:
        assert exc.code in (401, 403), exc.code
        exc.read()

    authorized = urllib.request.Request(
        base + "secure",
        headers={"Authorization": "Bearer " + token},
        method="GET",
    )
    with open_request(authorized) as response:
        assert response.status == 200
        assert response.read() == b"ok"
        assert_security_headers(response)

    with open_request(base + "override") as response:
        assert response.status == 200
        assert response.read() == b"override"
        assert_security_headers(response, referrer="strict-origin")

    preflight = urllib.request.Request(
        base + "secure",
        headers={
            "Origin": "https://app.example",
            "Access-Control-Request-Method": "GET",
            "Access-Control-Request-Headers": "Authorization",
        },
        method="OPTIONS",
    )
    with open_request(preflight) as response:
        assert response.status == 204
        assert response.read() == b""
        assert response.headers["Access-Control-Allow-Origin"] == "https://app.example"
        assert response.headers["Access-Control-Allow-Methods"] == "GET, OPTIONS"
        assert response.headers["Access-Control-Allow-Headers"] == "Authorization"
        assert_security_headers(response)

    layout = open(layout_path, encoding="utf-8").read()
    lowered = layout.lower()
    assert "<style" not in lowered
    for match in re.finditer(r"<script\b([^>]*)>", layout, re.IGNORECASE):
        attrs = match.group(1)
        src = re.search(r'\bsrc\s*=\s*"([^"]+)"', attrs, re.IGNORECASE)
        assert src, match.group(0)
        assert src.group(1).startswith("/"), src.group(1)
    stylesheets = re.findall(
        r'<link\b[^>]*rel\s*=\s*"stylesheet"[^>]*href\s*=\s*"([^"]+)"',
        layout,
        re.IGNORECASE,
    )
    assert stylesheets
    assert all(item.startswith("/") for item in stylesheets)
    assert "'unsafe-inline'" not in EXPECTED_CSP
    assert "script-src 'self'" in EXPECTED_CSP
    assert "style-src 'self'" in EXPECTED_CSP

finally:
    if proc.stdin:
        try:
            proc.stdin.write("\n")
            proc.stdin.flush()
        except BrokenPipeError:
            pass
    try:
        rc = proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        rc = proc.wait(timeout=5)
    stderr = proc.stderr.read() if proc.stderr else ""

assert rc == 0, stderr
print("CHttp::Web security middleware verification passed")
