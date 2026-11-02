"""spotify_playlists.py against a pretend Spotify: python3 -I test_spotify_playlists.py"""
import http.server, json, os, sys, tempfile, threading, urllib.parse, urllib.request

here = os.path.dirname(os.path.abspath(__file__))
work = tempfile.mkdtemp()
seen = {"token_posts": []}


class Fake(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def reply(self, code, body):
        self.send_response(code); self.send_header("Content-Type", "application/json"); self.end_headers()
        self.wfile.write(json.dumps(body).encode())

    def do_POST(self):
        form = urllib.parse.parse_qs(self.rfile.read(int(self.headers["Content-Length"])).decode())
        seen["token_posts"].append({k: v[0] for k, v in form.items()})
        assert "client_secret" not in form            # PKCE: no secret, ever
        self.reply(200, {"access_token": "acc", "refresh_token": "ref2", "expires_in": 3600})

    def do_GET(self):
        assert self.headers["Authorization"] == "Bearer acc"
        p = urllib.parse.urlparse(self.path)
        base = "http://127.0.0.1:%d/v1" % port
        song = lambda n, ms: {"type": "track", "name": n, "artists": [{"name": "Mira Sol"}, {"name": "Kite"}],
                              "album": {"name": "Harbor\tLights"}, "duration_ms": ms, "external_ids": {"isrc": "USX1"}}
        if p.path == "/v1/me": return self.reply(200, {"id": "joe"})
        if p.path == "/v1/me/top/artists":
            return self.reply(200, {"items": [{"name": "Mira Sol"}, {"name": "Kite\tBand"}, {}]})
        if p.path == "/v1/me/playlists":
            if "offset=50" in p.query:
                return self.reply(200, {"items": [{"id": "B", "name": "Theirs", "owner": {"id": "dj"}}], "next": None})
            return self.reply(200, {"items": [{"id": "A", "name": "Road Trip", "owner": {"id": "joe"}},
                                              {"id": "C", "name": "Shared", "owner": {"id": "x"}, "collaborative": True}],
                                    "next": base + "/me/playlists?limit=50&offset=50"})
        if p.path == "/v1/playlists/A/items": return self.reply(404, {"error": "old app"})
        if p.path == "/v1/playlists/A/tracks":
            if "offset=50" in p.query:
                return self.reply(200, {"items": [{"track": song("Two\nLines", 2000)}], "next": None})
            return self.reply(200, {"items": [{"track": song("Night Tide", 241000)},
                                              {"track": {"type": "episode", "name": "A podcast"}}, {"track": None}],
                                    "next": base + "/playlists/A/tracks?limit=50&offset=50"})
        if p.path == "/v1/playlists/C/items":
            return self.reply(200, {"items": [{"item": song("Silent City", 300000)}], "next": None})
        self.reply(404, {})


srv = http.server.HTTPServer(("127.0.0.1", 0), Fake)
port = srv.server_address[1]
threading.Thread(target=srv.serve_forever, daemon=True).start()
os.environ.update(SLOPPY_SPOTIFY_API="http://127.0.0.1:%d/v1" % port,
                  SLOPPY_SPOTIFY_ACCOUNTS="http://127.0.0.1:%d" % port,
                  SLOPPY_SPOTIFY_CONFIG=os.path.join(work, "cfg"))
sys.path.insert(0, here)
import spotify_playlists as sp

# sign-in: the browser comes back to the loopback page with the code and state
def browser(url):
    q = urllib.parse.parse_qs(urllib.parse.urlparse(url).query)
    assert q["code_challenge_method"] == ["S256"] and "client_secret" not in q
    assert q["redirect_uri"] == ["http://127.0.0.1:8899/callback"]
    seen["scope"] = q["scope"][0]
    cb = q["redirect_uri"][0] + "?" + urllib.parse.urlencode({"code": "c0de", "state": q["state"][0]})
    threading.Timer(0.3, lambda: urllib.request.urlopen(cb).read()).start()
    return True
sp.webbrowser.open = browser
sp.login("myapp", 8899)
post = seen["token_posts"][-1]
assert post["grant_type"] == "authorization_code" and post["code"] == "c0de" and len(post["code_verifier"]) >= 43
assert oct(os.stat(sp.TOKEN).st_mode & 0o777) == "0o600"

# an expired token refreshes with the client ID only
tok = sp.load_token(); tok["expires_at"] = 0; sp.save_token(tok)
card = os.path.join(work, "card"); os.makedirs(os.path.join(card, ".rockbox"))
assert sp.main(["export", "--card", card, "--all"]) == 0
assert seen["token_posts"][-1]["grant_type"] == "refresh_token" and sp.load_token()["refresh_token"] == "ref2"
a = open(os.path.join(card, ".rockbox/sloppy/spotify/A.tsv"), encoding="utf-8").read().splitlines()
assert a[:3] == ["#SLOPPY-SPOTIFY\t1", "#id\tA", "#name\tRoad Trip"]
assert a[4:] == ["Night Tide\tMira Sol;Kite\tHarbor Lights\t241000\tUSX1",
                 "Two Lines\tMira Sol;Kite\tHarbor Lights\t2000\tUSX1"]     # order kept, no episode, no tabs
c = open(os.path.join(card, ".rockbox/sloppy/spotify/C.tsv"), encoding="utf-8").read()
assert "Silent City" in c                                                    # a collaboration counts
assert not os.path.exists(os.path.join(card, ".rockbox/sloppy/spotify/B.tsv"))   # followed: not readable
assert sp.main(["export", "--card", work, "--all"]) == 1                    # not a card
assert "user-top-read" not in seen["scope"]                                  # only when asked
assert sp.main(["export", "--card", card, "--top-artists"]) == 1            # not allowed at sign-in
sp.main(["login", "--client-id", "myapp", "--port", "8899", "--top-artists"])
assert "user-top-read" in seen["scope"]
assert sp.main(["export", "--card", card, "--top-artists"]) == 0
top = open(os.path.join(card, ".rockbox/sloppy/spotify/top-artists.txt"), encoding="utf-8").read().splitlines()
assert top[0] == "#SLOPPY-SPOTIFY-TOP\t1" and top[2:] == ["Mira Sol", "Kite Band"]
assert sp.main(["logout"]) == 0 and not os.path.exists(sp.TOKEN)
assert sp.main(["list"]) == 1                                                # signed out
print("PASS: PKCE sign-in, refresh, export order, followed playlists skipped, top artists opt-in, sign-out.")
