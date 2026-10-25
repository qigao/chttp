#!/usr/bin/env python3
"""CHttp/CNet/GmSSL port14 TLS 1.3 independent OpenSSL mTLS acceptance.

Fresh per-test CA, server cert with localhost SAN, verified client identities.
No local-file source shortcut, no nonstandard TLS option for default profile.
Default OpenSSL X25519-first HRR and dummy CCS is mandatory. Negative tests
must fail before application authority or return the exact forbidden status.
"""
from __future__ import annotations

import hashlib
import json
import pathlib
import select
import ssl
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

TIMEOUT = 7


def openssl(*args: str) -> None:
    subprocess.run(["openssl", *args], check=True,
                   capture_output=True, timeout=20)


def generate(root: pathlib.Path):
    ca, cak = root / "ca.crt", root / "ca.key"
    openssl("req", "-x509", "-newkey", "ec",
            "-pkeyopt", "ec_paramgen_curve:prime256v1",
            "-sha256", "-nodes", "-days", "2", "-subj", "/CN=CHttp Port14 CA",
            "-out", str(ca), "-keyout", str(cak),
            "-addext", "basicConstraints=critical,CA:TRUE,pathlen:0",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    bad_ca, bad_cak = root / "untrusted.crt", root / "untrusted.key"
    openssl("req", "-x509", "-newkey", "ec",
            "-pkeyopt", "ec_paramgen_curve:prime256v1",
            "-sha256", "-nodes", "-days", "2", "-subj", "/CN=Unknown CA",
            "-out", str(bad_ca), "-keyout", str(bad_cak),
            "-addext", "basicConstraints=critical,CA:TRUE,pathlen:0",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign")

    def leaf(name, issuer, issuer_key, purpose):
        crt, key = root / (name+".crt"), root / (name+".key")
        csr, ext = root / (name+".csr"), root / (name+".ext")
        ext.write_text("basicConstraints=critical,CA:FALSE\n"
                       "keyUsage=critical,digitalSignature\n"
                       "extendedKeyUsage="+purpose+"\n"+
                       ("subjectAltName=DNS:localhost,IP:127.0.0.1\n"
                        if name == "server" else ""), encoding="ascii")
        openssl("req", "-newkey", "ec",
                "-pkeyopt", "ec_paramgen_curve:prime256v1",
                "-sha256", "-nodes", "-subj", "/CN="+name,
                "-out", str(csr), "-keyout", str(key))
        openssl("x509", "-req", "-in", str(csr),
                "-CA", str(issuer), "-CAkey", str(issuer_key),
                "-CAcreateserial", "-sha256", "-days", "2",
                "-extfile", str(ext), "-out", str(crt))
        return crt, key

    server = leaf("server", ca, cak, "serverAuth")
    allowed = leaf("allowed", ca, cak, "clientAuth")
    unpinned = leaf("not-pinned", ca, cak, "clientAuth")
    wrong_issuer = leaf("wrong-issuer", bad_ca, bad_cak, "clientAuth")
    wrong_eku = leaf("wrong-eku", ca, cak, "serverAuth")
    openssl("verify", "-CAfile", str(ca),
            "-purpose", "sslserver", "-verify_hostname", "localhost",
            str(server[0]))
    openssl("verify", "-CAfile", str(ca),
            "-purpose", "sslclient", str(allowed[0]))
    return ca, server, allowed, unpinned, wrong_issuer, wrong_eku


def context(ca, identity, direct_p256=False, middlebox=True):
    c = ssl.create_default_context(cafile=str(ca))
    c.minimum_version = ssl.TLSVersion.TLSv1_3
    c.maximum_version = ssl.TLSVersion.TLSv1_3
    c.set_alpn_protocols(["http/1.1"])
    if direct_p256:
        c.set_ecdh_curve("prime256v1")
    if not middlebox:
        c.options &= ~ssl.OP_ENABLE_MIDDLEBOX_COMPAT
    if identity is not None:
        c.load_cert_chain(str(identity[0]), str(identity[1]))
    return c


def fetch(port, tls_context, forwarded=None):
    url = "https://127.0.0.1:" + str(port) + "/interop"
    headers = {}
    if forwarded is not None:
        headers["X-Forwarded-Client-Cert"] = forwarded
    request = urllib.request.Request(url, headers=headers, method="GET")
    try:
        with urllib.request.urlopen(request, context=tls_context,
                                    timeout=TIMEOUT) as result:
            return result.status, result.read()
    except urllib.error.HTTPError as error:
        return error.code, error.read()


def main(binary):
    binary = pathlib.Path(binary).resolve()
    if not binary.is_file():
        raise RuntimeError("missing source-built CHttp TLS probe; no fallback")
    with tempfile.TemporaryDirectory(prefix="chttp-port14-mtls-") as temp:
        root = pathlib.Path(temp)
        ca, server, allowed, unpinned, wrong_issuer, wrong_eku = generate(root)
        sha = hashlib.sha256(
            ssl.PEM_cert_to_DER_cert(
                allowed[0].read_text(encoding="ascii"))).hexdigest()
        process = subprocess.Popen(
            [str(binary), str(ca), str(server[0]), str(server[1]), sha],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1)
        try:
            assert process.stdout is not None
            ready, _, _ = select.select([process.stdout], [], [], TIMEOUT)
            if not ready:
                raise AssertionError("CHttp mTLS source listener not ready")
            line = process.stdout.readline().strip()
            if not line.startswith("CHTTP_TLS14_PORT="):
                raise AssertionError("bad CHttp mTLS startup: "+line)
            port = int(line.partition("=")[2])
            if not 0 < port <= 65535:
                raise AssertionError("invalid listener port")

            for label, p256, compatible in [
                ("P256-no-CCS", True, False),
                ("P256-default-CCS", True, True),
                ("X25519-HRR-no-CCS", False, False),
                ("X25519-HRR-default-CCS", False, True),
            ]:
                status, body = fetch(port, context(
                    ca, allowed, p256, compatible))
                if status != 200 or json.loads(body).get("mTLS") is not True:
                    raise AssertionError(
                        label + ": no verified HTTP 200 identity response")
                print("chttp.tls14.openssl: PASS "+label, flush=True)

            status, _ = fetch(port, context(ca, unpinned), forwarded=sha)
            if status != 403:
                raise AssertionError(
                    "trusted CA but unpinned identity or forged header admitted")
            print("chttp.tls14.openssl: PASS unpinned-service rejection",
                  flush=True)
            for label, cert in [
                ("missing-client-cert", None),
                ("wrong-client-ca", wrong_issuer),
                ("wrong-client-EKU", wrong_eku),
            ]:
                try:
                    status, body = fetch(port, context(ca, cert))
                    if status == 200 or b'mTLS' in body:
                        raise AssertionError(label+" reached authenticated route")
                    # A TLS failure must not be substituted with an HTTP 401:
                    # the client cert is mandatory, so route cannot run.
                    raise AssertionError(label+" reached HTTP routing")
                except (ssl.SSLError, urllib.error.URLError, OSError):
                    print("chttp.tls14.openssl: PASS "+label, flush=True)
        finally:
            if process.poll() is None and process.stdin:
                try:
                    process.stdin.write("\n")
                    process.stdin.flush()
                    process.wait(timeout=10)
                except (BrokenPipeError, subprocess.TimeoutExpired):
                    process.kill()
                    process.wait(timeout=4)
            else:
                process.wait(timeout=4)
            if process.returncode != 0:
                diagnostics = process.stderr.read()[-1100:] if process.stderr else ""
                raise AssertionError(
                    "source CHttp mTLS probe terminated "+
                    str(process.returncode)+": "+diagnostics)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify_tls14_openssl.py ./chttp_tls14_server_probe")
    main(sys.argv[1])
