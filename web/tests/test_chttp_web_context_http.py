import http.cookiejar
import subprocess
import sys
import urllib.error
import urllib.request

binary = sys.argv[1]
proc = subprocess.Popen(
    [binary],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
)

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None

try:
    line = proc.stdout.readline().strip()
    assert line.startswith(
        "CHttp::Web context example: http://127.0.0.1:"
    ), line
    base = line.split("CHttp::Web context example: ", 1)[1]
    if not base.endswith("/"):
        base += "/"

    jar = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(
        urllib.request.HTTPCookieProcessor(jar)
    )

    normal = urllib.request.Request(
        base + "users/42",
        headers={"X-Request-ID": "req-a", "X-Theme": "light"},
    )
    with opener.open(normal, timeout=5) as response:
        body = response.read()
        assert response.status == 200
        assert response.headers.get_content_type() == "text/html"
        assert response.headers.get_content_charset() == "utf-8"
    assert body.startswith(b"<!doctype html>")
    assert b'<p class="kind">full</p>' in body
    assert b'<p class="id">42</p>' in body
    assert b'<p class="rid">req-a</p>' in body
    assert b'<p class="theme">light</p>' in body
    assert b"&lt;Admin &amp; Users&gt;" in body
    assert b"<Admin & Users>" not in body

    fragment = urllib.request.Request(
        base + "users/7",
        headers={
            "HX-Request": "true",
            "X-Request-ID": "req-b",
            "X-Theme": "dark",
        },
    )
    with opener.open(fragment, timeout=5) as response:
        fragment_body = response.read()
        assert response.status == 200
        assert response.headers.get("HX-Trigger") == "users:rendered"
        assert response.headers.get("HX-Retarget") == "#users"
        assert response.headers.get_content_type() == "text/html"
        assert response.headers.get_content_charset() == "utf-8"
    assert not fragment_body.startswith(b"<!doctype html>")
    assert b'<p class="kind">fragment</p>' in fragment_body
    assert b'<p class="id">7</p>' in fragment_body
    assert b'<p class="rid">req-b</p>' in fragment_body
    assert b'<p class="theme">dark</p>' in fragment_body
    assert b"req-a" not in fragment_body
    assert b'<p class="id">42</p>' not in fragment_body
    assert b"&lt;Admin &amp; Users&gt;" in fragment_body

    case_sensitive = urllib.request.Request(
        base + "users/9",
        headers={
            "HX-Request": "True",
            "X-Request-ID": "req-c",
            "X-Theme": "light",
        },
    )
    with opener.open(case_sensitive, timeout=5) as response:
        case_body = response.read()
        assert response.status == 200
        assert response.headers.get("HX-Trigger") is None
        assert response.headers.get("HX-Retarget") is None
    assert case_body.startswith(b"<!doctype html>")
    assert b'<p class="kind">full</p>' in case_body
    assert b'<p class="id">9</p>' in case_body
    assert b'<p class="rid">req-c</p>' in case_body

    no_redirect = urllib.request.build_opener(
        urllib.request.HTTPCookieProcessor(jar), NoRedirect()
    )
    try:
        no_redirect.open(base + "redirect", timeout=5)
        raise AssertionError("303 redirect unexpectedly followed")
    except urllib.error.HTTPError as exc:
        assert exc.code == 303
        assert exc.headers.get("Location") == "/users/redirected"
        assert exc.read() == b""

    with opener.open(base + "hx-headers", timeout=5) as response:
        assert response.status == 204
        assert response.read() == b""
        assert response.headers.get("HX-Redirect") == "/users/42"
        assert response.headers.get("HX-Trigger") == "users:refresh"
        assert response.headers.get("HX-Retarget") == "#users"

    # Fresh client: no Session cookie, so the eight response-header slots are
    # exactly six fill headers plus HX-Redirect and HX-Trigger. HX-Retarget must
    # be rejected by the configured CHTTP bound without corrupting the response.
    with urllib.request.urlopen(base + "hx-bounded", timeout=5) as response:
        assert response.status == 204
        assert response.read() == b""
        assert response.headers.get("HX-Redirect") == "/users/42"
        assert response.headers.get("HX-Trigger") == "users:refresh"
        assert response.headers.get("HX-Retarget") is None
        for i in range(6):
            assert response.headers.get(f"X-Fill-{i}") == "1"

    try:
        opener.open(base + "error", timeout=5)
        raise AssertionError("404 error response unexpectedly succeeded")
    except urllib.error.HTTPError as exc:
        assert exc.code == 404
        error_body = exc.read()
        assert exc.headers.get_content_type() == "text/html"
        assert exc.headers.get_content_charset() == "utf-8"
    assert error_body.startswith(b"<!doctype html>")
    assert b"&lt;Not Found &amp; Gone&gt;" in error_body
    assert b"<Not Found & Gone>" not in error_body
    assert b"/error" in error_body
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
print("CHttp::Web request context HTTP verification passed")
