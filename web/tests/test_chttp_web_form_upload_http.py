import html
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


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def post_form(opener, url, fields, headers=None):
    data = urllib.parse.urlencode(fields, doseq=True).encode("utf-8")
    req_headers = {"Content-Type": "application/x-www-form-urlencoded"}
    if headers:
        req_headers.update(headers)
    return opener.open(
        urllib.request.Request(
            url, data=data, headers=req_headers, method="POST"
        ),
        timeout=5,
    )


def expect_http_error(opener, request, code):
    try:
        opener.open(request, timeout=5)
        raise AssertionError(f"expected HTTP {code}")
    except urllib.error.HTTPError as exc:
        assert exc.code == code, (exc.code, code)
        return exc


def multipart_body(boundary, fields, filename, file_bytes):
    chunks = []
    for name, value in fields:
        chunks.extend(
            [
                f"--{boundary}\r\n".encode(),
                (
                    f'Content-Disposition: form-data; name="{name}"'
                    "\r\n\r\n"
                ).encode(),
                value.encode("utf-8"),
                b"\r\n",
            ]
        )
    chunks.extend(
        [
            f"--{boundary}\r\n".encode(),
            (
                'Content-Disposition: form-data; name="file"; '
                f'filename="{filename}"\r\n'
            ).encode("utf-8"),
            b"Content-Type: text/plain\r\n\r\n",
            file_bytes,
            b"\r\n",
            f"--{boundary}--\r\n".encode(),
        ]
    )
    return b"".join(chunks)


try:
    line = proc.stdout.readline().strip()
    assert line.startswith(
        "CHttp::Web forms example: http://127.0.0.1:"
    ), line
    forms_url = line.split("CHttp::Web forms example: ", 1)[1]
    assert forms_url.endswith("/forms"), forms_url
    base = forms_url[: -len("forms")]

    jar = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(
        urllib.request.HTTPCookieProcessor(jar)
    )
    no_redirect = urllib.request.build_opener(
        urllib.request.HTTPCookieProcessor(jar), NoRedirect()
    )

    with opener.open(forms_url, timeout=5) as response:
        page = response.read()
        assert response.status == 200
        assert response.headers.get_content_type() == "text/html"
        assert response.headers.get_content_charset() == "utf-8"

    assert page.startswith(b"<!doctype html>")
    assert b'action="/profile"' in page
    assert b'hx-post="/profile"' in page
    assert b'action="/upload"' in page
    assert b'enctype="multipart/form-data"' in page
    assert b'hx-post="/upload"' in page
    tokens = re.findall(rb'name="_csrf" value="([0-9a-f]{64})"', page)
    assert len(tokens) == 2, tokens
    token = tokens[0].decode("ascii")
    assert tokens[0] == tokens[1]

    hostile_note = "<script>alert(note)</script>"
    invalid_data = urllib.parse.urlencode(
        {"_csrf": token, "name": "x", "note": hostile_note}
    ).encode("utf-8")
    invalid = urllib.request.Request(
        base + "profile",
        data=invalid_data,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
        method="POST",
    )
    exc = expect_http_error(opener, invalid, 422)
    invalid_page = exc.read()
    assert invalid_page.startswith(b"<!doctype html>")
    assert b"name must contain at least 3 characters" in invalid_page
    escaped_note = html.escape(hostile_note, quote=True).encode()
    assert escaped_note in invalid_page
    assert hostile_note.encode() not in invalid_page

    invalid_hx = urllib.request.Request(
        base + "profile",
        data=invalid_data,
        headers={
            "Content-Type": "application/x-www-form-urlencoded",
            "HX-Request": "true",
        },
        method="POST",
    )
    exc = expect_http_error(opener, invalid_hx, 422)
    fragment = exc.read()
    assert not fragment.startswith(b"<!doctype html>")
    assert exc.headers.get("HX-Retarget") == "#profile"
    assert b'id="profile"' in fragment
    assert escaped_note in fragment
    assert hostile_note.encode() not in fragment

    try:
        post_form(
            no_redirect,
            base + "profile",
            {"_csrf": token, "name": "Alice", "note": "server driven"},
        )
        raise AssertionError("303 profile redirect unexpectedly followed")
    except urllib.error.HTTPError as exc:
        assert exc.code == 303
        assert exc.headers.get("Location") == "/forms"
        assert exc.read() == b""

    with opener.open(forms_url, timeout=5) as response:
        saved = response.read()
    assert b'<p id="flash">profile saved</p>' in saved
    assert b'value="Alice"' in saved
    assert b"server driven" in saved

    with opener.open(forms_url, timeout=5) as response:
        no_flash = response.read()
    assert b'<p id="flash"></p>' in no_flash

    hostile_label = "<img src=x onerror=upload>"
    hostile_filename = "evil<script>.txt"
    boundary = "FormsBoundary"
    upload_body = multipart_body(
        boundary,
        [("_csrf", token), ("label", hostile_label)],
        hostile_filename,
        b"payload",
    )
    upload_req = urllib.request.Request(
        base + "upload",
        data=upload_body,
        headers={
            "Content-Type": f"multipart/form-data; boundary={boundary}",
        },
        method="POST",
    )
    try:
        no_redirect.open(upload_req, timeout=5)
        raise AssertionError("303 upload redirect unexpectedly followed")
    except urllib.error.HTTPError as exc:
        assert exc.code == 303
        assert exc.headers.get("Location") == "/forms"
        assert exc.read() == b""

    with opener.open(forms_url, timeout=5) as response:
        upload_page = response.read()
    assert b'<p id="flash">upload saved</p>' in upload_page
    escaped_label = html.escape(hostile_label, quote=True).encode()
    escaped_filename = html.escape(hostile_filename, quote=True).encode()
    assert escaped_label in upload_page
    assert escaped_filename in upload_page
    assert hostile_label.encode() not in upload_page
    assert hostile_filename.encode() not in upload_page

    hx_filename = "next<&>.txt"
    hx_label = "htmx<&>"
    hx_body = multipart_body(
        boundary,
        [("label", hx_label)],
        hx_filename,
        b"next",
    )
    hx_req = urllib.request.Request(
        base + "upload",
        data=hx_body,
        headers={
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "HX-Request": "true",
            "X-CSRF-Token": token,
        },
        method="POST",
    )
    with opener.open(hx_req, timeout=5) as response:
        hx_fragment = response.read()
        assert response.status == 200
        assert response.headers.get("HX-Trigger") == "upload:saved"
        assert response.headers.get("HX-Retarget") == "#upload"
    assert not hx_fragment.startswith(b"<!doctype html>")
    assert b'id="upload"' in hx_fragment
    assert b"upload saved" in hx_fragment
    assert html.escape(hx_label, quote=True).encode() in hx_fragment
    assert html.escape(hx_filename, quote=True).encode() in hx_fragment
    assert hx_label.encode() not in hx_fragment
    assert hx_filename.encode() not in hx_fragment

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
print("CHttp::Web validation/upload reference app verification passed")
