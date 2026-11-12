"""Run the public reflected Application example through its real HTTP entry point."""
import json
import queue
import re
import subprocess
import sys
import threading
import urllib.error
import urllib.request

process = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, text=True)
lines = queue.Queue()
reader = threading.Thread(target=lambda: lines.put(process.stdout.readline()), daemon=True)
reader.start()
try:
    try:
        line = lines.get(timeout=10)
    except queue.Empty as error:
        raise AssertionError("Example did not publish its listener within 10 seconds") from error
    match = re.match(r"(http://127\.0\.0\.1:\d+)/answer", line)
    assert match, f"Example did not publish its listener: {line!r}"
    origin = match.group(1)
    for path, expected in (("/answer", 200), ("/reject", 403),
                           ("/answer", 200), ("/missing", 404)):
        try:
            response = urllib.request.urlopen(origin + path, timeout=5)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            assert response.status == expected, (path, response.status)
            assert response.headers["X-Example-Interceptor"] == "ACE", path
            body = response.read()
            if expected == 200:
                assert response.headers.get_content_type() == "application/json"
                assert json.loads(body) == {"answer": 42}
            elif expected == 403:
                assert body == b"Rejected"
finally:
    try:
        _, stderr = process.communicate("\n", timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        _, stderr = process.communicate()
        raise AssertionError(f"Example did not complete shutdown: {stderr}")
    reader.join(timeout=1)
    assert process.returncode == 0, stderr
print("Reflected Controller/Service/Interceptor startup, requests and shutdown passed")
