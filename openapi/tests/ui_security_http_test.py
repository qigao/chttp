"""Final OpenAPI UI security/escaping acceptance against the real server."""

import json
import pathlib
import re
import shutil
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
                "requestBody": {
                    "required": True,
                    "description": "body <script>b</script> &",
                    "content": {
                        "application/json": {
                            "schema": {
                                "type": "object",
                                "description": "body schema <script>bs</script>",
                            }
                        }
                    },
                },
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
        assert b'<script>alert("summary")</script>' not in docs
        assert b"x-data=" not in docs
        assert b"hx-on" not in docs
        assert b"/docs/htmx.js" in docs
        assert b"/docs/tryit.js" in docs

        response, detail = fetch(origin, "/docs/operations/evilOp")
        assert response.headers.get_content_type() == "text/html"
        assert b"&lt;script&gt;alert" in detail
        assert b"&lt;b&gt;bold&lt;/b&gt;" in detail
        assert b"&lt;script&gt;s&lt;/script&gt;" in detail
        assert b"&lt;script&gt;b&lt;/script&gt;" in detail
        assert b"&lt;script&gt;bs&lt;/script&gt;" in detail
        assert b"&lt;script&gt;r&lt;/script&gt;" in detail
        for raw_html in (
            b'<script>alert("summary")</script>',
            b"<b>bold</b>",
            b"<script>p</script>",
            b"<script>s</script>",
            b"<script>b</script>",
            b"<script>bs</script>",
            b"<script>r</script>",
        ):
            assert raw_html not in detail
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

    def copy_assets(name):
        destination = pathlib.Path(tmp) / name
        shutil.copytree(assets, destination)
        return destination

    def startup_failure(asset_root):
        failed = subprocess.run(
            [server, str(document_path), str(asset_root)],
            input="\n",
            capture_output=True,
            text=True,
            timeout=10,
        )
        assert failed.returncode != 0
        return failed.stderr

    missing_template = copy_assets("missing-template")
    (missing_template / "templates" / "docs.html").unlink()
    stderr = startup_failure(missing_template)
    assert "docs.html" in stderr
    assert "Asset" in stderr

    missing_static = copy_assets("missing-static")
    (missing_static / "style.css").unlink()
    stderr = startup_failure(missing_static)
    assert "style.css" in stderr
    assert "Asset" in stderr

    invalid_template = copy_assets("invalid-template")
    (invalid_template / "templates" / "docs.html").write_text(
        "{% if %}", encoding="utf-8"
    )
    stderr = startup_failure(invalid_template)
    assert "renderer startup failed" in stderr

    oversized_template = copy_assets("oversized-template")
    (oversized_template / "templates" / "docs.html").write_text(
        "x" * (64 * 1024 + 1), encoding="utf-8"
    )
    stderr = startup_failure(oversized_template)
    assert "65536" in stderr
    assert "docs.html" in stderr

print("OpenAPI UI final escaping/security acceptance passed")
