"""Machine-check final OpenAPI UI cleanup and dependency boundaries."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
OPENAPI = ROOT / "openapi"

def read(path):
    return (ROOT / path).read_text(encoding="utf-8")

def link_block(text, target):
    marker = f"target_link_libraries({target}"
    start = text.find(marker)
    assert start >= 0, f"missing {marker}"
    end = text.find(")\n", start)
    assert end >= 0, f"unterminated {marker}"
    return text[start:end + 1]

removed = [
    "openapi/ui/app.js",
    "openapi/ui/index.html",
    "openapi/tests/ui_test.cjs",
    "openapi/ui/vendor/alpine-3.14.9.min.js",
    "openapi/ui/vendor/LICENSE.alpine.md",
    "openapi/ui/vendor/README.md",
]
for path in removed:
    assert not (ROOT / path).exists(), f"obsolete migration file remains: {path}"

assert (ROOT / "openapi/ui/vendor/htmx-4.0.0.min.js").is_file()
assert (ROOT / "openapi/ui/vendor/htmx-4.0.0.LICENSE").is_file()
assert (ROOT / "openapi/ui/vendor/HTMX.md").is_file()

templates = "\n".join(
    path.read_text(encoding="utf-8")
    for path in sorted((OPENAPI / "ui/templates").glob("*.html"))
)
for legacy in ("x-data=", "x-for=", "x-model=", "x-show=", "hx-on"):
    assert legacy not in templates, f"legacy/inline expression remains: {legacy}"

layout = read("openapi/ui/templates/layout.html")
assert 'src="/docs/htmx.js"' in layout
assert 'src="/docs/tryit.js"' in layout
assert not re.search(r'(?:src|href)=["\']https?://', layout)

server = read("openapi/examples/ui_server.c")
for removed_route in ("/docs/app.js", "/docs/alpine.js", "/docs/tryit/proxy"):
    assert removed_route not in server, f"obsolete/proxy route remains: {removed_route}"
for route in ("/docs/style.css", "/docs/htmx.js", "/docs/tryit.js", "/openapi.json"):
    assert route in server, f"final static route missing: {route}"

openapi_cmake = read("openapi/CMakeLists.txt")
generator_links = link_block(openapi_cmake, "openapi_generator")
renderer_links = link_block(openapi_cmake, "openapi_ui_renderer")
assert "Jinja" not in generator_links and "HTMX" not in generator_links
assert "Salts::JinjaCMeta" in renderer_links
assert openapi_cmake.count("Salts::JinjaCMeta") == 1

server_cmake = read("http_server/CMakeLists.txt")
server_links = link_block(server_cmake, "chttp_server")
assert "Jinja" not in server_links and "HTMX" not in server_links

examples_cmake = read("openapi/examples/CMakeLists.txt")
assert "openapi_ui_renderer" in examples_cmake
assert "Salts::JinjaCMeta" not in examples_cmake

print("OpenAPI final cleanup and dependency audit passed")
