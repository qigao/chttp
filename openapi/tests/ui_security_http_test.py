"""Final OpenAPI UI security/escaping acceptance against the real server."""

import json
import pathlib
import re
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

server, assets = sys.argv[1:]


def fetch(origin, path):
    with urllib.request.urlopen(origin + path, timeout=5) as response:
        body = response.read()
        assert response.status == 200
        assert response.headers["X-Content-Type-Options"] == "nosniff"
        return response, body


document = {
    "openapi": "3.1.0",
    "info": {
        "title": '<script>alert("title")</script> & 中',
        "version": '1&"',
    },
    "servers": [{"url": "https://api.example.test/v1"}],
    "paths": {
        "/evil": {
            "get": {
                "operationId": "evilOp",
                "summary": 'sum <script>alert("summary")</script> &',
                "description": "desc <b>bold</b> & 中",
                "parameters": [
                    {
                        "name": "q",
                        "in": "query",
                        "required": False,
                        "description": "param <script>p</script>",
                        "schema": {
                            "type": "string",
                            "description": "schema <script>s</script>",
                        },
                    }
                ],
                "responses": {
                    "200": {
                        "description": "resp <script>r</script> &",
                    }
                },
            }
        }
    },
}

with tempfile.TemporaryDirectory() as tmp:
    document_path = pathlib.Path(tmp) / "openapi.json"
    document_path.write_text(
        json.dumps(document, ensure_ascii=False), encoding="utf-8"
    )

    process = subprocess.Popen(
        [server, str(document_path), assets],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        line = process.stdout.readline()
        match = re.search(r"http://127\.0\.0\.1:\d+", line)
        assert match, f"server did not start: {line!r}"
        origin = match.group()

        response, docs = fetch(origin, "/docs")
        assert response.headers.get_content_type() == "text/html"
        assert b"&lt;script&gt;alert" in docs
        assert b'<script>alert("title")</script>' not in docs
        assert b"x-data=" not in docs
        assert b"/docs/htmx.js" in docs
        assert b"/docs/tryit.js" in docs

        response, detail = fetch(origin, "/docs/operations/evilOp")
        assert response.headers.get_content_type() == "text/html"
        assert b"&lt;script&gt;alert" in detail
        assert b"&lt;b&gt;bold&lt;/b&gt;" in detail
        assert b"&lt;script&gt;s&lt;/script&gt;" in detail
        assert b"&lt;script&gt;r&lt;/script&gt;" in detail
        assert b'data-tryit' in detail
        assert b'https://api.example.test/v1' in detail

        response, raw = fetch(origin, "/openapi.json")
        assert response.headers.get_content_type() == "application/json"
        parsed = json.loads(raw)
        assert parsed["info"]["title"] == document["info"]["title"]

        try:
            urllib.request.urlopen(origin + "/docs/tryit/proxy", timeout=5)
            raise AssertionError("server-side Try-it proxy unexpectedly exists")
        except urllib.error.HTTPError as error:
            assert error.code == 404
    finally:
        try:
            _, stderr = process.communicate("\n", timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            raise
        assert process.returncode == 0, stderr

    missing_assets = pathlib.Path(tmp) / "missing-assets"
    missing_assets.mkdir()
    failed = subprocess.run(
        [server, str(document_path), str(missing_assets)],
        input="\n",
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert failed.returncode != 0
    assert "Asset" in failed.stderr or "Template" in failed.stderr

print("OpenAPI UI final escaping/security acceptance passed")
