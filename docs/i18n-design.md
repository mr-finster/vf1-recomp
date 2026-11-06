# The menus in other languages (i18n)

Virtua Fighter Recomp's menus, notices, online screens and its own system boxes (choosing the ROM the first time, a
start that failed, the recompiler's warning) are in 13 languages: English, Korean, Japanese, Simplified
and Traditional Chinese, Spanish, Brazilian Portuguese, French, Russian, German, Italian, Polish and Turkish. A
pseudo-locale is there for testing. The system is Virtua Fighter 3 Recomp's, the same files in VF1, VF2 and VF3; each
game has its own tables. This page covers:

- how it works;
- how to add a string or a language;
- the fonts;
- the tests;
- VF1's own strings, and what still needs a native speaker.

What is not translated: the game's own text (the ROM's: its title screen, HUD, the rules between demos; Arcade
settings > Language chooses its Japanese or English), players' and rooms' names, a random fighter's seed (its name),
the secret characters' names, codes, addresses, key and button names as printed (Enter, Esc, A, LB, Start), the
games' names and "VS".

The language never touches the game or online play:
- it is not sent in the protocol and not part of a save or the game's state;
- the log stays in English;
- the HUD's names stay as they are ("PLAYER 1");
- a room without a name is "NAME'S ROOM" in English everywhere, because everyone sees the same name.

## The mechanism (src/host/i18n.h, i18n.cpp, text.h, text.cpp, lang/*.inc)

- **One table per language, whole sentences.** `src/host/lang/en.inc` is the list of strings, `T(ID, "English")`, each
  with a comment saying where it is shown and how much room it has. Every other language has its own file
  (`ko.inc`, `ja.inc`, `zh-Hans.inc`, ...) with the same IDs. A string is a whole sentence. What changes in it is a
  named place, `{name}`, `{code}`, `{n}`, and a translation puts the place where its language needs it. There is no
  concatenation of pieces: the word order is the translation's.
- **IDs.** `text.h` turns each line of `en.inc` (an X-macro) into a constant: `tr::ONLINE`, `tr::C_SEED_HELP`. A
  `tr::text` converts to `std::string` in the language currently on, so it goes anywhere a string did. A missing
  translation falls back to English, which `langtest` catches.
- **Filling places.** `tr::fill(tr::C_SEED_UNKNOWN, {{"seed", typed}})` fills the places in one pass.
- **Messages kept for later.** A notice, a failure's why and an error's line are a `tr::line`: an ID with its places,
  or a raw string. `.str()` renders it in the language on at the time it is drawn, so a language switch re-renders
  it. `.en()` renders it in English for the log (the CPU notice, the 3D renderer's value in the log, the online
  notices).
- **src/net's sentences.** `tr::from_net(english)` maps the shared net code's English sentences to `NET_*` IDs, so
  src/net stays the same in every game and the menus still show them translated.
- **The setting.** Game > Language: Auto, then each language in its own name and script ("한국어", "Русский").
  Auto follows the system (`SDL_GetPreferredLocales`): zh-TW, zh-HK and zh-MO (or a script tag Hant) give
  Traditional Chinese, any other Chinese Simplified, any Portuguese pt-BR, anything else not covered English.
  The language is set as soon as settings.ini is read (before the ROM is asked for: `use_language` in main.cpp), so
  the game's own boxes come up in it too; their log lines stay English. `settings.ini` stores it as `language = auto|en|ko|ja|zh-Hans|zh-Hant|es|pt-BR|fr|ru|de|it|pl|tr`. Left or right on
  the row switches the language at once (`set_language` in main.cpp, then `menus_relabelled`: the title, the main
  page and Game made again, Language still selected).
- **VF1's own pieces.** A fighter on the VS screen (`vf1net::fighter_words`' RANDOM, SECRET: SIBA, SEED 1234) is put
  in the language on by main.cpp's `fighter_shown`; a secret character's name (`model1::UNUSED_CHARACTERS`, "Takeru
  (early Akira)") by `online_menu::secret_name`: the name as the game has it, what's said of it in brackets
  translated.
- **For tests.** `VF_LANG=<code>` (or `pseudo`) overrides the setting for a run. `VF_LANG=pseudo` gives accented
  letters, about 40% longer, in brackets: it finds text that isn't going through the tables and the boxes that are too
  tight.

### How to add a string

1. Add `T(MY_ID, "The English sentence with a {place}.")   // where it shows, how much room` to `en.inc`, in its
   section.
2. Use `tr::MY_ID` in the code, or `tr::fill(tr::MY_ID, {{"place", value}})`; for a message drawn later, a
   `tr::line(tr::MY_ID, {{...}})`.
3. Add the line to every other `lang/*.inc`. If there is no translation yet, add the English: it is listed, not
   hidden. Keep the glossary's terms (`docs/i18n-glossary.md`). For Chinese and Japanese, use full-width punctuation,
   but keep ASCII for codes and numbers.
4. Build and run `langtest` (`make build/bin/langtest`). It fails on an ID missing, doubled or extra in any language;
   a translation whose `{places}` differ from the English; a letter that is in no font for that language (re-run
   `tools/fonts/subset.py`); a string that starts or ends with a space.
5. Run `tools/langcheck.py`: menutest in every language, listing every box the text overflows where the English
   doesn't, and every check the English passes that this language fails. Shorten the translation, or give the layout
   room without changing the English picture.

There are no plural rules: where a count changes the word, the strings are written so that they don't need one.

### How to add a language

1. Add it to the `tr::lang` enum (before `pseudo`), `LANGS`, `REAL_LANGS`, `info()` and `from_locale` (i18n.*: the
   same in VF2 and VF3, so change all three).
2. Add `lang/<code>.inc` and add it to `text.cpp` (an `#include` and the `TABLE` and `TABLE_COUNT` entries).
3. Fonts: if Barlow Condensed doesn't have its letters, add a subset (`tools/fonts/subset.py`) and put the face in
   `FALLBACK_FONTS` (menu.cpp) and `fallback_order()`.
4. Add it to `tools/langcheck.py`'s list, and to the Online site page's (VF3's repo, shared by the three games).
5. Run `langtest`, `langcheck`, and look at the pictures.

## Fonts and their size

Barlow Condensed stays the face for every Latin letter and number in every language (its Latin Extended has Polish
and Turkish). Everything else comes from Noto Sans subsets, used as RmlUi fallback faces, cut to the letters VF1's
tables use:

| script | files | bytes | letters |
|---|---|---|---|
| Korean | NotoSansKR-Medium, -Bold | 140,192 | 442 |
| Japanese | NotoSansJP-Medium, -Bold | 351,484 | 532 (the kanji used, plus all the kana) |
| Simplified Chinese | NotoSansSC-Medium, -Bold | 316,536 | 603 |
| Traditional Chinese | NotoSansTC-Medium, -Bold | 366,552 | 604 |
| Cyrillic | NotoSansCyr-Medium, -SemiBold, -BoldItalic (condensed, wdth 75) | 31,196 | 66 |
| **total** | 11 files | **1,205,960 (1.15 MB)** | |

The licences (`assets/fonts/OFL-NotoSansCJK.txt`, `OFL-NotoSans.txt`) and the credit (`assets/fonts/README.txt`,
README.md) go with them, and `tools/release/licenses.sh` includes them. To remake the fonts after a translation adds
a letter:

```
cd vf1-recomp
pip install fonttools
python3 tools/fonts/subset.py <folder with NotoSansKR[wght].ttf, NotoSansJP[wght].ttf, NotoSansSC[wght].ttf,
                               NotoSansTC[wght].ttf, NotoSans[wdth,wght].ttf, NotoSans-Italic[wdth,wght].ttf>
```

(the variable fonts from google/fonts' `ofl/notosans*`). **RmlUi** (third_party/rmlui, as VF3's): fallback faces
ordered by family priority, then style, then weight (`Rml::SetFallbackFontOrder`, the language on's script first);
faces remember their style and weight; `ElementText` breaks lines between CJK characters with the kinsoku rules. CJK
is never slanted.

**Layout.** In languages other than English, a page's help note shrinks from 17dp down to 14dp until it fits.
Translations were shortened wherever `langcheck` found a box that the English fits. English's own long lines were
left alone, so that its pictures stay the same (Window size's help, CRT colours' "BT.601 PAL (...) (not with this
renderer)"): the same rows in other languages overflow the same way.

## Tests

- **`langtest`**: the tables, the places, every letter of every language in its fonts, the language names in their
  fonts, `tr::line` and `from_net`, `fill`, the setting's round trip, the locale mapping and pseudo's length.
- **`menutest`** in any language (`VF_LANG=ko`): every Online screen, the in-match menu, the notices, the VS screen
  (with each player's fighter), the watching cells; replicas of every main.cpp page (`pages_*.png`: the main page, in
  and out of a match, Display and its other values, Sound, Game and each language, Arcade settings, Cheats with its
  headings and sub rows and each secret character, Online > Settings' fighter, Controls, the keys and pad buttons,
  the prompts and toasts), each row selected so each help is measured; every line checked against its boxes; the
  rows' left edges checked to be in line; a runtime switch with a page open (ko, ja, ru, tr, then back to English,
  the identical picture). In English its pictures are pixel-identical to the ones before the languages (the two of
  the interrupted connection's spinner differ between any two runs, before as after).
- **`tools/langcheck.py`**: menutest in all 13 languages plus pseudo, each compared with English: every translation 0
  boxes overflowed that the English fits and 0 extra failed checks (pseudo is listed, not failed).
- **The game itself, headless, per language**: `VF_SCRIPT` (both players start, then 2200 frames of `fight`),
  `VF_STEP_HZ=57.5205` (one frame a pass), no sound device, `language = <code>` in settings.ini: the frames
  2984-2995 (`VF_CAPTURE`) are byte-identical in all 13 languages and pseudo; and the menu opened over the game
  (`VF_MENU_KEYS`) is drawn in each.
- **Online**: the language isn't in any packet. nettest, delaytest, watchsim 0/1/50 (and through the lobby's
  `test/serve.mjs`), and menutest against that lobby all pass as before. src/net isn't changed.

## VF1's own strings

The online, room, watching, Display, Sound, Controls and net strings are VF3's, in VF3's words. The random fighters,
the seed's row, Ring size, Continue and the other rows VF1 shares with VF2 (the same English) are VF2's words in every
language, so the two games agree. VF3's own rows (Team
Battle, Dural, the stage select, Distant textures, Quad rendering, Smooth motion, energy, country, debug mode) aren't
here. VF1's own, in all 13 languages:

- **Cheats**: the page's name and its headings (FIGHTERS, FIGHT, LOOKS); each player's fighter (Normal, Random,
  Seed, Secret character), the seed's row (its help, "Enter to type a seed", the typing prompt and its two mistakes),
  the secret character's row and what each one is (Siba, Jeff, Takeru, Rekka, Score, Aeon, Dural), Random opponents,
  Infinite energy (Off, Player 1, Player 2, Both), One-hit KO, No time limit, Ring (Normal, Tiny, Huge), Kage's mask
  (Comes off, As the arcade).
- **Online > Settings**: Your fighter (and Secret: a name), with the Cheats page's seed row.
- **Arcade settings**: Language (the ROM's text, Japanese or English: called "the game's language" in the other
  languages, as Game > Language is the menus'), Ring size and its five values, Continue; Difficulty's and the service
  menu's helps as VF1's.
- **Display**: VF1's helps (Render scale, Antialiasing, 3D renderer, Graphics driver, Picture shape, Background
  filter), Antialiasing's Low and "Off (too large here)", the 3D renderer's values (won't start, for this run, no GPU,
  this build).
- **The CPU notice**: "Drawing on the CPU: {why}. {todo}" and VF1's reasons (the pictures put together on the GPU, its
  test, reading them back, the size) and "A lower Display > Render scale puts it back on the GPU."
- **The VS screen**: RANDOM, SECRET: {name}, SEED {n}.
- the game's name, "That match isn't Virtua Fighter's", vf.zip.
- **The game's own boxes**: choosing the ROM the first time (VF3's ROMS_ rows with VF1's set, MAME's "vf", vf.zip;
  ROMS_BAD_SET), a failed start (FAIL_VIDEO, FAIL_WINDOW, FAIL_GRAPHICS, VF3's) and the recompiler's warning
  (FAIL_RECOMPILE). SDL's and the ROM check's own words in them stay English.

The glossary (`docs/i18n-glossary.md`, made by `tools/glossary.py`) has the main terms side by side. Every
translation here was written without a native reviewer; the points to check first are VF3's (its i18n-design.md:
the host, room, watching, rollback and arcade terms) and, for VF1:

- **Seed**: 시드, シード, 种子/種子, Semilla, Semente, Graine, Сид, Seed, Seme, Ziarno, Tohum (VF2's too). Is it the word
  players expect for a random fighter's name?
- **Normal / Random** (a player's fighter): 보통 / 랜덤, ノーマル / ランダム; Polish's fighter is a postać (Normalna, Losowa,
  Tajna postać), as VF2's, where VF3's prose has zawodnik.
- **Secret character**: 숨겨진 캐릭터, 隠しキャラクター, 隐藏角色/隱藏角色, personaje secreto, personagem secreto,
  personnage secret, секретный персонаж, geheime Figur, personaggio segreto, sekretna postać, gizli karakter.
- **Ring out**: 링아웃, リングアウト, 出界; "ring out" kept in Spanish, Portuguese, French, Italian and Polish,
  Ring-out in German, ринг-аут in Russian, "ring dışı" in Turkish.
- **Japanese** names Kage 影丸 (as VF3's tables do) and Akira アキラ (as VF2's) in sentences, the cut characters in
  katakana; the values show the game's own Latin names. Shortened to fit: "隠し{name}", "{name}（旧アキラ）",
  "{name}（コスコル）", "1Pのキャラクター" (VF2's プレイヤー1のキャラクター didn't fit with its value).
