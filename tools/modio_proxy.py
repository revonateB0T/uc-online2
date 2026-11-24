#!/usr/bin/env python3
"""
mod.io proxy for the UCOnline2 / OnlineFix Steam-API spoof.

Why this exists
---------------
Space Engineers talks to mod.io through `Bin64/VRage.Mod.Io.dll` (a .NET
assembly).  It asks Steam for an *encrypted app ticket*, base64s it and POSTs
it to

    POST https://api.mod.io/v1/external/steamauth
    Content-Type: application/x-www-form-urlencoded
    appdata=<base64 of the encrypted app ticket>&terms_agreed=true

mod.io decrypts that ticket with the ticket key it has configured for the
game (app id 244850) and checks Valve's signature on it.  A ticket that was
minted for a different app -- or a locally forged one -- is rejected with
HTTP 401 / error ref 11018 ("invalid Steam encrypted app ticket"), so the
in-game mod browser never gets a session.

This tool lets you *supply your own* mod.io credentials instead: it sits in
front of api.mod.io only, forwards everything untouched, and rewrites the
answer of the external-auth endpoints with a token you obtained yourself.
Nothing else in the game's traffic is read or modified.

The access token is expected in the form mod.io hands out (the same bearer
token the mod.io website uses once you signed in with Steam).  Put it in a
file, one token per line, or a JSON object:

    {"access_token": "...", "refresh_token": "...", "date_expires": 1799999999}

Usage
-----
    python3 tools/modio_proxy.py init
    python3 tools/modio_proxy.py cert install      # trust the CA
    python3 tools/modio_proxy.py hosts add         # point api.mod.io here
    python3 tools/modio_proxy.py serve --token-file ~/.uc-online2-modio/token.json
    python3 tools/modio_proxy.py hosts remove
    python3 tools/modio_proxy.py cert remove
"""

import argparse
import http.client
import json
import os
import re
import shutil
import socket
import ssl
import subprocess
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_DIR = os.path.expanduser("~/.uc-online2-modio")
# The host Space Engineers talks to.  The game id (264) and the api key are
# hard coded in the game's own executable:
#   MyModIoService.Create(..., "spaceengineers", "264",
#                         "1fb4489996a5e8ffc6ec1135f9985b5b", "331",
#                         "f2b64abe55452252b030c48adc0c1f0e", ...)
# which is why the API moved from the old api.mod.io to g-<id>.modapi.io.
DEFAULT_HOST = "g-264.modapi.io"
DEFAULT_SANS = ["g-264.modapi.io", "g-331.modapi.io", "api.mod.io"]
HOSTS_MARKER = "# uc-online2 modio proxy"
UPSTREAM_TIMEOUT = 30

# Endpoints whose answer we may replace with our own credentials.
AUTH_ENDPOINTS = ("/external/steamauth", "/external/psnauth", "/external/xboxauth")

# Headers that must not be forwarded verbatim.
HOP_BY_HOP = {
    "connection", "keep-alive", "proxy-authenticate", "proxy-authorization",
    "te", "trailers", "transfer-encoding", "upgrade", "host", "content-length",
}


def log_line(path, text):
    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    line = "[%s] %s" % (stamp, text)
    print(line, flush=True)
    if path:
        try:
            with open(path, "a", encoding="utf-8") as fh:
                fh.write(line + "\n")
        except OSError:
            pass


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)


# ---------------------------------------------------------------- certificates

def cert_paths(directory):
    return {
        "ca_key": os.path.join(directory, "ca.key"),
        "ca_crt": os.path.join(directory, "ca.crt"),
        "key": os.path.join(directory, "server.key"),
        "crt": os.path.join(directory, "server.crt"),
    }


def cmd_init(args):
    directory = args.dir
    os.makedirs(directory, exist_ok=True)
    p = cert_paths(directory)
    if os.path.exists(p["crt"]) and not args.force:
        print("certificates already present in %s (use --force to regenerate)" % directory)
        return 0

    run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", p["ca_key"], "-out", p["ca_crt"], "-days", "3650",
         "-subj", "/CN=uc-online2 modio proxy CA"])

    san = ",".join("DNS:" + h for h in args.san)
    with open(os.path.join(directory, "server.ext"), "w", encoding="utf-8") as fh:
        fh.write("subjectAltName=%s\n" % san)
        fh.write("basicConstraints=CA:FALSE\n")
        fh.write("extendedKeyUsage=serverAuth\n")

    run(["openssl", "req", "-newkey", "rsa:2048", "-nodes",
         "-keyout", p["key"], "-out", os.path.join(directory, "server.csr"),
         "-subj", "/CN=%s" % args.san[0]])
    run(["openssl", "x509", "-req", "-sha256",
         "-in", os.path.join(directory, "server.csr"),
         "-CA", p["ca_crt"], "-CAkey", p["ca_key"], "-CAcreateserial",
         "-out", p["crt"], "-days", "3650",
         "-extfile", os.path.join(directory, "server.ext")])
    os.chmod(p["key"], 0o600)
    print("wrote CA and server certificate for %s into %s" % (", ".join(args.san), directory))
    print("next:  python3 tools/modio_proxy.py cert install")
    return 0


def proton_prefixes():
    """Every Proton/Wine prefix we can find on this machine."""
    roots = [
        os.path.expanduser("~/.steam/steam/steamapps/compatdata"),
        os.path.expanduser("~/.local/share/Steam/steamapps/compatdata"),
    ]
    found = []
    for root in roots:
        if not os.path.isdir(root):
            continue
        for name in sorted(os.listdir(root)):
            pfx = os.path.join(root, name, "pfx")
            if os.path.isdir(pfx):
                found.append(pfx)
    return found


def cmd_cert(args):
    directory = args.dir
    p = cert_paths(directory)
    if not os.path.exists(p["ca_crt"]):
        print("no CA certificate in %s -- run 'init' first" % directory)
        return 1
    ca = open(p["ca_crt"], encoding="utf-8").read()

    if args.action == "remove":
        for pfx in proton_prefixes():
            cert = os.path.join(pfx, "cert.pem")
            if not os.path.exists(cert):
                continue
            body = open(cert, encoding="utf-8").read()
            if "uc-online2 modio proxy CA" in body:
                body = re.sub(r"-----BEGIN CERTIFICATE-----.*?uc-online2 modio proxy CA.*?"
                              r"-----END CERTIFICATE-----\n?", "", body, flags=re.S)
                with open(cert, "w", encoding="utf-8") as fh:
                    fh.write(body)
                print("removed CA from %s" % cert)
        return 0

    for pfx in proton_prefixes():
        cert = os.path.join(pfx, "cert.pem")
        body = open(cert, encoding="utf-8").read() if os.path.exists(cert) else ""
        if "uc-online2 modio proxy CA" in body:
            print("CA already trusted by %s" % cert)
            continue
        if not body.endswith("\n"):
            body += "\n"
        body += ca
        with open(cert, "w", encoding="utf-8") as fh:
            fh.write(body)
        print("trusted CA in %s" % cert)

    # Mono (what the game runs on under Proton) may read the host CA bundle
    # instead of the Wine store, so install it there as well.
    system = "/usr/local/share/ca-certificates/uc-online2-modio.crt"
    if os.geteuid() == 0 and shutil.which("update-ca-certificates"):
        with open(system, "w", encoding="utf-8") as fh:
            fh.write(ca)
        run(["update-ca-certificates"])
        print("trusted CA system wide (%s)" % system)
    else:
        print("NOT installed system wide -- run as root if the game still rejects the certificate")
    return 0


# --------------------------------------------------------------------- /etc/hosts

def cmd_hosts(args):
    path = "/etc/hosts"
    body = open(path, encoding="utf-8").read()
    lines = [l for l in body.splitlines() if HOSTS_MARKER not in l]

    if args.action == "add":
        for host in [DEFAULT_HOST] + DEFAULT_SANS + list(args.alias):
            lines.append("127.0.0.1 %s %s" % (host, HOSTS_MARKER))
    elif args.action == "remove":
        lines = [l for l in lines if not (l.strip().startswith("127.0.0.1") and
                                          DEFAULT_HOST in l and HOSTS_MARKER in l)]
        body = "\n".join(lines) + "\n"
        open(path, "w", encoding="utf-8").write(body)
        print("removed the %s entry from %s" % (DEFAULT_HOST, path))
        return 0

    try:
        open(path, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    except PermissionError:
        print("need root privileges to edit %s" % path)
        return 1
    print("%s -> 127.0.0.1 in %s" % (DEFAULT_HOST, path))
    return 0


# ------------------------------------------------------------------------ server

def load_token(path):
    """Returns (access_token, refresh_token, date_expires) or (None, None, None)."""
    if not path or not os.path.exists(path):
        return None, None, None
    raw = open(path, encoding="utf-8").read().strip()
    if not raw:
        return None, None, None
    if raw.startswith("{"):
        try:
            data = json.loads(raw)
        except ValueError:
            return None, None, None
        return (data.get("access_token"),
                data.get("refresh_token"),
                data.get("date_expires"))
    lines = [l.strip() for l in raw.splitlines() if l.strip()]
    token = lines[0] if lines else None
    refresh = lines[1] if len(lines) > 1 else None
    return token, refresh, None


class ProxyHandler(BaseHTTPRequestHandler):
    server_version = "uc-online2-modio/1.0"
    protocol_version = "HTTP/1.1"

    # set by serve()
    upstream = DEFAULT_HOST
    logfile = None
    token_file = None
    rules = []
    token_cache = None
    allowed_hosts = list(DEFAULT_SANS)

    def log_message(self, fmt, *a):  # silence the default stderr spam
        pass

    def _upstream_for(self, path):
        """Forward to the host the client asked for, but only hosts we know."""
        host = (self.headers.get("Host") or "").split(":")[0].lower()
        if host in self.allowed_hosts:
            return host
        return self.upstream

    def _token(self):
        """The access token, read once and then kept in memory."""
        if self.token_cache is None:
            token, _, _ = load_token(self.token_file)
            self.token_cache = token
        return self.token_cache

    def _read_body(self):
        length = self.headers.get("Content-Length")
        if length:
            return self.rfile.read(int(length))
        if (self.headers.get("Transfer-Encoding") or "").lower() == "chunked":
            chunks = []
            while True:
                size = int(self.rfile.readline().split(b";")[0] or b"0", 16)
                if size == 0:
                    self.rfile.readline()
                    break
                chunks.append(self.rfile.read(size))
                self.rfile.readline()
            return b"".join(chunks)
        return b""

    def _forward(self, method):
        body = self._read_body()
        path = self.path
        headers = {}
        for key, value in self.headers.items():
            if key.lower() in HOP_BY_HOP:
                continue
            headers[key] = value
        upstream = self._upstream_for(path)
        headers["Host"] = upstream
        headers["Accept-Encoding"] = "identity"
        headers["Connection"] = "close"

        # Every API call the game makes is authenticated with our token, not
        # only the exchange.  That way it does not matter whether the game's
        # own login attempt succeeded.
        token = self._token()
        if token and not any(k.lower() == "authorization" for k in headers):
            if path.startswith("/v1/") and not any(ep in path for ep in AUTH_ENDPOINTS):
                headers["Authorization"] = "Bearer " + token
                log_line(self.logfile, "** added our Authorization header to %s %s" %
                         (method, path))

        conn = None
        try:
            conn = http.client.HTTPSConnection(upstream, 443, timeout=UPSTREAM_TIMEOUT)
            conn.request(method, path, body=body or None, headers=headers)
            resp = conn.getresponse()
            data = resp.read()
            status = resp.status
            resp_headers = [(k, v) for (k, v) in resp.getheaders()
                            if k.lower() not in HOP_BY_HOP]
            log_line(self.logfile, "<- %s %s -> HTTP %d (%d bytes)" %
                     (method, path, status, len(data)))
        except Exception as exc:  # noqa: BLE001 - report and answer 502
            log_line(self.logfile, "!! upstream %s failed: %s" % (upstream, exc))
            self.send_error(502, "upstream failed: %s" % exc)
            return
        finally:
            if conn:
                try:
                    conn.close()
                except Exception:
                    pass

        data, status = self._rewrite(path, status, data, resp_headers)

        self.send_response(status)
        for key, value in resp_headers:
            if key.lower() == "content-length":
                continue
            self.send_header(key, value)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if method != "HEAD":
            self.wfile.write(data)

    def _rewrite(self, path, status, data, resp_headers):
        """Replace the answer of the external-auth endpoints with our token."""
        token, refresh, expires = load_token(self.token_file)
        is_auth = any(ep in path for ep in AUTH_ENDPOINTS)
        if not is_auth:
            return data, status

        text = data.decode("utf-8", "replace")
        if token and '"access_token"' not in text:
            if expires is None:
                expires = int(time.time()) + 365 * 24 * 3600
            payload = {
                "code": 200,
                "access_token": token,
                "date_expires": int(expires),
            }
            if refresh:
                payload["refresh_token"] = refresh
            # the game also looks at "auth_code"
            payload["auth_code"] = token
            out = json.dumps(payload).encode("utf-8")
            log_line(self.logfile, "** rewrote %s: HTTP %d -> 200 (our own credentials)" %
                     (path, status))
            return out, 200

        if token:
            log_line(self.logfile, "** %s already returned an access_token -- left alone" % path)
        else:
            log_line(self.logfile, "** %s failed (HTTP %d): %s" % (path, status, text[:200]))
            log_line(self.logfile, "** no token configured -- put one in %s" % self.token_file)
        return data, status

    def do_GET(self):
        self._forward("GET")

    def do_POST(self):
        self._forward("POST")

    def do_PUT(self):
        self._forward("PUT")

    def do_HEAD(self):
        self._forward("HEAD")

    def do_DELETE(self):
        self._forward("DELETE")


def cmd_serve(args):
    directory = args.dir
    p = cert_paths(directory)
    if not os.path.exists(p["crt"]):
        print("no certificate in %s -- run 'init' first" % directory)
        return 1

    ProxyHandler.upstream = args.upstream
    ProxyHandler.logfile = args.log
    ProxyHandler.token_file = args.token_file

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(p["crt"], p["key"])

    class Server(ThreadingHTTPServer):
        allow_reuse_address = True
        address_family = socket.AF_INET

    httpd = Server(("127.0.0.1", args.port), ProxyHandler)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)

    token, _, _ = load_token(args.token_file)
    log_line(args.log, "listening on 127.0.0.1:%d, forwarding to https://%s" %
             (args.port, args.upstream))
    log_line(args.log, "credentials: %s" % ("loaded" if token else "MISSING -- the game will not get a session"))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


def cmd_status(args):
    p = cert_paths(args.dir)
    print("certificates : %s" % ("present" if os.path.exists(p["crt"]) else "missing"))
    for pfx in proton_prefixes():
        cert = os.path.join(pfx, "cert.pem")
        trusted = False
        if os.path.exists(cert):
            trusted = "uc-online2 modio proxy CA" in open(cert, encoding="utf-8").read()
        print("prefix %-70s CA trusted: %s" % (pfx, trusted))
    try:
        body = open("/etc/hosts", encoding="utf-8").read()
        print("hosts entry : %s" % ("present" if (DEFAULT_HOST in body and HOSTS_MARKER in body) else "missing"))
    except OSError:
        print("hosts entry : unknown")
    token, _, _ = load_token(args.token_file)
    print("credentials : %s" % ("loaded" if token else "missing"))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=DEFAULT_DIR, help="certificate directory")
    sub = ap.add_subparsers(dest="cmd")

    s = sub.add_parser("init", help="create the CA and the server certificate")
    s.add_argument("--san", nargs="+", default=DEFAULT_SANS, help="host names to cover")
    s.add_argument("--force", action="store_true")
    s.set_defaults(func=cmd_init)

    s = sub.add_parser("serve", help="run the proxy")
    s.add_argument("--port", type=int, default=443)
    s.add_argument("--upstream", default=DEFAULT_HOST)
    s.add_argument("--token-file", default=os.path.join(DEFAULT_DIR, "token.json"))
    s.add_argument("--log", default=os.path.join(DEFAULT_DIR, "proxy.log"))
    s.set_defaults(func=cmd_serve)

    s = sub.add_parser("cert", help="trust / untrust the CA")
    s.add_argument("action", choices=["install", "remove"])
    s.set_defaults(func=cmd_cert)

    s = sub.add_parser("hosts", help="redirect api.mod.io to 127.0.0.1")
    s.add_argument("action", choices=["add", "remove"])
    s.add_argument("--alias", nargs="*", default=[], help="extra host names to redirect")
    s.set_defaults(func=cmd_hosts)

    s = sub.add_parser("status", help="show what is set up")
    s.add_argument("--token-file", default=os.path.join(DEFAULT_DIR, "token.json"))
    s.set_defaults(func=cmd_status)

    args = ap.parse_args()
    if not getattr(args, "func", None):
        ap.print_help()
        return 1
    return args.func(args) or 0


if __name__ == "__main__":
    sys.exit(main())
