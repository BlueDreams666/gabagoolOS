# Spotify playlists for SloppyOS

The R1 can't reach Spotify (it has no TLS), so a computer copies your
playlists' song lists onto the card and the player matches them to the songs
it has. Only metadata moves; no audio comes from Spotify.

## Once: your own Spotify app

Spotify gives apps like this one only Development Mode: up to five users you
add yourself, and the app's owner needs Spotify Premium. No client secret is
used (PKCE), so nothing secret ships with SloppyOS.

1. At <https://developer.spotify.com/dashboard>, create an app (Web API).
2. Redirect URI: `http://127.0.0.1:8888/callback` (Spotify allows plain http
   only for 127.0.0.1, never `localhost`).
3. Under User Management, add the Spotify account you will sign in with.
4. Copy the Client ID.

## Each time

```sh
python3 tools/sloppy-spotify/spotify_playlists.py login --client-id <Client ID>
python3 tools/sloppy-spotify/spotify_playlists.py list
python3 tools/sloppy-spotify/spotify_playlists.py export --card /Volumes/MP3 --all
```

Then on the player: Playlists > Spotify playlists > a playlist > Save as
playlist. Exporting again and tapping Update playlist refreshes it in place.

* Optional, for Radio: `login --client-id <id> --top-artists` once, then
  `export --card /Volumes/MP3 --top-artists`. It copies the names of your
  Spotify top artists (Spotify's own ranking, about six months) to
  `.rockbox/sloppy/spotify/top-artists.txt`; Radio leans towards albums by
  them that are on the card. Only the names are kept, nothing is sent
  anywhere, and "Remove these lists" deletes them. Spotify's developer
  policy is strict about using its data (no analysing, no user profiles);
  this is the personalisation the endpoint exists for, but it is your call,
  which is why it is off unless you ask for it.
* Optional, for For you: `login --client-id <id> --liked` once, then
  `export --card /Volumes/MP3 --liked`. Your Liked Songs become
  `liked.tsv`: a Spotify playlist on the player like any other, and the
  first part of For you > Loved.
* Spotify returns the songs only for playlists you made or collaborate on.
  Followed and Spotify-made playlists show in `list` but can't be exported.
* The sign-in lives in `~/.config/sloppyos-spotify/token.json` (yours only).
  `logout` deletes it; remove the app's access at
  <https://www.spotify.com/account/apps/>.
* A playlist saved on the player starts with a `#SLOPPY-SPOTIFY:<id>` line.
  Updating replaces that file, even renamed, so songs added to it by hand go.
  A playlist of the same name you made yourself is never touched: the import
  gets "(Spotify)" on its name.
* Matching is by title, artist, album and length. Versions (live, acoustic,
  remix, edit...) must match, remaster notes don't matter, and a song more
  than 7 s longer or shorter is another recording. What isn't found is listed
  under "Not on this player".

Tests: `python3 -I tools/sloppy-spotify/test_spotify_playlists.py` (a pretend
Spotify) and `apps/sloppy/tests/spotify_test.c` (the matcher).
