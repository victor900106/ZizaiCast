# On-screen translation (`pm_translate`) — 0.7

「翻譯畫面」 translates the text on the mirrored phone picture in place: the
picture freezes, PaddleOCR (bundled ONNX Runtime + downloaded models; Windows
OCR as the fallback) finds the text lines, an **offline** translation
engine translates them, and themed cards with the translation are drawn over
the original text (`VideoWindow::setTextOverlay`, see docs/video.md
*Magnifier, high contrast, freeze, text overlay*). 「顯示原文」 hides the
cards (only their outlines stay); a dragged rectangle translates just that
area; an optional live mode re-translates every N seconds.

Nothing leaves the PC: the OCR models (PaddleOCR, ModelScope) and the
translation models (Mozilla's CDN) are downloaded once — after the user
agreed — SHA-256 verified, stored under `%LOCALAPPDATA%\PhoneMirror\models`.
It works on photos taken with the phone camera (labels, signs, menus at an
angle) as well as on screens, with no Windows language pack.

```
grabPicture (frozen frame, content coords) ─► [first use: consent ─► OCR models]
  ─► PaddleOCR det + rec (onnxruntime.dll; Windows.Media.Ocr if unavailable)
  ─► groupLines (paragraphs / UI labels) ─► language per block (script)
  ─► [first use: consent ─► download + SHA-256] ─► Bergamot (bergamot.dll)
  ─► setTextOverlay (cards)                      ja / ko / zh-Hans → en → zh-Hant
```

## Engine decision

Requirements: offline, free, small download, permissive licence (the app is
GPL), good quality for ja / ko / en → 繁體中文 (and → English for the English
UI), runnable from C++ on the CPU.

| Candidate | Pairs we need | Download | Licence | Runtime | Verdict |
|---|---|---|---|---|---|
| **Mozilla Firefox Translations (Bergamot)** | ja/ko/zh-Hans/zh-Hant → en, **en → zh-Hant (Taiwan wording)**; ja/ko → zh-Hant via English (as Firefox does) | 49.5 MB (en→zh-Hant), 54.8 MB (ja→en), 54.0 MB (ko→en); ja→zh-Hant = 104 MB | models MPL-2.0; engine MPL-2.0 + MIT/Apache parts | `bergamot.dll` 6.6 MB, built here without BLAS/MKL | **chosen** |
| OPUS-MT (Helsinki-NLP) via ONNX Runtime / CTranslate2 | ja/ko/zh → en, en → zh (`>>cmn_Hant<<`); no ja→zh / ko→zh | ~110 MB per direction (int8 ONNX) | CC-BY-4.0 | ORT / CT2 (no CT2 in vcpkg) | clearly worse (below), slower |
| Argos Translate | same OPUS-based models | 70–120 MB per direction | MIT + CC-BY | Python | no (Python, quality as OPUS) |
| NLLB-200 distilled 600M | direct | ~600 MB | **CC-BY-NC-4.0** | CT2 | no: non-commercial terms pass to users |
| Small LLMs (Qwen2.5-1.5B Q4 1.1 GB, Qwen3, HY-MT1.5-1.8B) | direct by prompt | 0.5–1.8 GB | Apache-2.0 (HY-MT: territory-restricted, excludes EU/UK/KR) | llama.cpp | no: 10–20x larger, seconds per screen, drift to Simplified |
| Windows built-in | none (no public text-translation API; Phi Silica needs NPU + LAF) | — | — | — | not available |

Evidence (run here, i7-13700K, CPU only; same 14 real UI strings per
language; full outputs in the test logs):

| Source | OPUS-MT int8 (pivot via en) | Bergamot (pivot via en) |
|---|---|---|
| 機内モード | 列印模式 ✗ | 飛機模式 |
| カートに追加 | 新增到 Curt ✗ | 加入購物車 |
| 送料無料（3,980円以上のご注文） | 免費送來 ✗ | 免運費（3,980 日圓或以上） |
| 明日の天気：晴れのち曇り | 我要給你安排新的一天 ✗ | 明日天氣：晴朗且多雲 |
| 장바구니에 담기 | 包裝在球衣裡 ✗ | 加入購物車 |
| 무료 배송 | 自由船 ✗ | 免費送貨 |
| 저장 공간이 거의 가득 찼습니다 | 你幾乎滿是儲藏區域 ✗ | 儲存空間幾乎已滿 |
| Add to Cart | 添加到墨盒 ✗ | 加入購物車 |
| Free shipping on orders over $35 | 自由運輸,35美元以上 ✗ | 訂單超過 35 美元免運費 |
| Sign in with Apple | 以蘋果簽署 ✗ | 使用 Apple 登入 |
| speed (14 lines) | 220–320 ms **per line** (Python greedy loop over ONNX Runtime) | 46–206 ms **per batch** of 14 (warm) |

Bergamot's own metrics (Mozilla registry, FLORES-200 COMET22): ja→en 0.862,
ko→en 0.864, en→zh-Hant 0.870. We use the 2.1 "base-memory" ja/ko/zh-Hans→en
models (Firefox Android's; COMET22 0.862 vs 0.8621 for the 59.5 MB desktop
ones, but 44 MB): identical quality on our samples (slightly better on
キャンセル / 天気), 15 MB less to download.

**Online fallback: not implemented.** Every pair we need exists offline, so
the optional online path (which would have to be off by default, explicitly
enabled with a consent dialog naming the service, and documented as sending
the screen text to that service) is not needed; no code calls any
translation service. If a future target language has no offline model, add
it behind such a switch — never as an automatic fallback.

## Models

Pinned in `translate/src/models.inc` (generated from Mozilla Remote Settings
`main/translations-models`, 2026-10-08): pair, file type, name, size,
SHA-256, CDN location under
`https://firefox-settings-attachments.cdn.mozilla.net/main-workspace/translations-models/`.

| Pair | Files | MB | Used for |
|---|---|---|---|
| `ja-en` 2.1 | model, lex, vocab | 54.8 | ja → en, ja → zh-Hant (pivot) |
| `ko-en` 2.1 | model, lex, vocab | 54.0 | ko → en, ko → zh-Hant |
| `zhHans-en` 2.1 | model, lex, vocab | 54.6 | zh-Hans → en (English UI) |
| `zhHant-en` 2.0 | model, lex, srcvocab, trgvocab | 51.8 | zh-Hant → en (English UI) |
| `en-zhHant` 2.0 | model, lex, srcvocab, trgvocab | 49.5 | en → zh-Hant, pivot target |

zh-Hans → zh-Hant needs no model (`LCMapStringEx`, characters only).

* Stored in `%LOCALAPPDATA%\PhoneMirror\models\bergamot\<pair>\` (+
  `config.yml` written on demand; `PM_MODELS_DIR` overrides, tests use
  `build-translate\models`). Download: WinHTTP, `.part` file, SHA-256
  (BCrypt) of the exact bytes, then renamed; a `<file>.sha256ok` marker means
  "verified" (later checks compare sizes, no re-hash). A wrong hash deletes
  the file (`TrVerifyFailed`).
* If Mozilla republishes a file under a new location (404), the records API
  (`firefox.settings.services.mozilla.com/.../translations-models/records`) is
  searched for the **same SHA-256** and that location used; anything else
  fails. Updating to new model versions = regenerate `models.inc`.
* Never bundled in the installer. The consent dialog (`TrModelAsk`) names the
  pair, the size, Mozilla Firefox Translations and MPL-2.0, and says the text
  never leaves the PC. Declined → `TrDeclined` toast, nothing downloaded.

## OCR (PaddleOCR via ONNX Runtime; Windows.Media.Ocr as the fallback) — 0.7.0

**Why.** The owner's first real test (iPhone camera on a Japanese food label,
「八ッ橋ショコラ」) gave 5 cards out of 26 blocks, and those were garbage
(「二bØ商ロイハ八株式社…」, 「稱燒子」): this PC's Windows OCR has only en-GB,
en-US and zh-Hant-TW, the ja / ko features cannot be installed here
(`0x800f0950`) and friends' PCs usually lack them too. The kana were read by
the Chinese recogniser, the result looked like (Simplified) Chinese and the
zh-Hans → zh-Hant character conversion was shown as a "translation".

**Engine.** `translate/src/paddle_ocr.cpp`: PaddleOCR detection +
recognition, run by `onnxruntime.dll` (Microsoft ONNX Runtime 1.30.0, MIT,
official CPU build, 18.4 MB, loaded at run time from the exe's folder by full
path — never the different System32 copy; `PM_ONNXRUNTIME_DLL` overrides;
`translate/tools/get_onnxruntime.sh` fetches it, SHA-256 pinned). The C API
header is vendored in `translate/third_party/onnxruntime/include`. No
OpenCV: resize, the DB post-processing (connected regions → convex hull →
minimum-area rectangle → unclip), the perspective crop of tilted lines and
CTC decoding are implemented there (~700 lines). 2–4 intra-op threads
(measured: more threads are slower on the i7-13700K's E-cores).

| Model (ONNX, RapidOCR's conversions, tag v3.10.0) | MB | Role |
|---|---|---|
| `PP-OCRv6_det_tiny` | 1.8 | text lines (tilted boxes); input ≤ 960 px |
| `PP-OCRv6_rec_small` | 21.2 | 18,708 characters: Chinese (Simplified + Traditional), Japanese kana + kanji, Latin … |
| `korean_PP-OCRv5_rec_mobile` | 13.5 | hangul; only for lines the main recogniser could not read |

36.6 MB, pinned in `translate/src/ocr_models.inc` (size + SHA-256), pair
`"ocr"` of `ModelStore`, downloaded from `www.modelscope.cn` (RapidAI/RapidOCR,
Apache-2.0; PaddleOCR models Apache-2.0) into
`%LOCALAPPDATA%\PhoneMirror\models\ocr\` after the consent dialog
(「下載文字辨識模型」, `TrOcrConsent*`; source + licence rows). Declined →
Windows OCR for the rest of the session (asked again next start). 管理翻譯模型
lists it as 「文字辨識（PaddleOCR）」 (delete like a translation pair).

**Choice by measurement** (CPU, warm, `pm_translate_test --ocr`, 10 synthetic
photos below, 1,669 ground-truth characters; CER = character error rate):

| det + rec | CER | det ms | notes |
|---|---|---|---|
| Windows OCR (en + zh-Hant here) | 58.0 % | — (≈ 200–340 ms total) | ja / ko unreadable; English label 16.4 % |
| v5 mobile det + v5 mobile rec | 9.4 % | 220 | Japanese label 27.5 % (katakana → kanji look-alikes) |
| v6 small det + v6 small rec | 2.9 % | 780–1,040 | |
| **v6 tiny det + v6 small rec** (+ v5 Korean) | **2.5 %** (after the fixes below) | 35–45 | chosen |
| v6 tiny rec | 39.9 % | | far too weak for Japanese |
| DirectML | not tried | | CPU is fast enough (total 120–900 ms); one less DLL |

Recognition steps: lines taller than 1.5× their width are columns (Japanese
vertical writing), turned like PaddleOCR does; lines that look unread by the
main recogniser (empty, confidence < 0.8, far fewer characters than the
width allows) are read again by the Korean model and its result kept if it
has hangul and is at least as sure; once ≥ 2 Korean lines were found, number
lines get the Korean pass too (8,000 → 8,000원). Garbage filter: lines with
confidence < 0.5, single characters < 0.85, ≤ 4 characters < 0.75. Japanese
pictures (kana anywhere): look-alikes next to katakana (力力オマス → カカオマス,
ダ一 → ダー), Simplified-only forms → Japanese (膨張剂 → 膨張剤), 平 / T before a
postal code → 〒. Each line keeps its angle, glyph height (box × 0.72),
column flag and background brightness for `groupLines`.

**Script → language.** By the recognised characters (kana → ja, hangul → ko,
Han → zh-Hant / zh-Hans by `LCMapStringEx`, Latin → en), as before; with
PaddleOCR there is no "recogniser language" any more.

**Windows OCR** stays as the fallback (onnxruntime.dll missing, the OCR
download declined, a PaddleOCR error) with the old behaviour (all installed
recognisers, `looksMisread` → 「需要加入文字辨識語言」).

## Windows OCR fallback (Windows.Media.Ocr)

* Recognisers are Windows "Optical character recognition" language features.
  This PC (zh-TW + en-GB) has **en and zh-Hant only**; ja / ko could not be
  added here (`Add-WindowsCapability -Online -Name Language.OCR~~~ja-JP~0.0.1.0`
  → 0x800f0950 on this preview build 26300; nothing changed).
* Automatic recogniser choice: every installed one of ja / ko / zh-Hant /
  zh-Hans / en runs (~120–250 ms each for 1170x2532) and the result whose own
  script dominates wins (kana for Japanese, hangul for Korean, Han for
  Chinese, Latin for English). `setSource()` forces one.
* Missing language: a Japanese / Korean screen read by the Chinese / English
  recogniser comes out as radical-like stand-ins (乇 亻 冫 卜 丩 匚 …,
  bopomofo) — `looksMisread()` detects that and the user gets
  `TrOcrMissing` (how to add the OCR feature: Settings → Time & language →
  Language & region → Add a language → 日文 / 韓文 → only "Optical character
  recognition"). With no recogniser at all: `TrOcrNone`. Admin alternative
  for support: `Add-WindowsCapability -Online -Name "Language.OCR~~~ja-JP~0.0.1.0"`
  (`ko-KR`, `zh-CN`, `zh-TW`, `en-US`).
* Pictures larger than `OcrEngine::MaxImageDimension` are scaled down, small
  crops (< 1000 px) up x2. Words are joined without spaces between CJK
  characters.

## Lines → blocks → languages

`groupLines`: a line joins the block above it when the heights are similar
(glyph boxes, ratio ≤ 1.5), the gap is < 0.7 line, they overlap
horizontally, the left edges (or centres) line up, the line above does not
end a CJK sentence, and **the line above was wrapped** (the next line's first
word would not have fitted after it, measured against the widest line of
that column). So paragraphs and wrapped alert titles become one block, while
stacked UI labels ("Free shipping…", "In Stock", "Your package…") stay
separate (merged, the engine dropped "In Stock").

0.7.0 for photographed labels (PaddleOCR lines carry an angle, the glyph
height, a column flag and the background brightness): heights and gaps are
measured across tilted lines along their centre lines (gap < 0.9 glyph
height); columns never join; light-on-dark text (a label's black box) never
joins dark-on-light text; a block keeps its first line spacing (a table row
1.3x further away starts a new block); Japanese / Korean sentence endings
without 。 (…ました, …ください, …합니다, a full stop after hangul) end a
block; a new item starts with a price / number line (8,000원, 850円), a field
label (「配料：」, 「保存方法：」 within the first 8 characters), an all-capitals
heading (CAUTION), or a capitalised Latin line after one that does not
continue (no comma / hyphen); long CJK lines (>= 10 characters) may end up to
two characters short of the column edge (justified labels, perspective).
Cards use the glyph band (`Block::cy0 / cy1`), not the tilted lines' boxes.

Language per block by script; kanji-only labels (設定, 一般) on a screen with
kana are Japanese, hanja on a hangul screen Korean; zh-Hant / zh-Hans by which
`LCMapStringEx` conversion leaves the text unchanged. Skipped: no letters
(9:41, 5G, $89.99), already in the target language, one Latin letter;
0.7.0 also: fewer than 2 letters, mostly symbols, quantities (468g, 2180 kJ:
digits >= letters and <= 3 letters), short capitalised tokens on a Japanese /
Korean picture (camera UI "HEIF"), Japanese postal addresses (〒 / 123-4567:
the models garble place names), and — for a 繁體中文 target — kanji / hanja
only blocks on a Japanese / Korean picture (賞味期限, 株式会社美十: readable as
they are, while ja → en → zh-Hant turns names into nonsense such as 美十 →
美州) and zh-Hans blocks that the conversion would not change (or on a
Japanese picture). **A card is shown only for a real translation**
(`translatedOk`): not empty, not the original again, not still in kana /
hangul for a Chinese / English target, not Latin-only for a CJK source.
A small glossary fixes one-word UI labels sentence models get wrong without
context (オン → 開啟 instead of 「上」, 켬 → 開啟 instead of 「肯」, 취소 → 取消
instead of 「已取消」, Menu → 選單, 메뉴 / メニュー / お品書き → 菜單, signs:
CAUTION → 注意, Wet Floor → 小心地滑 …, ~50 menu items: 김치찌개 → 泡菜鍋,
とんこつラーメン → 豚骨拉麵 …; matched without the spaces of a
spaced-out heading 「메 뉴」). Chinese output gets full-width punctuation next
to CJK characters and `LCMapStringEx` Traditional, plus a word list for the
one-to-many characters it gets wrong (餅干 → 餅乾, 干燥 → 乾燥, 置于 → 置於,
制品 → 製品, 面粉 → 麵粉 …).

## API (`translate/include/pm/translate.h`)

Most code only needs `ScreenTranslator`:

```cpp
pm::translate::ScreenTranslator tr(win);       // win: pm::VideoWindow (after create)
tr.translateScreen();                          // UI thread: freeze + translate the whole picture
tr.translateRegion();                          // freeze + let the user drag a rectangle
tr.setShowOriginal(!tr.showOriginal());        // cards hidden (outlines) / shown
tr.setLive(true, 5);                           // re-translate the live picture every 5 s
tr.close();                                    // overlay off, live off, unfreeze if we froze
```

| Member | Notes |
|---|---|
| `ScreenTranslator(win, Callbacks)` | Own worker thread (COM MTA). `Callbacks`: `askDownload(src, tgt, MB, answer)` (default: Yes/No `MessageBox` owned by the video window), `notify(title, text, important)` (default: toast, or a `MessageBox` when important), `changed()` (refresh menus), `finished(ok, Timing)`, `ocrOverride` (test hook). All callbacks run on the UI thread except `ocrOverride`. |
| `setTarget(Lang)` / `target()` | `ZhHant` (default with the 繁體中文 UI) or `En` (English UI: `defaultTarget()`). |
| `setSource(Lang)` / `source()` | `Unknown` = automatic (default). |
| `translateScreen()` | Ignored while busy. Freezes (unless live), clears old cards, busy card 「正在辨識文字…」 → 「正在下載翻譯模型… N%」 → 「正在翻譯…」 → cards. |
| `translateRegion()` | `beginRegionSelect`; Esc / right click cancels (unfreezes again if it froze). |
| `setShowOriginal(bool)` | `setTextOverlayOriginal`. |
| `setLive(on, seconds)` | 2..60 s; unfreezes; live re-runs never ask / download. |
| `active()` / `busy()` / `live()` / `showOriginal()` | For menu check marks / greying. |
| `close()` | Drops results still in flight (generation counter), cancels a download. |
| `lastTiming()` / `lastItems()` | `Timing { grabMs, ocrMs, translateMs, totalMs, lines, blocks, translated, source }`; items = original + translation + box. |

Lower level (also used by the tests): `Ocr::installed / available /
recognize`, `groupLines`, `detectScript`, `toTraditional`,
`ModelStore::root / pairsFor / installed / missingBytes / download /
configPath`, `Engine::available / translate`.

## Building the engine (`bergamot.dll`)

`translate/tools/build_bergamot.sh [build-dir]` (Git Bash, VS 2026, vcpkg
`pcre2:x64-windows-static`): clones BergamotTranslatorSharp (MPL-2.0; its C
API `translator_initialize / translator_translate_multiple / …` and Windows
patches) at a pinned commit with bergamot-translator + marian-dev, enables
Marian's Eigen-based ONNX sgemm (`USE_ONNX_SGEMM`, normally WASM-only) so
**no Intel MKL and no BLAS** is linked (MKL's licence is not GPL-compatible),
stubs faiss' unused Fortran BLAS/LAPACK symbols (`translate/bergamot/
lapack_stubs.cpp`, abort if ever called; Firefox models use a lexical
shortlist), `/arch:SSE2` baseline (intgemm still picks AVX2 / AVX-512 at run
time). Output `build-translate/bergamot/bin/bergamot.dll`, 6.6 MB, static CRT;
translations byte-identical to the MKL build of the same engine, similar speed
(14 lines, warm batch: ja→zh-Hant 153 ms vs 197 ms with MKL, en 68 vs 41 ms). The CMake cache path
`PM_BERGAMOT_DLL` is copied next to the test tool.

## Integration (app/ — done in 0.7.0, see docs/app.md *放大鏡 / 翻譯*)

The app follows the plan below with two changes: the menus are two submenus
(放大鏡 ▸ with zoom, colours and 凍結畫面; 翻譯 ▸ with 翻譯整個畫面, 框選翻譯, 顯示原文,
連續翻譯, 翻成, 管理翻譯模型), and the targets include 日本語 / 한국어 (below).

**日本語 / 한국어 targets (0.7.0).** Mozilla's registry (fetched 2026-10-08)
has en → ja (2.3, all platforms, 49.6 MB) and en → ko (2.1, the Android
base-memory model like ja/ko → en, 51.9 MB; 2.0 desktop is 67.6 MB); both
are pinned in `models.inc` as `en-ja` / `en-ko`. `pairsFor(src, Ja|Ko)` =
`{src-en, en-ja|en-ko}` (English directly), the same pivot as zh-Hant. There
is no direct ja ↔ ko or zh → ja/ko model; the pivot through English is the
same thing Firefox does. `defaultTarget()` follows the UI language (日本語 →
Ja, 한국어 → Ko). The one-word glossary only applies to zh-Hant / en
targets. Verified: both downloaded with the SHA-256 check and translated the
English test screen (「製品を検索」, 「35ドル以上のご注文で送料無料」; 「제품 검색」,
「$35 이상 주문시 무료 배송」).

Original plan:

1. **CMake**: `target_link_libraries(phonemirror PRIVATE pm_translate)`; copy
   `${PM_BERGAMOT_DLL}` next to the exe (POST_BUILD, like pm_translate_test)
   and add `bergamot.dll` to the installer (`installer/`, ~6.6 MB, same
   folder as the exe). Models are **not** in the installer.
2. **Object**: one `pm::translate::ScreenTranslator g_tr(window, cb)` after
   `window.create`; `cb.changed = refreshToolbar + menu state`;
   `cb.askDownload` / `cb.notify` may use the app's own themed dialogs
   (popup_menu style) instead of the default `MessageBox`. Set
   `setTarget(pm::translate::defaultTarget())` again when the UI language
   changes. Destroy it before the window.
3. **Context menu** (畫面 ▸ submenu, after iPhone 外框), all from
   `i18n_strings.inc`:
   * `MenuMagnifier` 放大鏡 ▸ `MenuZoomIn` 放大 (Ctrl+=), `MenuZoomOut` 縮小
     (Ctrl+-), `MenuZoomReset` 原始大小 (Ctrl+Shift+0) — greyed at the
     limits; `win.zoomStep(+1/-1)`, `win.resetMagnifier()`.
   * `MenuFilter` 高對比 ▸ radio: `FilterNone` 原本的顏色, `FilterContrast`
     加強對比, `FilterGray` 黑白（灰階）, `FilterInvert` 反轉顏色（黑白對調）,
     `FilterYellow` 黃字黑底 → `win.setFilter(...)`; Ctrl+K cycles them.
   * `MenuFreeze` 凍結畫面 ✓ (Ctrl+P) → `win.setFrozen(!frozen)`.
   * `MenuTranslate` 翻譯畫面 (Ctrl+L), `MenuTranslateRegion` 翻譯框選範圍
     (Ctrl+Shift+L), and while `g_tr.active()`: `MenuTranslateOriginal`
     顯示原文 ✓ (Ctrl+O), `fmt(MenuTranslateLive, {L"5"})` 自動重新翻譯 ✓,
     `MenuTranslateTo` 翻譯成 ▸ 繁體中文 / English (`TrLangZhHant`,
     `TrLangEn`), `MenuTranslateClose` 關閉翻譯 (Ctrl+L again).
   * 還原 (Ctrl+0) should also call `win.resetMagnifier()`.
4. **Shortcuts** (taken: Ctrl+D T S R → ← H 0 F): new **Ctrl+= / Ctrl+-**
   (zoom; built into video too, so they work even if the app does not
   handle them), **Ctrl+Shift+0** (1x, built in), **Ctrl+K** (filters),
   **Ctrl+P** (freeze), **Ctrl+L** (translate / close), **Ctrl+Shift+L**
   (area), **Ctrl+O** (original). Ctrl+wheel, drag, arrows, Alt+arrows and
   Esc (cancel selection) are handled inside video/. Ctrl+letters are never
   forwarded to an Android phone (except C V X A Z), so these do not collide
   with remote control.
5. **Toolbar** (`setLiveToolbar`, new group before 中斷連線): 放大鏡 `0xE71E`
   (click: 1x → 2x → 4x → 1x; tooltip `MenuMagnifier` + 「Ctrl+滾輪」),
   翻譯 `0xE8C1` (toggled while `g_tr.active()`; click: translate / close),
   凍結 `0xE769` (toggled while frozen). High contrast `0xE793` can stay in
   the menu.
6. **Window title / toasts**: `TrDone` (「已翻譯 {0} 段文字」) after
   `finished(ok)` if the app wants feedback; the overlay itself is the
   result.
7. **Licences** (`docs/licenses`, `THIRD_PARTY_NOTICES`): add bergamot.dll's
   components — bergamot-translator (MPL-2.0), BergamotTranslatorSharp C API
   (MPL-2.0), Marian (MIT), intgemm (MIT), SentencePiece (Apache-2.0),
   ssplit-cpp (Apache-2.0), PCRE2 (BSD-3-Clause), yaml-cpp (MIT), spdlog
   (MIT), pathie-cpp (BSD-2-Clause), faiss (MIT), Eigen (MPL-2.0), onnxjs
   sgemm (MIT), zlib (zlib), CLI11 (BSD-3-Clause) — check the exact list in
   `build-translate/bts-src` when packaging — and the downloaded models
   (Mozilla, MPL-2.0; source: github.com/mozilla/translations). MPL-2.0
   source availability: the pinned commit + `build_bergamot.sh` patches.

## Tests (`pm_translate_test`, off-screen, no sound)

```
translate/testdata/render.sh build-translate/screens        # 3 synthetic phone screens + .gt.tsv ground truth
set PM_MODELS_DIR=…\build-translate\models
pm_translate_test --ocr-langs
pm_translate_test --ocr screens\en_shop.png                  # lines, boxes, accuracy vs ground truth
pm_translate_test --sentences --target zh-Hant [--download]  # quality table + ms
pm_translate_test --overlay shots screens\en_shop.png [--download]
pm_translate_test --overlay shots screens\ja_settings.png screens\ko_chat.png --gt [--download]
```

`--gt` feeds the rendered text boxes (Chrome, `gt.js`) instead of OCR.
Without `--download` the consent is answered "no" (tests the declined path).

0.7.0 photographed test set: `py -3 translate/testdata/make_photos.py
build-translate/photos` renders 10 scenes (Japanese food label / sign / menu /
vertical shop sign, Korean food label / sign / menu, English label / sign,
Simplified Chinese label) onto paper textures with Windows fonts, puts them
on a table at an angle (perspective), adds uneven light, glare, blur, noise
and JPEG compression (quality 62) and frames them like the iPhone camera app
at 994x2160 (the owner's picture size, zh-TW camera UI); `NAME.gt.tsv` has
each line's quad. Plus the owner's screenshot (its uncovered Japanese text
transcribed by hand, 505 characters, scored as characters correct).

```
pm_translate_test --ocr photos\*.png [--engine windows] [--download]   # CER per picture + TOTAL
pm_translate_test --overlay shots photos\*.png --target zh-Hant          # cards + SUMMARY lines
PM_OCR_THREADS=n, PM_OCR_DET="limit thresh box unclip"                  # tuning overrides
powershell -File translate/tools/app_translate_test.ps1 -ExeDir build-app\bin\Release -Feed ja_food_label.h264 -Models build-translate\models [-Consent 0] [-Lang ja]
```

`app_translate_test.ps1` runs the real app off-screen (`--dev
--test-offscreen --test-no-network`, a `--test-feed` H.264 loop of a photo
made with ffmpeg), DevCommand 192 (翻譯整個畫面), answers the consent dialog
(905 shot / 906 answer) and saves window shots to `%TEMP%\pmshots\tr_*`.

## Measured (2026-10-08, i7-13700K, RTX 3060 Ti, 1170x2532 screens)

| Step | Time |
|---|---|
| `grabPicture` (frozen frame → BGRA) | 6–10 ms |
| OCR, English recogniser | 220–280 ms (auto mode runs en + zh-Hant) |
| OCR accuracy, English screen | 24/24 visible lines exact (CER 0 %; the 3 "errors" are buttons hidden under the alert) |
| Model download + verify | en→zh-Hant 49.5 MB in 1.9 s; ja→en 54.8 MB ≈ 1.6 s (this connection) |
| Model load | 75–190 ms per pair (first translation only) |
| Translation, warm, whole screen | en 18 blocks 81–110 ms; ja 16 blocks 111–124 ms; ko 14 blocks 87–100 ms |
| translateScreen total, warm | en 323–389 ms (OCR + translation); first run incl. model load 550–900 ms |
| Region (middle third) | OCR 103–113 ms, translation 18–37 ms |
| Live mode | 2 runs in 2.7 s with `setLive(true, 2)`, picture not frozen |

Quality on the screens (full lists in the test output):
ja — 機内モード → 飛機模式, 自宅のネットワーク → 居家網路, サウンドと触覚 →
聲音與觸覺, スクリーンタイム → 螢幕時間, コントロールセンター → 控制中心,
バッテリー残量が少なくなっています。低電力モードをオンにすると… → 電池較少。啟動低功耗模式可降低電池消耗。,
明日の待ち合わせは駅の改札前で10時でいいですか？ → 我明天上午10點可以在車站售票口前見面嗎？ (meaning
slightly off: "is 10:00 in front of the gates OK?"); ko — 안녕하세요! 내일
회의 시간이 바뀌었어요. → 哈囉！明天的會面時間已經改變了。, 오후 3시에 2층 회의실에서
만나요. → 下午三點在二樓的會議室見。, 저장 공간이 거의 가득 찼습니다… →
儲存空間幾乎已滿。請整理您的照片和影片。, 김민지 → 金敏司 (names are guessed);
en — Allow "Shop" to use your location? → 允許「商店」使用您的位置？, Your
package will arrive tomorrow by 8 PM. → 您的包裹將於明天晚上8點前送達。, In
Stock → 庫存 (should be 有現貨), Menu → 選單 (glossary).

## Measured 0.7.0 — PaddleOCR vs Windows OCR (2026-10-08, i7-13700K, CPU)

Before = the 0.7.0 build before this change (Windows OCR en + zh-Hant, the
only recognisers this PC can have); after = PaddleOCR + the new grouping /
filters. Target 繁體中文. "cards" = translated blocks shown; before, most
Japanese / Korean pictures ended with no card or a zh-Hans "conversion" of
garbage. Times: warm `translateScreen` total (OCR + translation; the first
run adds model loading, about 0.4–1 s).

| Picture | chars correct before → after | blocks before → after | cards before → after | ms after |
|---|---|---|---|---|
| ja_food_label (tilted, glare) | 4.3 % → 95.4 % | 8 → 28 | 0 → 10 | 1,584 |
| ja_menu | 56.4 % → 100 % | 17 → 24 | 0 → 8 | 377 |
| ja_sign | 52.1 % → 100 % | 0 → 13 | 0 → 5 | 260 |
| ja_vertical (columns) | 63.9 % → 100 % | 7 → 11 | 1 (garbage) → 3 | 157 |
| ko_food_label | 6.4 % → 92.3 % | 6 → 24 | 0 → 15 | 1,277 |
| ko_menu | 19.3 % → 100 % | 12 → 22 | 1 (garbage) → 8 | 407 |
| ko_sign | 17.5 % → 100 % | 5 → 11 | 0 → 4 | 247 |
| en_label | 83.6 % → 99.5 % | 15 → 26 | 10 → 13 | 521 |
| en_sign | 93.7 % → 100 % | 4 → 12 | 4 → 5 | 208 |
| zhs_label | 89.0 % → 100 % | 10 → 16 | 6 → 8 | 258 |
| **all 10 photos** | **42.0 % → 97.5 %** (CER 58.0 % → 2.5 %) | | | |
| owner's screenshot (533x979 window shot, uncovered text) | 20.4 % → 97.8 % | 23 → 46 | 1 → 14 | 1,941 |
| synthetic screens ja_settings / ko_chat / en_shop | CER 67.0 / 93.2 / 8.1 % → 1.7 / 1.4 / 8.1 % | | → 15 / 13 / 18 | 501 / 982 / 581 |

(en_shop's 8.1 % are the buttons hidden under its alert, both engines.)
OCR alone: detection 30–45 ms, recognition 80–850 ms (about 20 ms per line;
a Korean picture reads its lines twice). OCR model download 36.6 MB in 4.1 s
here. Translation quality on the photos: sentences are good (賞味期限は…お召し
上がりください → 到期日為未開封狀態的到期日。開封後請盡快食用。; 本日は臨時休業と
させていただきます → 今天我們將暫時關閉。; 화장실은 2층에 있습니다 → 二樓的廁所。;
Store in a cool, dry place… → 存放在陰涼乾燥處，遠離陽光直射。); dish names
went wrong through the English pivot (味噌ラーメン → 米蘇·拉門, 된장찌개 → 美索江),
so a glossary of ~50 common Japanese / Korean dishes and drinks now gives
味噌拉麵, 大醬湯, 拌飯, 辣炒豬肉, 加點白飯 …; product names still go wrong
(八ッ橋 → 哈希).

## Known gaps

* Brand / product names, uncommon dishes and ingredient lists go through
  English and are often transliterated or wrong (see above).
* Tables on labels: a long value cell next to the next row can still be
  joined (ko_food_label 원재료 + 보관 rows), and a cut-off line (glare, edge)
  is translated as it was read.
* Text at more than ~30° or upside down is not straightened (no
  orientation classifier); DirectML / GPU is not used.
* Bergamot aborts the process on a corrupt model (Marian `ABORT`); the
  SHA-256 check before use makes that practically impossible, but the engine
  is not process-isolated.
* Names and one-word labels without context are guessed (金敏司, 庫存).
