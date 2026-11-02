#!/usr/bin/env python3
"""Exercise macOS trust restrictions on disposable GitHub Actions runners only."""

import argparse
import hashlib
import http.server
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading
import uuid


def main():
    if sys.platform != "darwin" or os.environ.get("GITHUB_ACTIONS") != "true":
        raise SystemExit("This test modifies trust settings; run only on a disposable macOS CI runner")
    parser = argparse.ArgumentParser()
    parser.add_argument("tests", type=Path)
    parser.add_argument("--openssl", required=True)
    args = parser.parse_args()
    tests = args.tests.resolve()

    def run(*command, **kwargs):
        return subprocess.run(command, check=True, timeout=60, **kwargs)

    with tempfile.TemporaryDirectory(prefix="eglantine-tls-") as temporary:
        directory = Path(temporary)
        servers = []
        installed = False
        root = directory / "root.pem"
        keychain = "/Library/Keychains/System.keychain"
        identity = uuid.uuid4().hex

        def openssl(*arguments):
            run(args.openssl, *map(str, arguments), stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE)

        def certificate(name, issuer=None, hostname="localhost"):
            cert = directory / f"{name}.pem"
            key = directory / f"{name}.key"
            subject = f"/CN=Eglantine CI {identity} {name}" if issuer is None else f"/CN={hostname}"
            if issuer is None:
                openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", subject, "-keyout", key, "-out", cert,
                        "-addext", "basicConstraints=critical,CA:TRUE",
                        "-addext", "keyUsage=critical,keyCertSign,cRLSign")
            else:
                csr = directory / f"{name}.csr"
                extensions = directory / f"{name}.ext"
                extensions.write_text(
                    "basicConstraints=critical,CA:FALSE\n"
                    "keyUsage=critical,digitalSignature,keyEncipherment\n"
                    "extendedKeyUsage=serverAuth\n"
                    f"subjectAltName=DNS:{hostname}" +
                    (",IP:127.0.0.1" if hostname == "localhost" else "") + "\n",
                    encoding="ascii")
                openssl("req", "-new", "-newkey", "rsa:2048", "-nodes", "-subj", subject,
                        "-keyout", key, "-out", csr)
                openssl("x509", "-req", "-in", csr, "-CA", directory / f"{issuer}.pem",
                        "-CAkey", directory / f"{issuer}.key", "-CAcreateserial", "-days", "1",
                        "-extfile", extensions, "-out", cert)
            return cert, key

        certificate("root")
        certificate("untrusted-root")
        good = certificate("good", "root")
        untrusted = certificate("untrusted", "untrusted-root")
        wrong_host = certificate("wrong-host", "root", "wrong.invalid")
        fingerprint = hashlib.sha1(ssl.PEM_cert_to_DER_cert(root.read_text())).hexdigest()

        class Handler(http.server.BaseHTTPRequestHandler):
            redirects = {}

            def do_GET(self):
                if self.path in self.redirects:
                    self.send_response(302)
                    self.send_header("Location", self.redirects[self.path])
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                body = b'{"ok":true}\n'
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *arguments):
                pass

        def serve(pair):
            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(*pair)
            server.socket = context.wrap_socket(server.socket, server_side=True)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            servers.append(server)
            return server.server_port

        def clear_trust():
            nonlocal installed
            if installed:
                try:
                    run("sudo", "-n", "security", "remove-trusted-cert", "-d", str(root))
                finally:
                    run("sudo", "-n", "security", "delete-certificate", "-Z", fingerprint, keychain)
                    installed = False

        def trust(*settings):
            nonlocal installed
            clear_trust()
            # Only this uniquely generated public certificate enters the keychain;
            # no private keys or existing certificates/settings are modified.
            installed = True
            run("sudo", "-n", "security", "add-trusted-cert", "-d", "-k", keychain,
                *settings, str(root))

        def check(label, accepted, path="/"):
            print(f"Native trust case: {label}", flush=True)
            environment = dict(os.environ, EGLANTINE_TEST_TLS_URL=f"https://localhost:{origin}",
                               EGLANTINE_TEST_TLS_PATH=path,
                               EGLANTINE_TEST_TLS_ACCEPT="1" if accepted else "0",
                               SSL_CERT_FILE="/dev/null", SSL_CERT_DIR=str(directory / "no-ca-files"))
            run(str(tests), "[macos_trust]", env=environment)

        def check_custom_ca():
            environment = dict(os.environ, EGLANTINE_TEST_TLS_URL=f"https://localhost:{origin}",
                               EGLANTINE_TEST_TLS_CA_FILE=str(root),
                               EGLANTINE_TEST_TLS_UNRELATED_CA_FILE=str(directory / "untrusted-root.pem"),
                               SSL_CERT_FILE="/dev/null", SSL_CERT_DIR=str(directory / "no-ca-files"))
            run(str(tests), "[macos_custom_ca]", env=environment)

        try:
            origin = serve(good)
            Handler.redirects = {
                "/redirect-good": f"https://127.0.0.1:{serve(good)}/",
                "/redirect-untrusted": f"https://127.0.0.1:{serve(untrusted)}/",
                "/redirect-wrong-host": f"https://127.0.0.1:{serve(wrong_host)}/",
            }
            check("untrusted root", False)
            check_custom_ca()
            trust("-r", "trustRoot", "-p", "ssl")
            check("trusted SSL root without filesystem CA files", True)
            check_custom_ca()
            check("trusted cross-host redirect", True, "/redirect-good")
            check("redirect to untrusted root", False, "/redirect-untrusted")
            check("redirect with wrong hostname", False, "/redirect-wrong-host")
            trust("-r", "trustRoot", "-p", "codeSign")
            check("root trusted only for code signing", False)
            trust("-r", "trustRoot", "-p", "ssl", "-s", "wrong.invalid")
            check("root trusted only for another hostname", False)
            trust("-r", "deny")
            check("explicitly distrusted root", False)
        finally:
            try:
                clear_trust()
            finally:
                for server in servers:
                    server.shutdown()
                    server.server_close()


if __name__ == "__main__":
    main()
