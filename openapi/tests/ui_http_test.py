"""Exercise real http_server routes and orderly shutdown, without external services."""
import json
import re
import subprocess
import sys
import urllib.error
import urllib.request

server, assets, document = sys.argv[1:]
process = subprocess.Popen([server, document, assets], stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
try:
    line = process.stdout.readline()
    match = re.search(r"http://127\.0\.0\.1:\d+", line)
    assert match, f"Server did not start: {line}"
    origin = match.group()
    for path, mime in (("/docs", "text/html"), ("/docs/", "text/html"),
                       ("/docs/app.js", "text/javascript"), ("/docs/style.css", "text/css"),
                       ("/docs/alpine.js", "text/javascript"), ("/openapi.json", "application/json")):
        with urllib.request.urlopen(origin + path, timeout=5) as response:
            assert response.status == 200
            assert response.headers.get_content_type() == mime
            assert response.headers["X-Content-Type-Options"] == "nosniff"
            content = response.read()
            assert content
            if path == "/openapi.json":
                assert json.loads(content)["openapi"] == "3.1.0"
    try:
        urllib.request.urlopen(origin + "/docs/../README.md", timeout=5)
        raise AssertionError("Unmapped asset was exposed")
    except urllib.error.HTTPError as error:
        assert error.code == 404
finally:
    try:
        out, err = process.communicate("\n", timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate()
        raise
    assert process.returncode == 0, err
bad = subprocess.run([server, document + ".missing", assets], capture_output=True, text=True, timeout=10)
assert bad.returncode != 0 and "Asset" in bad.stderr
print("UI HTTP routes, missing asset rejection, shutdown passed")
