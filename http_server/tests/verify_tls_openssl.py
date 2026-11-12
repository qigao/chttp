"""Formal independent OpenSSL TLS 1.3/mTLS and HTTP identity qualification."""
from __future__ import annotations

import hashlib
import pathlib
import queue
import re
import ssl
import subprocess
import sys
import tempfile
import threading

TIMEOUT = 10
OPENSSL = sys.argv[2]
HRR_RANDOM = bytes.fromhex("cf21ad74e59a6111be1d8c021e65b891c2a211167abb8c5e079e09e2c8a8339c")


def openssl(*args: str) -> None:
    result = subprocess.run([OPENSSL, *args], capture_output=True, timeout=20)
    if result.returncode:
        raise AssertionError("OpenSSL credential preparation failed: " + result.stderr.decode(errors="replace"))


def generate(root: pathlib.Path):
    ca, cak = root / "ca.crt", root / "ca.key"
    openssl("req", "-x509", "-newkey", "ec",
            "-pkeyopt", "ec_paramgen_curve:prime256v1",
            "-sha256", "-nodes", "-days", "2", "-subj", "/CN=CHttp Interop CA",
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



def exchange(port, ca, identity, groups, middlebox, forwarded=None):
    command = [OPENSSL, "s_client", "-connect", f"127.0.0.1:{port}",
               "-servername", "localhost", "-verify_hostname", "localhost",
               "-verify_return_error", "-CAfile", str(ca), "-tls1_3",
               "-alpn", "http/1.1", "-groups", groups, "-msg", "-ign_eof"]
    if not middlebox:
        command.append("-no_middlebox")
    if identity is not None:
        command += ["-cert", str(identity[0]), "-key", str(identity[1])]
    request = "GET /interop HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
    if forwarded is not None:
        request += "X-Forwarded-Client-Cert: " + forwarded + "\r\n"
    result = subprocess.run(command, input=(request+"\r\n").encode("ascii"),
                            capture_output=True, timeout=TIMEOUT)
    output = result.stdout.decode("ascii", errors="replace")
    diagnostics = result.stderr.decode("ascii", errors="replace")
    statuses = re.findall(r"HTTP/1\.[01] ([0-9]{3})", output)
    return result.returncode, output, diagnostics, statuses


def verify_handshake(output, hrr, middlebox):
    server_hellos = re.findall(
        r"^<<< [^\n]*ServerHello[^\n]*\n((?:[ \t]+(?:[0-9a-f]{2}[ \t]*)+\r?\n)+)",
        output, flags=re.MULTILINE)
    retries = sum(HRR_RANDOM in bytes.fromhex(block) for block in server_hellos)
    if retries != int(hrr):
        raise AssertionError(f"expected HRR={hrr}, observed {retries} ServerHello retries\n{output}")
    ccs = bool(re.search(r"^>>> [^\n]*ChangeCipherSpec", output, flags=re.MULTILINE))
    if ccs != middlebox:
        raise AssertionError(f"expected outbound dummy CCS={middlebox}, observed {ccs}")


def main(binary):
    version = subprocess.run([OPENSSL, "version"], check=True, capture_output=True,
                             text=True, timeout=TIMEOUT).stdout.strip()
    if not version.startswith("OpenSSL 3."):
        raise AssertionError("OpenSSL 3.x test tool required: " + version)
    print(version, flush=True)
    with tempfile.TemporaryDirectory(prefix="chttp-openssl-mtls-") as temp:
        root = pathlib.Path(temp)
        ca, server, allowed, unpinned, wrong_issuer, wrong_eku = generate(root)
        sha = hashlib.sha256(ssl.PEM_cert_to_DER_cert(
            allowed[0].read_text(encoding="ascii"))).hexdigest()
        # stderr goes to a file so startup/shutdown cannot deadlock on a full pipe.
        with (root / "server.log").open("w+b") as log:
            process = subprocess.Popen(
                [str(pathlib.Path(binary).resolve()), str(ca), str(server[0]), str(server[1]), sha],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True)
            startup = queue.Queue(maxsize=1)
            reader = threading.Thread(target=lambda: startup.put(process.stdout.readline()), daemon=True)
            reader.start()
            try:
                line = startup.get(timeout=TIMEOUT).strip()
                if not line.startswith("CHTTP_TLS_INTEROP_PORT="):
                    raise AssertionError("invalid TLS listener startup: " + line)
                port = int(line.partition("=")[2])
                if not 0 < port <= 65535:
                    raise AssertionError("invalid listener port")
                for label, groups, middlebox, hrr in [
                    ("P256-no-CCS", "P-256", False, False),
                    ("P256-CCS", "P-256", True, False),
                    ("X25519-HRR-no-CCS", "X25519:P-256", False, True),
                    ("X25519-HRR-CCS", "X25519:P-256", True, True),
                ]:
                    code, output, diagnostics, statuses = exchange(
                        port, ca, allowed, groups, middlebox)
                    if code or statuses != ["200"] or '{"mTLS":true' not in output:
                        raise AssertionError(label + " failed: " + diagnostics + output)
                    verify_handshake(output, hrr, middlebox)
                    print("PASS " + label, flush=True)

                code, output, diagnostics, statuses = exchange(
                    port, ca, unpinned, "P-256", True, forwarded=sha)
                if code or statuses != ["403"]:
                    raise AssertionError("unpinned/forged identity admitted: " + diagnostics + output)
                print("PASS trusted-but-unpinned and forged forwarded identity", flush=True)
                for label, cert in [("missing-client-cert", None),
                                    ("wrong-client-ca", wrong_issuer),
                                    ("wrong-client-EKU", wrong_eku)]:
                    code, output, diagnostics, statuses = exchange(port, ca, cert, "P-256", True)
                    if not code or statuses or "alert" not in (output + diagnostics).lower():
                        raise AssertionError(label + " did not fail at TLS: " + diagnostics + output)
                    print("PASS " + label, flush=True)
                # Negative handshakes must not poison the listener or pin state.
                code, output, diagnostics, statuses = exchange(port, ca, allowed, "P-256", True)
                if code or statuses != ["200"]:
                    raise AssertionError("listener not reusable after rejection: " + diagnostics + output)
            finally:
                timed_out = False
                try:
                    process.communicate(input="\n", timeout=TIMEOUT)
                except subprocess.TimeoutExpired:
                    timed_out = True
                    process.kill()
                    process.communicate(timeout=TIMEOUT)
                reader.join(timeout=TIMEOUT)
                if timed_out or process.returncode or reader.is_alive():
                    log.seek(0)
                    raise AssertionError("TLS fixture shutdown failed: " + log.read().decode(errors="replace"))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: verify_tls_openssl.py server-binary openssl-binary")
    main(sys.argv[1])
