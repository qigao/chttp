"""Verify the server-rendered OpenAPI full page and fragment routes."""
import json
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

def fetch(origin, path):
    with urllib.request.urlopen(origin + path, timeout=5) as response:
        assert response.status == 200
        assert response.headers.get_content_type() == "text/html"
        assert response.headers["X-Content-Type-Options"] == "nosniff"
        return response.read()

try:
    line = process.stdout.readline()
    match = re.search(r"http://127\.0\.0\.1:\d+", line)
    assert match, f"Server did not start: {line}"
    origin = match.group()

    docs = fetch(origin, "/docs")
    assert b"Pets API" in docs
    assert b"list_pets" in docs
    assert b"createPet" in docs
    assert b"List pets" in docs

    operation_list = fetch(origin, "/docs/operations")
    assert b"list_pets" in operation_list
    assert b"createPet" in operation_list
    assert b"/pets" in operation_list

    list_detail = fetch(origin, "/docs/operations/list_pets")
    assert b"List pets" in list_detail
    assert b"limit" in list_detail
    assert b"/pets" in list_detail
    assert b"Create a pet" not in list_detail

    create_detail = fetch(origin, "/docs/operations/createPet")
    assert b"Create a pet" in create_detail
    assert b"Pet to create" in create_detail
    assert b"List pets" not in create_detail

    try:
        urllib.request.urlopen(origin + "/docs/operations/not-a-real-operation", timeout=5)
        raise AssertionError("Unknown operation key unexpectedly succeeded")
    except urllib.error.HTTPError as error:
        assert error.code == 404

    with urllib.request.urlopen(origin + "/openapi.json", timeout=5) as response:
        assert response.status == 200
        assert response.headers.get_content_type() == "application/json"
        assert json.loads(response.read())["openapi"] == "3.1.0"
finally:
    try:
        out, err = process.communicate("\n", timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate()
        raise
    assert process.returncode == 0, err

print("OpenAPI Jinja full-page and fragment HTTP routes passed")
