# Keyboard dictionaries

`words-<id>.txt`: the words a glide or a suggestion can become, most common
first (whitespace separated). Written for GabagoolOS, not copied from a
corpus, so they carry no licence beyond the project's. The rank is the
only frequency: a word's weight is Zipf's, from its place in the list.

`gen-kb-dict.py` turns them into `apps/sloppy/sloppy_kb_dict.h`. Run it
after editing a list:

    python3 sloppy-dev/kb/gen-kb-dict.py

Pinyin (`pinyin-*.txt`) is described in `gen-pinyin.py`.
