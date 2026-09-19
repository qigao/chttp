"""Verify browser-only Try-it is exposed by server-rendered operation details."""
import re
import subprocess
import sys
import urllib.error
import urllib.request

server, assets, document = sys.argv[1:]
process = subprocess.Popen(
    [server, document, assets],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
)
try:
    line = process.stdout.readline()
    match = re.search(r"http://127\.0\.0\.1:\d+", line)
    assert match, f"Server did not start: {line}"
    origin = match.group()

    with urllib.request.urlopen(origin + "/docs", timeout=5) as response:
        docs = response.read()
        assert response.status == 200
        assert b'/docs/tryit.js' in docs

    with urllib.request.urlopen(
        origin + "/docs/operations/list_pets", timeout=5
    ) as response:
        detail = response.read()
        assert response.status == 200
        assert b'data-tryit-parameter' in detail
        assert b'data-in="query"' in detail
        assert b'data-name="limit"' in detail

    with urllib.request.urlopen(
        origin + "/docs/operations/createPet", timeout=5
    ) as response:
        detail = response.read()
        assert response.status == 200
        assert response.headers["X-Content-Type-Options"] == "nosniff"
        assert b"data-tryit" in detail
        assert b'data-method="post"' in detail
        assert b'data-path="/pets"' in detail
        assert b'data-server-url="https://api.example.test/v1"' in detail
        assert b"data-tryit-parameter" not in detail  # POST fixture has no params.
        assert b"data-tryit-request-body" in detail
        assert b"application/json" in detail

    with urllib.request.urlopen(origin + "/docs/tryit.js", timeout=5) as response:
        assert response.status == 200
        assert response.headers.get_content_type() == "text/javascript"
        assert response.headers["X-Content-Type-Options"] == "nosniff"
        script = response.read()
        assert b"requestFor" in script
        assert len(script) < 65536

    try:
        urllib.request.urlopen(origin + "/docs/tryit/proxy", timeout=5)
        raise AssertionError("Unexpected server-side Try-it proxy route")
    except urllib.error.HTTPError as error:
        assert error.code == 404
finally:
    try:
        process.communicate("\n", timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate()
        raise
    assert process.returncode == 0

print("OpenAPI browser Try-it HTTP contract passed")
