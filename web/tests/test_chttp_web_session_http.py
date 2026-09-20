import http.cookiejar
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

binary = sys.argv[1]
proc = subprocess.Popen(
    [binary],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
)

def expect_http_error(opener, request, code):
    try:
        opener.open(request, timeout=5)
        raise AssertionError(f"expected HTTP {code}")
    except urllib.error.HTTPError as exc:
        assert exc.code == code, (exc.code, code)
        exc.read()
        return exc

def post_form(opener, url, fields, headers=None):
    data = urllib.parse.urlencode(fields, doseq=True).encode("ascii")
    req_headers = {"Content-Type": "application/x-www-form-urlencoded"}
    if headers:
        req_headers.update(headers)
    return opener.open(
        urllib.request.Request(
            url, data=data, headers=req_headers, method="POST"
        ),
        timeout=5,
    )

try:
    line = proc.stdout.readline().strip()
    assert line.startswith(
        "CHttp::Web session example: http://127.0.0.1:"
    ), line
    base = line.split("CHttp::Web session example: ", 1)[1]
    if not base.endswith("/"):
        base += "/"

    with urllib.request.urlopen(base + "safe", timeout=5) as response:
        assert response.status == 204
        assert response.read() == b""

    jar = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(
        urllib.request.HTTPCookieProcessor(jar)
    )

    with opener.open(base + "csrf", timeout=5) as response:
        token = response.read().decode("ascii")
        assert response.status == 200
    assert re.fullmatch(r"[0-9a-f]{64}", token), token
    cookies = list(jar)
    assert len(cookies) == 1
    stale_cookie_name = cookies[0].name
    stale_cookie_value = cookies[0].value

    expect_http_error(
        opener,
        urllib.request.Request(
            base + "mutate",
            data=b"",
            headers={"Content-Type": "application/x-www-form-urlencoded"},
            method="POST",
        ),
        403,
    )
    expect_http_error(
        opener,
        urllib.request.Request(
            base + "mutate",
            data=urllib.parse.urlencode({"_csrf": "0" * 64}).encode("ascii"),
            headers={"Content-Type": "application/x-www-form-urlencoded"},
            method="POST",
        ),
        403,
    )

    with post_form(opener, base + "mutate", {"_csrf": token}) as response:
        assert response.status == 204
        assert response.read() == b""

    with post_form(
        opener,
        base + "mutate",
        {},
        headers={"HX-Request": "true", "X-CSRF-Token": token},
    ) as response:
        assert response.status == 204
        assert response.read() == b""

    expect_http_error(
        opener,
        urllib.request.Request(
            base + "mutate",
            data=b"",
            headers={
                "Content-Type": "application/x-www-form-urlencoded",
                "HX-Request": "True",
                "X-CSRF-Token": token,
            },
            method="POST",
        ),
        403,
    )

    with opener.open(base + "rotate", timeout=5) as response:
        rotated = response.read().decode("ascii")
    assert re.fullmatch(r"[0-9a-f]{64}", rotated), rotated
    assert rotated != token

    expect_http_error(
        opener,
        urllib.request.Request(
            base + "mutate",
            data=urllib.parse.urlencode({"_csrf": token}).encode("ascii"),
            headers={"Content-Type": "application/x-www-form-urlencoded"},
            method="POST",
        ),
        403,
    )
    with post_form(opener, base + "mutate", {"_csrf": rotated}) as response:
        assert response.status == 204

    with opener.open(base + "invalidate", timeout=5) as response:
        assert response.status == 204
        assert response.read() == b""

    stale_request = urllib.request.Request(
        base + "mutate",
        data=urllib.parse.urlencode({"_csrf": rotated}).encode("ascii"),
        headers={
            "Content-Type": "application/x-www-form-urlencoded",
            "Cookie": f"{stale_cookie_name}={stale_cookie_value}",
        },
        method="POST",
    )
    expect_http_error(urllib.request.build_opener(), stale_request, 403)

    with opener.open(base + "csrf", timeout=5) as response:
        current = response.read().decode("ascii")
    assert re.fullmatch(r"[0-9a-f]{64}", current), current

    with post_form(opener, base + "flash", {"_csrf": current}) as response:
        assert response.status == 204
        assert response.read() == b""

    with opener.open(base + "flash", timeout=5) as response:
        flash = response.read()
        assert response.status == 200
    assert flash == b"info:saved\nwarning:check\n", flash

    with opener.open(base + "flash", timeout=5) as response:
        empty = response.read()
        assert response.status == 200
    assert empty == b"", empty

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
print("CHttp::Web CSRF and flash HTTP verification passed")
