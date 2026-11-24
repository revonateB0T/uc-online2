#!/usr/bin/env python3
"""
Get a mod.io access token for Space Engineers without a Steam ticket.

The game authenticates against mod.io with an encrypted Steam app ticket
(POST /v1/external/steamauth).  A ticket that was not minted for app 244850
is refused with HTTP 401 / error_ref 11018, so the in-game mod browser never
gets a session.

mod.io also has its own email flow, and it is exactly what the game itself
implements in `VRage.Mod.Io.dll` -- it is just not wired to a button in this
build:

    POST /v1/oauth/emailrequest   email=<your address>
    POST /v1/oauth/emailexchange  security_code=<the code that was mailed>

The second call answers with the access token object.  This script performs
those two calls with the game's own public api key and writes the result to
~/.uc-online2-modio/token.json, which is what tools/modio_proxy.py feeds to
the game.

Usage:
    python3 tools/modio_login.py request  you@example.com
    python3 tools/modio_login.py exchange 12345
    python3 tools/modio_login.py login    you@example.com      (both steps)

If you already signed in on the mod.io website, you do not need this script:
put the bearer token from your browser session into ~/.uc-online2-modio/token.json
as {"access_token": "..."} and go straight to tools/modio_proxy.py.
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

# Taken from the game's own executable:
#   MyModIoService.Create(..., "spaceengineers", "264",
#                         "1fb4489996a5e8ffc6ec1135f9985b5b", ...)
HOST = "https://g-264.modapi.io"
API_KEY = "1fb4489996a5e8ffc6ec1135f9985b5b"
TOKEN_DIR = os.path.expanduser("~/.uc-online2-modio")
TOKEN_FILE = os.path.join(TOKEN_DIR, "token.json")


def call(endpoint, fields):
    url = "%s/v1/%s?api_key=%s" % (HOST, endpoint, API_KEY)
    body = urllib.parse.urlencode(fields).encode("utf-8")
    req = urllib.request.Request(url, data=body, method="POST")
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    req.add_header("Accept", "application/json")
    req.add_header("X-Modio-Platform", "steam")
    req.add_header("X-Modio-Portal", "spaceengineers")
    # Cloudflare bans the default urllib agent (error 1010), and this is the
    # agent the game itself sends.
    req.add_header("User-Agent", "Space Engineers Client")
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", "replace")
        try:
            return exc.code, json.loads(raw)
        except ValueError:
            return exc.code, {"raw": raw}
    except urllib.error.URLError as exc:
        print("network error: %s" % exc)
        return 0, {}


def show(status, data):
    print("HTTP %d" % status)
    print(json.dumps(data, indent=2, ensure_ascii=False)[:1200])


def cmd_request(args):
    status, data = call("oauth/emailrequest", {"email": args.email})
    show(status, data)
    if status == 200:
        print("\nmod.io mailed a security code to %s -- now run:" % args.email)
        print("    python3 tools/modio_login.py exchange <code>")
    return 0 if status == 200 else 1


def cmd_exchange(args):
    fields = {"security_code": args.code}
    if args.display_name:
        fields["display_name_portal"] = args.display_name
    status, data = call("oauth/emailexchange", fields)
    show(status, data)
    token = data.get("access_token") if isinstance(data, dict) else None
    if token:
        os.makedirs(TOKEN_DIR, exist_ok=True)
        out = {"access_token": token}
        for key in ("refresh_token", "date_expires"):
            if key in data:
                out[key] = data[key]
        with open(TOKEN_FILE, "w", encoding="utf-8") as fh:
            json.dump(out, fh, indent=2)
        os.chmod(TOKEN_FILE, 0o600)
        print("\nwrote %s" % TOKEN_FILE)
        print("next: python3 tools/modio_proxy.py cert install && "
              "python3 tools/modio_proxy.py hosts add && "
              "python3 tools/modio_proxy.py serve")
    return 0 if token else 1


def cmd_login(args):
    rc = cmd_request(args)
    if rc:
        return rc
    code = input("security code from the mail: ").strip()
    if not code:
        print("no code given")
        return 1
    return cmd_exchange(argparse.Namespace(code=code, display_name=args.display_name))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")

    s = sub.add_parser("request", help="ask mod.io to mail a security code")
    s.add_argument("email")
    s.set_defaults(func=cmd_request)

    s = sub.add_parser("exchange", help="exchange the code for an access token")
    s.add_argument("code")
    s.add_argument("--display-name", default="", help="display name shown on mod.io")
    s.set_defaults(func=cmd_exchange)

    s = sub.add_parser("login", help="request the code and exchange it")
    s.add_argument("email")
    s.add_argument("--display-name", default="")
    s.set_defaults(func=cmd_login)

    args = ap.parse_args()
    if not getattr(args, "func", None):
        ap.print_help()
        return 1
    return args.func(args) or 0


if __name__ == "__main__":
    sys.exit(main())
