#!/usr/bin/env python3
"""Copy the song lists of your Spotify playlists onto a SloppyOS card.

The HiBy R1 has no TLS, so it cannot reach Spotify. This runs on a
computer instead: it signs in to *your own* Spotify app (Authorization Code
with PKCE, no client secret), reads the playlists you pick and writes one
file per playlist to <card>/.rockbox/sloppy/spotify/<id>.tsv. On the player,
Playlists > Spotify playlists matches them to the songs on the card.

Only metadata is copied (title, artists, album, length, ISRC). No audio,
and nothing that signs in ever goes on the card: the token stays in
~/.config/sloppyos-spotify/token.json (readable by you only).

  spotify_playlists.py login --client-id <id>     sign in (opens a browser)
  spotify_playlists.py list                       your playlists
  spotify_playlists.py export --card /Volumes/MP3 --all    or: <id> <id> ...
  spotify_playlists.py logout                     forget the sign-in

Spotify only returns the songs of playlists you own or collaborate on to an
app in Development Mode; others are listed but cannot be exported.
Standard library only. GPLv2+.
"""
import argparse
import base64
import hashlib
import http.server
import json
import os
import secrets
import stat
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

API = os.environ.get("SLOPPY_SPOTIFY_API", "https://api.spotify.com/v1")
ACCOUNTS = os.environ.get("SLOPPY_SPOTIFY_ACCOUNTS", "https://accounts.spotify.com")
CONFIG = os.environ.get("SLOPPY_SPOTIFY_CONFIG",
                        os.path.join(os.path.expanduser("~"), ".config", "sloppyos-spotify"))
TOKEN = os.path.join(CONFIG, "token.json")
SCOPES = "playlist-read-private playlist-read-collaborative"
TOP_SCOPE = "user-top-read"           # only with login --top-artists
LIKED_SCOPE = "user-library-read"     # only with login --liked
CARD_DIR = os.path.join(".rockbox", "sloppy", "spotify")


class Fail(Exception):
    pass


def save_token(tok):
    os.makedirs(CONFIG, mode=0o700, exist_ok=True)
    tmp = TOKEN + ".tmp"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(tok, f)
    os.chmod(tmp, stat.S_IRUSR | stat.S_IWUSR)
    os.replace(tmp, TOKEN)


def load_token():
    try:
        with open(TOKEN) as f:
            return json.load(f)
    except FileNotFoundError:
        raise Fail("Not signed in. Run: spotify_playlists.py login --client-id <your app's client ID>")


def post_form(url, fields):
    req = urllib.request.Request(url, data=urllib.parse.urlencode(fields).encode(),
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        raise Fail("Spotify refused the sign-in (%d): %s" % (e.code, e.read().decode("utf-8", "replace")[:200]))


def pkce_pair():
    verifier = secrets.token_urlsafe(64)[:96]
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    return verifier, challenge


def login(client_id, port, open_browser=True, wait=300, top=False, liked=False):
    verifier, challenge = pkce_pair()
    state = secrets.token_urlsafe(16)
    redirect = "http://127.0.0.1:%d/callback" % port
    got = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            if urllib.parse.urlparse(self.path).path != "/callback":
                self.send_response(404); self.end_headers(); return
            got.update({k: v[0] for k, v in q.items()})
            ok = "code" in got and got.get("state") == state
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.end_headers()
            self.wfile.write(("Signed in. You can close this tab." if ok else
                              "Sign-in did not finish. Go back to the terminal.").encode())

        def log_message(self, *a):
            pass

    server = http.server.HTTPServer(("127.0.0.1", port), Handler)
    url = ACCOUNTS + "/authorize?" + urllib.parse.urlencode({
        "client_id": client_id, "response_type": "code", "redirect_uri": redirect,
        "code_challenge_method": "S256", "code_challenge": challenge,
        "state": state, "scope": SCOPES + (" " + TOP_SCOPE if top else "") + (" " + LIKED_SCOPE if liked else "")})
    print("Opening Spotify to sign in. If no browser opens, visit:\n" + url)
    if open_browser:
        webbrowser.open(url)
    server.timeout = 1
    deadline = time.time() + wait
    while "code" not in got and "error" not in got and time.time() < deadline:
        server.handle_request()
    server.server_close()
    if got.get("state") != state:
        raise Fail("Sign-in did not come back (or came back from somewhere else).")
    if "error" in got:
        raise Fail("Spotify said: " + got["error"])
    tok = post_form(ACCOUNTS + "/api/token", {
        "grant_type": "authorization_code", "code": got["code"], "redirect_uri": redirect,
        "client_id": client_id, "code_verifier": verifier})
    save_token({"client_id": client_id, "refresh_token": tok["refresh_token"],
                "access_token": tok["access_token"], "expires_at": time.time() + tok.get("expires_in", 3600) - 60})
    print("Signed in. The token is in " + TOKEN)


def access_token():
    tok = load_token()
    if time.time() < tok.get("expires_at", 0):
        return tok["access_token"]
    new = post_form(ACCOUNTS + "/api/token", {
        "grant_type": "refresh_token", "refresh_token": tok["refresh_token"], "client_id": tok["client_id"]})
    tok["access_token"] = new["access_token"]
    tok["expires_at"] = time.time() + new.get("expires_in", 3600) - 60
    if new.get("refresh_token"):
        tok["refresh_token"] = new["refresh_token"]
    save_token(tok)
    return tok["access_token"]


def get(path_or_url, tries=4):
    url = path_or_url if path_or_url.startswith("http") else API + path_or_url
    for _ in range(tries):
        req = urllib.request.Request(url, headers={"Authorization": "Bearer " + access_token()})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            if e.code == 429:
                time.sleep(min(int(e.headers.get("Retry-After", "2") or 2), 30))
                continue
            if e.code == 401:
                tok = load_token(); tok["expires_at"] = 0; save_token(tok)
                continue
            raise
    raise Fail("Spotify kept saying slow down; try again later.")


def me():
    return get("/me")["id"]


def playlists():
    out, url = [], "/me/playlists?limit=50"
    while url:
        page = get(url)
        out += [p for p in page.get("items", []) if p]
        url = page.get("next")
    return out


def readable(p, user):
    return (p.get("owner") or {}).get("id") == user or bool(p.get("collaborative"))


def clean(s):
    return " ".join(str(s or "").replace("\t", " ").replace("\r", " ").replace("\n", " ").split())


def playlist_songs(pid):
    """The playlist's songs in order. /items is the endpoint for apps in
    Development Mode since 2026; /tracks for older apps."""
    try:
        page = get("/playlists/%s/items?limit=50" % urllib.parse.quote(pid))
    except urllib.error.HTTPError as e:
        if e.code not in (403, 404):
            raise
        page = get("/playlists/%s/tracks?limit=50" % urllib.parse.quote(pid))
    rows = []
    while True:
        for it in page.get("items", []):
            t = (it or {}).get("item") or (it or {}).get("track")
            if not t or t.get("type", "track") != "track" or not t.get("name"):
                continue                        # episodes, removed songs
            rows.append(song_row(t))
        if not page.get("next"):
            return rows
        page = get(page["next"])


def song_row(t):
    return (clean(t["name"]),
            ";".join(clean(a.get("name")) for a in t.get("artists", []) if a.get("name")),
            clean((t.get("album") or {}).get("name")),
            str(int(t.get("duration_ms") or 0)),
            clean((t.get("external_ids") or {}).get("isrc")))


def liked_songs():
    """Your Liked Songs, newest first: For you's Loved on the player."""
    page = get("/me/tracks?limit=50")
    rows = []
    while True:
        for it in page.get("items", []):
            t = (it or {}).get("track")
            if t and t.get("name"):
                rows.append(song_row(t))
        if not page.get("next"):
            return rows
        page = get(page["next"])


def top_artists():
    """Your top artists' names, most listened first (Spotify's own ranking,
    about the last six months). Names only: they help Radio on the player
    lean towards artists you like that are on the card."""
    page = get("/me/top/artists?limit=50&time_range=medium_term")
    return [clean(a.get("name")) for a in page.get("items", []) if a and a.get("name")]


def write_top(card, names):
    folder = os.path.join(card, CARD_DIR)
    if not os.path.isdir(os.path.join(card, ".rockbox")):
        raise Fail("%s does not look like a SloppyOS card (no .rockbox folder)." % card)
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, "top-artists.txt")
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("#SLOPPY-SPOTIFY-TOP\t1\n#exported\t%s\n" % time.strftime("%Y-%m-%d"))
        for n in names:
            f.write(n + "\n")
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    return path


def write_card(card, pid, name, rows):
    folder = os.path.join(card, CARD_DIR)
    if not os.path.isdir(os.path.join(card, ".rockbox")):
        raise Fail("%s does not look like a SloppyOS card (no .rockbox folder)." % card)
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, "%s.tsv" % "".join(c for c in pid if c.isalnum()))
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("#SLOPPY-SPOTIFY\t1\n#id\t%s\n#name\t%s\n#exported\t%s\n" % (pid, clean(name), time.strftime("%Y-%m-%d")))
        for r in rows:
            f.write("\t".join(r) + "\n")
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    return path


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    lg = sub.add_parser("login"); lg.add_argument("--client-id", required=True); lg.add_argument("--port", type=int, default=8888)
    lg.add_argument("--top-artists", action="store_true", help="also allow reading your top artists, for Radio")
    lg.add_argument("--liked", action="store_true", help="also allow reading your Liked Songs, for For you")
    sub.add_parser("list")
    ex = sub.add_parser("export"); ex.add_argument("--card", required=True); ex.add_argument("--all", action="store_true")
    ex.add_argument("ids", nargs="*")
    ex.add_argument("--top-artists", action="store_true", help="also copy your top artists' names, for Radio")
    ex.add_argument("--liked", action="store_true", help="also copy your Liked Songs, for For you's Loved")
    sub.add_parser("logout")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "login":
            login(a.client_id, a.port, top=a.top_artists, liked=a.liked)
            load = load_token(); load["top"] = bool(a.top_artists); load["liked"] = bool(a.liked); save_token(load)
        elif a.cmd == "logout":
            if os.path.exists(TOKEN):
                os.remove(TOKEN)
            print("Signed out here. To take back the app's access too, remove it at "
                  "https://www.spotify.com/account/apps/")
        elif a.cmd == "list":
            user = me()
            for p in playlists():
                mark = "" if readable(p, user) else "   (followed: Spotify does not share its songs)"
                print("%s  %s%s" % (p["id"], p.get("name", ""), mark))
        elif a.cmd == "export":
            user = me()
            mine = {p["id"]: p for p in playlists()}
            ids = [i for i, p in mine.items() if readable(p, user)] if a.all else a.ids
            if not ids and not a.top_artists and not a.liked:
                raise Fail("Name playlist IDs (see list), use --all, --top-artists or --liked.")
            if a.liked:
                if not load_token().get("liked"):
                    raise Fail("Sign in again with: login --client-id <id> --liked")
                rows = liked_songs()
                print("Liked Songs: %d songs -> %s" % (len(rows), write_card(a.card, "liked", "Liked Songs", rows)))
            if a.top_artists:
                if not load_token().get("top"):
                    raise Fail("Sign in again with: login --client-id <id> --top-artists")
                names = top_artists()
                print("top artists: %d -> %s" % (len(names), write_top(a.card, names)))
            for pid in ids:
                p = mine.get(pid)
                if p and not readable(p, user):
                    print("skipped %s: followed, not yours" % p.get("name", pid))
                    continue
                rows = playlist_songs(pid)
                path = write_card(a.card, pid, (p or {}).get("name", pid), rows)
                print("%s: %d songs -> %s" % ((p or {}).get("name", pid), len(rows), path))
            print("On the player: Playlists > Spotify playlists.")
    except Fail as e:
        print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
