import subprocess
import sys
import urllib.request

binary = sys.argv[1]
proc = subprocess.Popen(
    [binary],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
)

try:
    line = proc.stdout.readline().strip()
    assert line.startswith("CHttp::Web example: http://127.0.0.1:"), line
    url = line.split("CHttp::Web example: ", 1)[1]

    with urllib.request.urlopen(url, timeout=5) as response:
        body = response.read()
        assert response.status == 200
        assert response.headers.get_content_type() == "text/html"
        charset = response.headers.get_content_charset()
        assert charset == "utf-8", charset

    assert b"&lt;script&gt;alert(1)&lt;/script&gt; CHttp::Web" in body
    assert b"<script>alert(1)</script>" not in body
    assert body.startswith(b"<!doctype html>")
finally:
    if proc.stdin:
        proc.stdin.write("\n")
        proc.stdin.flush()
    try:
        rc = proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        rc = proc.wait(timeout=5)
    stderr = proc.stderr.read() if proc.stderr else ""

assert rc == 0, stderr
print("CHttp::Web HTTP render passed")
