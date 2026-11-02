/* Romaji to kana for the Japanese keyboard. GPLv2+. Included by sloppy_kb.inc.
 *
 * Letters typed in romaji (Hepburn, and the kunrei and word-processor
 * spellings a Japanese keyboard takes too: si, tu, zi, hu ...) become
 * hiragana as they are typed; the bar offers them as hiragana, as words in
 * kanji when the reading is one of ja_words, and as katakana. A doubled
 * consonant is a small tsu (kitte), "nn" or n before a consonant is ん
 * (konnichiha, sanpo, n' before a vowel), x or l before a vowel the small
 * kana (xya), and "-" the long mark. Every character here is in the card's
 * cjk packs (sloppy_xfont.inc). */

/* romaji, kana: sorted by romaji (ja_find bisects it) */
static const char *const ja_roma[][2] = {
    { "a", "あ" }, { "ba", "ば" }, { "be", "べ" }, { "bi", "び" }, { "bo", "ぼ" }, { "bu", "ぶ" },
    { "bya", "びゃ" }, { "bye", "びぇ" }, { "byo", "びょ" }, { "byu", "びゅ" },
    { "ca", "か" }, { "ce", "せ" }, { "cha", "ちゃ" }, { "che", "ちぇ" }, { "chi", "ち" }, { "cho", "ちょ" }, { "chu", "ちゅ" },
    { "ci", "し" }, { "co", "こ" }, { "cu", "く" }, { "cya", "ちゃ" }, { "cye", "ちぇ" }, { "cyi", "ちぃ" }, { "cyo", "ちょ" }, { "cyu", "ちゅ" },
    { "da", "だ" }, { "de", "で" }, { "dha", "でゃ" }, { "dhe", "でぇ" }, { "dhi", "でぃ" }, { "dho", "でょ" }, { "dhu", "でゅ" },
    { "di", "ぢ" }, { "do", "ど" }, { "du", "づ" }, { "dwu", "どぅ" }, { "dya", "ぢゃ" }, { "dye", "ぢぇ" }, { "dyi", "ぢぃ" }, { "dyo", "ぢょ" }, { "dyu", "ぢゅ" },
    { "e", "え" }, { "fa", "ふぁ" }, { "fe", "ふぇ" }, { "fi", "ふぃ" }, { "fo", "ふぉ" }, { "fu", "ふ" }, { "fyu", "ふゅ" },
    { "ga", "が" }, { "ge", "げ" }, { "gi", "ぎ" }, { "go", "ご" }, { "gu", "ぐ" }, { "gwa", "ぐぁ" },
    { "gya", "ぎゃ" }, { "gye", "ぎぇ" }, { "gyo", "ぎょ" }, { "gyu", "ぎゅ" },
    { "ha", "は" }, { "he", "へ" }, { "hi", "ひ" }, { "ho", "ほ" }, { "hu", "ふ" },
    { "hya", "ひゃ" }, { "hye", "ひぇ" }, { "hyo", "ひょ" }, { "hyu", "ひゅ" },
    { "i", "い" }, { "ja", "じゃ" }, { "je", "じぇ" }, { "ji", "じ" }, { "jo", "じょ" }, { "ju", "じゅ" },
    { "jya", "じゃ" }, { "jye", "じぇ" }, { "jyi", "じぃ" }, { "jyo", "じょ" }, { "jyu", "じゅ" },
    { "ka", "か" }, { "ke", "け" }, { "ki", "き" }, { "ko", "こ" }, { "ku", "く" }, { "kwa", "くぁ" },
    { "kya", "きゃ" }, { "kye", "きぇ" }, { "kyo", "きょ" }, { "kyu", "きゅ" },
    { "la", "ぁ" }, { "le", "ぇ" }, { "li", "ぃ" }, { "lo", "ぉ" }, { "ltsu", "っ" }, { "ltu", "っ" }, { "lu", "ぅ" },
    { "lwa", "ゎ" }, { "lya", "ゃ" }, { "lyo", "ょ" }, { "lyu", "ゅ" },
    { "ma", "ま" }, { "me", "め" }, { "mi", "み" }, { "mo", "も" }, { "mu", "む" },
    { "mya", "みゃ" }, { "mye", "みぇ" }, { "myo", "みょ" }, { "myu", "みゅ" },
    { "na", "な" }, { "ne", "ね" }, { "ni", "に" }, { "no", "の" }, { "nu", "ぬ" },
    { "nya", "にゃ" }, { "nye", "にぇ" }, { "nyo", "にょ" }, { "nyu", "にゅ" },
    { "o", "お" }, { "pa", "ぱ" }, { "pe", "ぺ" }, { "pi", "ぴ" }, { "po", "ぽ" }, { "pu", "ぷ" },
    { "pya", "ぴゃ" }, { "pye", "ぴぇ" }, { "pyo", "ぴょ" }, { "pyu", "ぴゅ" },
    { "qa", "くぁ" }, { "qe", "くぇ" }, { "qi", "くぃ" }, { "qo", "くぉ" }, { "qu", "く" },
    { "ra", "ら" }, { "re", "れ" }, { "ri", "り" }, { "ro", "ろ" }, { "ru", "る" },
    { "rya", "りゃ" }, { "rye", "りぇ" }, { "ryo", "りょ" }, { "ryu", "りゅ" },
    { "sa", "さ" }, { "se", "せ" }, { "sha", "しゃ" }, { "she", "しぇ" }, { "shi", "し" }, { "sho", "しょ" }, { "shu", "しゅ" },
    { "si", "し" }, { "so", "そ" }, { "su", "す" }, { "sya", "しゃ" }, { "sye", "しぇ" }, { "syo", "しょ" }, { "syu", "しゅ" },
    { "ta", "た" }, { "te", "て" }, { "tha", "てゃ" }, { "the", "てぇ" }, { "thi", "てぃ" }, { "tho", "てょ" }, { "thu", "てゅ" },
    { "ti", "ち" }, { "to", "と" }, { "tsa", "つぁ" }, { "tse", "つぇ" }, { "tsi", "つぃ" }, { "tso", "つぉ" }, { "tsu", "つ" },
    { "tu", "つ" }, { "twu", "とぅ" }, { "tya", "ちゃ" }, { "tye", "ちぇ" }, { "tyi", "ちぃ" }, { "tyo", "ちょ" }, { "tyu", "ちゅ" },
    { "u", "う" }, { "va", "ゔぁ" }, { "ve", "ゔぇ" }, { "vi", "ゔぃ" }, { "vo", "ゔぉ" }, { "vu", "ゔ" },
    { "wa", "わ" }, { "we", "うぇ" }, { "wi", "うぃ" }, { "wo", "を" }, { "wu", "う" },
    { "xa", "ぁ" }, { "xe", "ぇ" }, { "xi", "ぃ" }, { "xo", "ぉ" }, { "xtsu", "っ" }, { "xtu", "っ" }, { "xu", "ぅ" },
    { "xwa", "ゎ" }, { "xya", "ゃ" }, { "xyo", "ょ" }, { "xyu", "ゅ" },
    { "ya", "や" }, { "ye", "いぇ" }, { "yi", "い" }, { "yo", "よ" }, { "yu", "ゆ" },
    { "za", "ざ" }, { "ze", "ぜ" }, { "zi", "じ" }, { "zo", "ぞ" }, { "zu", "ず" },
    { "zya", "じゃ" }, { "zye", "じぇ" }, { "zyi", "じぃ" }, { "zyo", "じょ" }, { "zyu", "じゅ" },
};

/* Words a song, an artist, a playlist or a pet is often named with, by
 * their reading in hiragana: the bar offers the kanji when the letters
 * typed read as one; a reading may have several. */
static const char *const ja_words[][2] = {
    { "あい", "愛" }, { "あお", "青" }, { "あか", "赤" }, { "あき", "秋" }, { "あくま", "悪魔" }, { "あさ", "朝" },
    { "あした", "明日" }, { "あに", "兄" }, { "あね", "姉" }, { "あめ", "雨" }, { "あるく", "歩く" }, { "いえ", "家" },
    { "いち", "一" }, { "いぬ", "犬" }, { "いのち", "命" }, { "いま", "今" }, { "いもうと", "妹" }, { "うた", "歌" },
    { "うたう", "歌う" }, { "うみ", "海" }, { "えいが", "映画" }, { "えいご", "英語" }, { "えがお", "笑顔" }, { "えき", "駅" },
    { "おおさか", "大阪" }, { "おきなわ", "沖縄" }, { "おと", "音" }, { "おとうと", "弟" }, { "おとこ", "男" }, { "おとな", "大人" },
    { "おもい", "想い" }, { "おもいで", "思い出" }, { "おんがく", "音楽" }, { "おんな", "女" }, { "かげ", "影" }, { "かこ", "過去" },
    { "かしゅ", "歌手" }, { "かぜ", "風" }, { "かぞく", "家族" }, { "がっこう", "学校" }, { "かみ", "神" }, { "かわ", "川" },
    { "かんこく", "韓国" }, { "きく", "聞く" }, { "きせき", "奇跡" }, { "きせつ", "季節" }, { "きのう", "昨日" }, { "きみ", "君" },
    { "きょう", "今日" }, { "きょうと", "京都" }, { "きょく", "曲" }, { "くも", "雲" }, { "くる", "来る" }, { "くるま", "車" },
    { "くろ", "黒" }, { "げんき", "元気" }, { "こい", "恋" }, { "こえ", "声" }, { "こおり", "氷" }, { "こころ", "心" },
    { "こども", "子供" }, { "さかな", "魚" }, { "さくら", "桜" }, { "さん", "三" }, { "しあわせ", "幸せ" }, { "しごと", "仕事" },
    { "しゃしん", "写真" }, { "じかん", "時間" }, { "じゆう", "自由" }, { "しょうじょ", "少女" }, { "しょうねん", "少年" }, { "しろ", "白" },
    { "すき", "好き" }, { "せいしゅん", "青春" }, { "せかい", "世界" }, { "せんせい", "先生" }, { "そら", "空" }, { "だいすき", "大好き" },
    { "たび", "旅" }, { "たべる", "食べる" }, { "ちち", "父" }, { "ちゅうごく", "中国" }, { "つき", "月" }, { "つばさ", "翼" },
    { "てんし", "天使" }, { "でんしゃ", "電車" }, { "でんわ", "電話" }, { "とうきょう", "東京" }, { "とき", "時" }, { "ともだち", "友達" },
    { "とり", "鳥" }, { "なつ", "夏" }, { "なまえ", "名前" }, { "なみだ", "涙" }, { "に", "二" }, { "にほん", "日本" },
    { "にほんご", "日本語" }, { "ねこ", "猫" }, { "のむ", "飲む" }, { "はしる", "走る" }, { "はな", "花" }, { "はなび", "花火" },
    { "はは", "母" }, { "はる", "春" }, { "ばんぐみ", "番組" }, { "ひ", "日" }, { "ひ", "火" }, { "ひかり", "光" },
    { "ひと", "人" }, { "ひとり", "一人" }, { "ひる", "昼" }, { "ふたり", "二人" }, { "ふゆ", "冬" }, { "へいわ", "平和" },
    { "ほし", "星" }, { "ほっかいどう", "北海道" }, { "ほのお", "炎" }, { "ほん", "本" }, { "まち", "町" }, { "まつり", "祭り" },
    { "まほう", "魔法" }, { "みず", "水" }, { "みち", "道" }, { "みどり", "緑" }, { "みらい", "未来" }, { "みる", "見る" },
    { "やま", "山" }, { "ゆき", "雪" }, { "ゆめ", "夢" }, { "よる", "夜" }, { "わたし", "私" },
};

static int ja_cmp_n(const char *a, int n, const char *b)
{
    int c = strncmp(a, b, n);
    return c ? c : b[n] ? -1 : 0;
}
/* the kana for exactly these n letters, or NULL; *prefix: some romaji starts with them */
static const char *ja_find(const char *s, int n, bool *prefix)
{
    int lo = 0, hi = (int)ARRAYLEN(ja_roma) - 1;
    if (prefix) *prefix = false;
    while (lo <= hi)
    {
        int m = (lo + hi) / 2, c = ja_cmp_n(s, n, ja_roma[m][0]);
        if (!c) { if (prefix) *prefix = true; return ja_roma[m][1]; }
        if (c < 0) hi = m - 1; else lo = m + 1;
    }
    if (prefix && lo < (int)ARRAYLEN(ja_roma) && !strncmp(s, ja_roma[lo][0], n)) *prefix = true;
    return NULL;
}
static bool ja_vowel(char c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }
static void ja_put(char **o, char *end, const char *s, int n)
{
    if (*o + n >= end) return;
    memcpy(*o, s, n);
    *o += n;
}
/* romaji to hiragana. Not final (still typing): letters that may yet
 * become kana stay letters at the end ("kak" is かk, "n" is n). Final:
 * a last n is ん and anything else left stays as typed. */
static void ja_kana(const char *s, char *out, size_t size, bool final)
{
    char *o = out, *end = out + size - 1;
    int len = (int)strlen(s);
    for (int i = 0; i < len;)
    {
        char c = s[i], d = i + 1 < len ? s[i + 1] : 0;
        if (c == '-') { ja_put(&o, end, "ー", 3); i++; continue; }
        if (c == 'n' && (d == '\'' || (d == 'n' && !(i + 2 < len && (ja_vowel(s[i + 2]) || s[i + 2] == 'y')))))
        {   /* n', or nn not before a vowel (konnichiha keeps its second n for に) */
            ja_put(&o, end, "ん", 3); i += 2; continue;
        }
        if (c == 'n' && d && !ja_vowel(d) && d != 'y') { ja_put(&o, end, "ん", 3); i++; continue; }
        if (c == 'n' && !d) { if (final) ja_put(&o, end, "ん", 3); else ja_put(&o, end, "n", 1); i++; continue; }
        if (c == d && c >= 'a' && c <= 'z' && !ja_vowel(c) && c != 'n') { ja_put(&o, end, "っ", 3); i++; continue; }
        if (c == 't' && d == 'c' && i + 2 < len && s[i + 2] == 'h') { ja_put(&o, end, "っ", 3); i++; continue; }   /* matcha */
        const char *k = NULL;
        int n = len - i < 4 ? len - i : 4;
        bool more = false;
        for (; n > 0; n--)
        {
            bool pre;
            k = ja_find(s + i, n, &pre);
            if (n == len - i && pre && !k) more = true;   /* the start of a longer one: wait */
            if (k) break;
        }
        if (k && !(more && !final))
        {
            ja_put(&o, end, k, (int)strlen(k));
            i += n;
            continue;
        }
        if (!final && i + 4 > len)
        {   /* the last letters may still make kana */
            bool pre;
            ja_find(s + i, len - i, &pre);
            if (pre) { ja_put(&o, end, s + i, len - i); break; }
        }
        ja_put(&o, end, s + i, 1);
        i++;
    }
    *o = '\0';
}
/* hiragana to katakana: U+3041 to U+3096 are 0x60 below theirs */
static void ja_katakana(const char *s, char *out, size_t size)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 4 < size;)
    {
        if (p[0] == 0xe3 && (p[1] == 0x81 || p[1] == 0x82))
        {
            uint32_t c = ((uint32_t)(p[0] & 0x0f) << 12) | ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
            if (c >= 0x3041 && c <= 0x3096) c += 0x60;
            out[o++] = (char)(0xe0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (c & 0x3f));
            p += 3;
        }
        else out[o++] = (char)*p++;
    }
    out[o] = '\0';
}
