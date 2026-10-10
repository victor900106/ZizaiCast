# Step 2 of the glossary build: wikidata_labels.jsonl + curated_{ja,ko}.tsv
#   -> glossary_ja.tsv, glossary_ko.tsv (src  zh  mode  kind  conf  source), rejected.tsv, stats.txt / stats.json
#
#   py build_glossary.py
#
# zh-Hant choice per item: zh-tw label (conf A) > zh-hant (B) > zh-hk (B, HK wording) > zh / zh-hans / zh-cn
# converted with OpenCC s2twp (C).  Curated entries are conf A and win over Wikidata.
# Precision rules (an entry that could fire on an ordinary word is dropped or limited to whole blocks):
#   * source >= 2 characters, has kana / kanji / hangul, no brackets / digits-only / list punctuation;
#   * zh has Han characters (Latin only for brands / companies), no kana / hangul, not a description;
#   * Japanese kanji-only names whose zh is the same characters (渋谷区 -> 澀谷區) are dropped: the app already
#     shows kanji-only text in Traditional forms;
#   * one source -> two different zh at the best confidence: dropped (ambiguous);
#   * hiragana-only Wikidata sources <= 3 characters dropped (なし, もも), longer ones whole-block only;
#   * mode s (inside sentences) only for: katakana / mixed Japanese >= 3 chars, kanji names with 駅 / 線 / 寺 …,
#     Korean >= 3 syllables (2 for stations / cities / dishes not in the homonym stop list);
#     a source with a negation word (禁 不 無 非 금지 없) is whole-block only;
#   * stop lists (stoplist_{ja,ko}.txt): homonyms and ordinary words, never used.
import collections
import json
import os
import re
import sys
import unicodedata

import opencc

sys.stdout.reconfigure(encoding="utf-8")
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
S2TWP = opencc.OpenCC("s2twp")
HK2TW = None
try:
    HK2TW = opencc.OpenCC("hk2t")
except Exception:  # noqa: BLE001
    pass

KANA = re.compile(r"[ぁ-ヿｦ-ﾟ]")
HIRA_ONLY = re.compile(r"^[ぁ-ゟー]+$")
KATA = re.compile(r"[ァ-ヺー]")
HANGUL = re.compile(r"[가-힯ᄀ-ᇿ㄰-㆏]")
HAN = re.compile(r"[㐀-䶿一-鿿々]")
KANJI_ONLY = re.compile(r"^[㐀-䶿一-鿿々ヶケッ]+$")
LATIN = re.compile(r"[A-Za-z]")
BAD_SRC = re.compile(r"[()（）\[\]「」『』、,/／:：;；·・!?！？\"“”]")
BAD_ZH = re.compile(r"[()（）\[\]、,/／;；:：\"“”]")
NEG_JA = re.compile(r"[禁不無非没]")
NEG_KO = re.compile(r"금지|없|불가|안 ")

KIND_PRI = {"curated": 0, "station": 1, "line": 1, "place": 2, "landmark": 2, "dish": 3, "drink": 3, "food": 4,
            "confectionery": 4, "ingredient": 5, "seasoning": 5, "fruit": 5, "vegetable": 5, "company": 6, "brand": 6}
NAME_KINDS = {"station", "line", "place", "landmark", "company", "brand"}
FOOD_KINDS = {"dish", "drink", "food", "confectionery", "ingredient", "seasoning", "fruit", "vegetable"}
JA_NAME_TAIL = ("駅", "線", "寺", "神社", "城", "公園", "空港", "大学", "美術館", "博物館", "タワー", "温泉", "市場", "通り")


def shin_table():
    src = open(os.path.join(REPO, "translate", "src", "preprocess.cpp"), encoding="utf-8").read()
    a = src.index("const wchar_t kShin[] =")
    b = src.index(";", a)
    s = "".join(re.findall(r'L"([^"]*)"', src[a:b]))
    return {s[i]: s[i + 1] for i in range(0, len(s) - 1, 2)}


SHIN = shin_table()


def ja_literal(s):  # ~ convertJapaneseKanji: shinjitai -> Traditional, then s2t
    return S2TWP.convert("".join(SHIN.get(c, c) for c in s))


def nfc(s):
    return unicodedata.normalize("NFKC", s).strip()


def clean_zh(z):
    z = nfc(z)
    z = re.sub(r"\s*[（(][^）)]*[）)]\s*$", "", z).strip()
    return z


def zh_ok(z, kind, src):
    if not z or BAD_ZH.search(z) or KANA.search(z) or HANGUL.search(z):
        return False
    if not HAN.search(z):
        return kind in ("brand", "company") and bool(LATIN.search(z)) and len(z) <= 20
    if len(z) > 3 * len(src) + 4 or len(z) > 24:
        return False
    return True


def src_ok(s, lang):
    if len(s) < 2 or len(s) > 24 or BAD_SRC.search(s) or s.isdigit():
        return False
    if lang == "ja":
        return bool(KANA.search(s) or HAN.search(s)) and not HANGUL.search(s)
    return bool(HANGUL.search(s)) and not KANA.search(s)


def pick_zh(labels):
    d = {}
    for l, t in labels:
        d.setdefault(l, t)
    if "zh-tw" in d:
        return clean_zh(d["zh-tw"]), "A", "zh-tw"
    if "zh-hant" in d:
        return clean_zh(S2TWP.convert(d["zh-hant"])), "B", "zh-hant"
    if "zh-hk" in d:
        return clean_zh(S2TWP.convert(d["zh-hk"])), "B", "zh-hk"
    for l in ("zh", "zh-hans", "zh-cn", "zh-sg"):
        if l in d:
            return clean_zh(S2TWP.convert(d[l])), "C", l + ">s2twp"
    return None, None, None


def load_stop(lang):
    p = os.path.join(HERE, f"stoplist_{lang}.txt")
    if not os.path.exists(p):
        return set()
    return {l.split("#")[0].strip() for l in open(p, encoding="utf-8") if l.split("#")[0].strip()}


def mode_for(src, lang, kind, derived=False):
    if lang == "ja":
        if NEG_JA.search(src):
            return "x"
        if KANJI_ONLY.match(src):
            return "s" if len(src) >= 3 and src.endswith(JA_NAME_TAIL) else "x"
        if HIRA_ONLY.match(src):
            return "x"
        return "s" if len(src) >= 3 else "x"
    if NEG_KO.search(src) or " " in src:
        return "x"
    syl = len(HANGUL.findall(src))
    if syl >= 3:
        return "s"
    if syl == 2 and kind in ("station", "place", "dish", "drink") and not derived:
        return "s"
    return "x"


def derive_ko(src, zh, kind):
    """Korean names without their suffix (홍대입구역 -> 홍대입구 / 弘大入口)."""
    out = []
    pairs = [("역", ("站",))] if kind == "station" else []
    if kind == "place":
        pairs += [("특별자치시", ("特別自治市",)), ("특별시", ("特別市",)), ("광역시", ("廣域市",)), ("시", ("市",)), ("구", ("區",))]
    for ks, zs in pairs:
        if src.endswith(ks):
            for z in zs:
                if zh.endswith(z):
                    b, zb = src[: -len(ks)], zh[: -len(z)]
                    if len(HANGUL.findall(b)) >= 2 and HAN.search(zb):
                        out.append((b, zb))
            break
    return out


def main():
    stats = collections.Counter()
    rejected = []
    cands = {"ja": collections.defaultdict(list), "ko": collections.defaultdict(list)}  # src -> [(conf, kindpri, zh, kind, source, mode)]
    stop = {"ja": load_stop("ja"), "ko": load_stop("ko")}
    # Curated.
    curated = {"ja": {}, "ko": {}}
    for lang in ("ja", "ko"):
        for ln in open(os.path.join(HERE, f"curated_{lang}.tsv"), encoding="utf-8"):
            if ln.startswith("#") or not ln.strip():
                continue
            c = ln.rstrip("\n").split("\t")
            src, zh, kind = c[0], c[1], c[2]
            mode = c[3] if len(c) > 3 and c[3] else mode_for(src, lang, kind)
            curated[lang][src] = (zh, kind, mode)
    # The app's built-in one-word glossary (label_text.cpp kG, its menu part): already exact matches there; here they also
    # become s-mode names inside a menu line (김치찌개 8,000원).
    lt = open(os.path.join(REPO, "translate", "src", "label_text.cpp"), encoding="utf-8").read()
    body = lt[lt.index("// Menus: dish names", lt.index("static const G kG[]")):lt.index("if (tgt != Lang::ZhHant && tgt != Lang::En) return nullptr;")]
    for src, zh in re.findall(r'\{L"([^"]+)", L"([^"]+)"', body):
        lang = "ja" if KANA.search(src) or (HAN.search(src) and not HANGUL.search(src)) else "ko" if HANGUL.search(src) else None
        if not lang or src in curated[lang] or "（" in zh:
            continue
        mode = mode_for(src, lang, "dish")
        if lang == "ja" and KANJI_ONLY.match(src):
            continue
        curated[lang][src] = (zh, "builtin", mode)
        stats[f"{lang}_builtin"] += 1
    # Wikidata.
    wpath = os.path.join(HERE, "wikidata_labels.jsonl")
    n_items = 0
    for ln in (open(wpath, encoding="utf-8") if os.path.exists(wpath) else []):
        it = json.loads(ln)
        n_items += 1
        zh, conf, zsrc = pick_zh(it["labels"])
        kinds = sorted(it["kinds"], key=lambda k: KIND_PRI.get(k, 9))
        kind = kinds[0]
        if not zh:
            stats["item_no_zh"] += 1
            continue
        for lang in ("ja", "ko"):
            srcs = [(nfc(t), False) for l, t in it["labels"] if l == lang]
            if kind in FOOD_KINDS or kind in ("brand", "company"):  # aliases: menu spellings (すし / 鮨), short brand names
                srcs += [(nfc(t), True) for l, t in it.get("aliases", []) if l == lang]
            for src, alias in srcs:
                why = None
                if not src_ok(src, lang):
                    why = "src"
                elif not zh_ok(zh, kind, src):
                    why = "zh"
                elif src in stop[lang]:
                    why = "stoplist"
                elif lang == "ja" and KANJI_ONLY.match(src) and ja_literal(src) == zh:
                    why = "kanji-identity"
                elif lang == "ja" and HIRA_ONLY.match(src) and len(src) <= 3:
                    why = "short-hiragana"
                elif lang == "ja" and KATA.search(src) and len(src) <= 2 and kind not in FOOD_KINDS:
                    why = "short-katakana"
                if why:
                    stats[f"rej_{lang}_{why}"] += 1
                    rejected.append((lang, src, zh, kind, why, it["qid"]))
                    continue
                c2 = conf if not alias else {"A": "B", "B": "C", "C": "C"}[conf]
                m = mode_for(src, lang, kind)
                if alias and m == "s" and kind in ("brand", "company") and len(src) < 4:
                    m = "x"
                cands[lang][src].append((c2, KIND_PRI.get(kind, 9), zh, kind, f"wikidata:{it['qid']}:{zsrc}{':alias' if alias else ''}", m))
                if lang == "ko" and not alias:
                    for b, zb in derive_ko(src, zh, kind):
                        if b in stop["ko"]:
                            continue
                        cands["ko"][b].append(({"A": "B", "B": "C", "C": "C"}[conf], KIND_PRI.get(kind, 9) + 1, zb, kind,
                                               f"wikidata:{it['qid']}:{zsrc}:derived", mode_for(b, "ko", kind, derived=True)))
    stats["wikidata_items"] = n_items
    out = {}
    for lang in ("ja", "ko"):
        rows = {}
        for src, (zh, kind, mode) in curated[lang].items():
            rows[src] = (src, zh, mode, kind, "A", "builtin" if kind == "builtin" else "curated")
        for src, cs in cands[lang].items():
            if src in rows:
                stats[f"{lang}_curated_overrides_wikidata"] += 1
                continue
            best = min(c[0] for c in cs)
            top = [c for c in cs if c[0] == best]
            zhs = {c[2] for c in top}
            if len(zhs) > 1:
                # Same name, readings differing only by 站 / 車站 or by a trailing 市 / 區: keep the shortest; else ambiguous.
                base = {re.sub(r"(車站|站|市|區|縣|町|村)$", "", z) for z in zhs}
                if len(base) > 1:
                    stats[f"rej_{lang}_ambiguous"] += 1
                    rejected.append((lang, src, " | ".join(sorted(zhs)), top[0][3], "ambiguous", top[0][4]))
                    continue
            top.sort(key=lambda c: (c[1], len(c[2])))
            c = top[0]
            mode = "x" if any(t[5] == "x" for t in top) else c[5]
            rows[src] = (src, c[2], mode, c[3], c[0], c[4])
        order = sorted(rows.values(), key=lambda r: (r[4], KIND_PRI.get(r[3], 9) if r[5] != "curated" else 0, r[0]))
        out[lang] = order
        with open(os.path.join(HERE, f"glossary_{lang}.tsv"), "w", encoding="utf-8", newline="\n") as f:
            f.write(f"# {lang} -> zh-Hant (Taiwan). Built by build_glossary.py; sources and licences: SOURCES.md.\n")
            f.write("src\tzh\tmode\tkind\tconf\tsource\n")
            for r in order:
                f.write("\t".join(r) + "\n")
    with open(os.path.join(HERE, "rejected.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("lang\tsrc\tzh\tkind\twhy\tsource\n")
        for r in rejected:
            f.write("\t".join(r) + "\n")
    # Stats.
    st = {"wikidata_items": n_items}
    lines = [f"Wikidata items fetched: {n_items} (no zh label: {stats['item_no_zh']})"]
    for lang in ("ja", "ko"):
        rows = out[lang]
        size = os.path.getsize(os.path.join(HERE, f"glossary_{lang}.tsv"))
        by = lambda i: dict(collections.Counter(r[i] for r in rows).most_common())  # noqa: E731
        st[lang] = {"entries": len(rows), "bytes": size, "mode": by(2), "kind": by(3), "conf": by(4),
                    "source": dict(collections.Counter(r[5].split(":")[0] + (":derived" if r[5].endswith("derived") else "") for r in rows))}
        lines.append(f"{lang}: {len(rows)} entries, {size / 1024:.0f} KiB; mode {st[lang]['mode']}; conf {st[lang]['conf']}")
        lines.append(f"    kind {st[lang]['kind']}")
        lines.append(f"    source {st[lang]['source']}")
    rej = {k: v for k, v in stats.items() if k.startswith("rej_") or "override" in k}
    st["rejected"] = rej
    lines.append(f"rejected / merged: {rej}")
    open(os.path.join(HERE, "stats.txt"), "w", encoding="utf-8").write("\n".join(lines) + "\n")
    json.dump(st, open(os.path.join(HERE, "stats.json"), "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print("\n".join(lines))


if __name__ == "__main__":
    main()
