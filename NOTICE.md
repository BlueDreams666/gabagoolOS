# Credits and licences

GabagoolOS is free software under the GNU General Public License, version 2
or (at your option) any later version. The full text is in `docs/COPYING` in the source tree; [LICENSE](LICENSE) points to it.

The paths below refer to the source in this repository.

## What it is built on

- **Rockbox** (https://www.rockbox.org), GPL v2 or later. The playback engine,
  codecs, DSP, database, plugins, the Doom and Game Boy ports and most of this
  tree are the work of the Rockbox project and its contributors. Their names
  are in `docs/CREDITS`.
- **Rockbox for the HiBy R1** fork (WillNHT, https://github.com/WillNHT/rockbox-hiby-r1),
  built on **bahusoid's Rockbox Audiobook Mod** (https://github.com/bahusoid/rockbox).
  The R1 port, the audiobook library and many device fixes come from these.
  The fork's own README is kept at `docs/README-rockbox-hiby-r1-fork.md`.
- **LVGL** 9 (https://lvgl.io), MIT licence (`lib/lvgl/LICENCE.txt`). Draws
  the GabagoolOS interface. Its copy of **lodepng** (zlib licence) reads and
  writes PNGs.
- **stb_image** (`lib/stb`), public domain or MIT.
- **libmad** (MP3) and **FAAD2** (AAC), GPL v2 or later, copied into
  `apps/sloppy/mad` and `apps/sloppy/faad` for internet radio. Their own
  `COPYING` files are beside them.
- The Game Boy core in `apps/sloppy/gb` is Rockbox's **rockboy** (from gnuboy),
  GPL v2.
- For Ogg, Opus and FLAC internet radio, copies of Rockbox's decoders:
  **Tremor** (Xiph.org, BSD-style, `apps/sloppy/tremor/COPYING`), **libopus**
  (Xiph.org and Skype, BSD, in each file's header, `apps/sloppy/opus`) and the
  FLAC decoder from Rockbox's libffmpegFLAC (LGPL v2.1 or later,
  `apps/sloppy/flac/COPYING`). Each folder's `README.gabagool` lists what
  changed.

## Fonts (SIL Open Font License 1.1)

- **Source Sans 3**, Adobe.
- **Shrikhand**, Jonny Pinhorn.
- **Noto Sans Hebrew**, **Noto Sans Arabic** and **Noto Sans SC**
  (Simplified Chinese, plus the traditional forms and Japanese kanji in the
  letter packs), **Noto Sans KR** (Korean) and **Noto Sans Thai**, Google and
  the Noto Project Authors.
- **Montserrat** (LVGL's built-in symbol font), Julieta Ulanovsky.

Licence texts: `sloppy-dev/fonts-src/` and `apps/sloppy/fonts/`.

## Data

- **Unicode Unihan database** (Unicode, Inc., Unicode licence): the readings,
  frequencies and character sets behind the pinyin keyboard and the Chinese
  letter packs (`sloppy-dev/kb/gen-pinyin.py`). Unicode's compatibility
  decompositions give the Arabic unshaping table.
- **Keyboard word lists** (`sloppy-dev/kb/words-*.txt`): written for
  GabagoolOS, not copied from a corpus.
- **Wallpapers** (`apps/sloppy/wallpapers/README.md`): the Orion Nebula by
  Bryan Goff (Unsplash Licence); the Pillars of Creation by NASA, ESA, CSA and
  STScI; the Nexus background and pulse from the Android Open Source Project
  (Apache 2.0, `NEXUS-NOTICE`).
- **Trivia**: the first quiz groups adapt factual topics from QuizWitz's 2026
  movie and TV roundup; the wording was written for GabagoolOS.

## Online services (used only when you turn on Wi-Fi and use the feature)

- **LRCLIB** (https://lrclib.net): lyrics.
- **radio-browser** (https://www.radio-browser.info): the internet radio
  directory. **ip-api.com** answers "which country am I in" for Radio near you.
- **AutoEq** by Jaakko Pasanen (https://github.com/jaakkopasanen/AutoEq):
  headphone correction profiles, fetched from GitHub.
- **MusicBrainz** (https://musicbrainz.org) and the **Cover Art Archive**
  (https://coverartarchive.org): finding and fetching missing album covers.
- **ListenBrainz** (https://listenbrainz.org) and **Last.fm**
  (https://www.last.fm): scrobbling, with your own account.
- **Apple's podcast directory** (iTunes Search) for finding podcasts, and the
  feeds you add.
- Your own **Subsonic / Navidrome** and **Audiobookshelf** servers, when you
  sign in to one.
- **NTP** (pool.ntp.org, time.windows.com): the clock.

No analytics, no tracking, no accounts with us.

## Not included

- No game cartridges. Bring your own and put them in `Games/`.
- The DOOM shareware episode (`doom1.wad`, id Software) ships unmodified in
  the player zip under its shareware terms; it is not part of this source.
