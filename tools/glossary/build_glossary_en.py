# Step 2 (English) of the glossary build: curated_en.tsv + the zh_TW localisation vote (raw_l10n/) + Wikidata food names
# (wikidata_en.jsonl) -> glossary_en.tsv (src  zh  mode  kind  conf  source), rejected_en.tsv, stats_en.txt / stats_en.json
#
#   py build_glossary_en.py [--no-eval]      (--no-eval: leave out the curated rows noted `eval`, for a held-out measurement)
#
# English common words are highly ambiguous, so:
#   * whole block (mode x) is the default; a single word is never matched inside a line unless curated says so;
#   * mode s (inside a line, whole words, case-insensitive) only for multiword terms / unambiguous names;
#   * mode q (curated nutrients: Sodium, Iron, Energy): inside a line only when the rest of the line is an amount;
#   * anything with a negation (no, not, don't, non-, -free, without, never, un…ed) is whole-block only;
#   * stoplist_en.txt: words whose zh depends on context, never taken from the vote or Wikidata;
#   * keys are case-insensitive and whitespace-collapsed (the loader does the same): the first row of a key wins.
# Localisation vote (UI terms): msgids of 1-4 words (letters, spaces, ' - & only; accelerators, trailing … / : removed)
# from GNOME, KDE, LibreOffice and Mozilla zh_TW / zh-TW files.  Each project votes its own majority msgstr (normalised:
# accelerators, trailing punctuation removed); an entry is kept when >= 3 projects of >= 2 families translate it, the
# top translation has >= 75 % of the project votes and every family's own majority agrees with it.  Else: rejected
# (conflict / too few).  conf A: >= 3 families and >= 6 projects; else B.
import collections
import glob
import json
import os
import re
import sys
import unicodedata

sys.stdout.reconfigure(encoding="utf-8")
try:
    import opencc  # build-time only (Apache-2.0); s2t: character-level simplified -> traditional
    S2T = opencc.OpenCC("s2t")
except Exception:  # noqa: BLE001
    S2T = None
HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(HERE, "raw_l10n")
HAN = re.compile(r"[㐀-䶿一-鿿]")
ASCII_LETTER = re.compile(r"[A-Za-z]")
NEG = re.compile(r"\b(no|not|don't|dont|do not|never|non|without|cannot|can't|isn't|aren't|won't|nothing|none)\b|-free\b|\bfree of\b|^un[a-z]+(ed|able)\b", re.I)
KIND_PRI = {"ui": 1, "error": 1, "sign": 1, "transit": 1, "nutrition": 1, "allergen": 1, "label": 1, "menu": 1,
            "dish": 2, "drink": 2, "food": 3}
FAMILY_LICENCE = {"gnome": "GNOME (GPL-2.0+/GPL-3.0+/LGPL-2.1+, per module)", "kde": "KDE (GPL-2.0+/LGPL-2.0+)",
                  "lo": "LibreOffice (MPL-2.0)", "moz": "Mozilla Firefox (MPL-2.0)"}


def key(s):
    s = s.replace("’", "'").replace("‘", "'")
    return re.sub(r"\s+", " ", s).strip().lower()


def nfc(s):
    return unicodedata.normalize("NFKC", s).strip()


def load_stop():
    out = set()
    for l in open(os.path.join(HERE, "stoplist_en.txt"), encoding="utf-8"):
        l = l.split("#")[0].strip()
        if l:
            out.add(key(l))
    return out


# ---------------------------------------------------------------- po / ftl
def po_entries(path):
    """(msgctxt, msgid, msgstr) of translated, non-fuzzy, non-plural, non-obsolete entries."""
    def unq(x):
        x = x.strip()
        if x.startswith('"') and x.endswith('"'):
            x = x[1:-1]
        return x.replace('\\"', '"').replace("\\n", "\n").replace("\\t", "\t").replace("\\\\", "\\")

    out = []
    text = open(path, encoding="utf-8", errors="replace").read().replace("\r\n", "\n")
    for block in re.split(r"\n\s*\n", text):
        fuzzy, cur, field = False, {}, None
        for ln in block.split("\n"):
            if ln.startswith("#~"):
                cur = None
                break
            if ln.startswith("#,") and "fuzzy" in ln:
                fuzzy = True
            if ln.startswith("#") or not ln.strip():
                continue
            m = re.match(r"(msgctxt|msgid_plural|msgid|msgstr(?:\[\d+\])?)\s+(.*)", ln)
            if m:
                field = m.group(1)
                cur[field] = unq(m.group(2))
            elif ln.lstrip().startswith('"') and field:
                cur[field] += unq(ln)
        if not cur or fuzzy or "msgid_plural" in cur or not cur.get("msgid") or not cur.get("msgstr"):
            continue
        out.append((cur.get("msgctxt", ""), cur["msgid"], cur["msgstr"]))
    return out


def ftl_entries(path):
    """id(.attr) -> single-line value."""
    out = {}
    cur = None
    for ln in open(path, encoding="utf-8", errors="replace"):
        ln = ln.rstrip("\n")
        m = re.match(r"^([A-Za-z][\w-]*)\s*=\s*(.*)$", ln)
        if m:
            cur = m.group(1)
            if m.group(2).strip():
                out[cur] = m.group(2).strip()
            continue
        m = re.match(r"^\s+\.([\w-]+)\s*=\s*(.*)$", ln)
        if m and cur:
            if m.group(2).strip():
                out[cur + "." + m.group(1)] = m.group(2).strip()
            continue
        if ln.startswith((" ", "\t")) and ln.strip() and cur:  # a multi-line value: not a short label
            out.pop(cur, None)
    return out


MSGID_OK = re.compile(r"^[A-Za-z][A-Za-z' &-]*[A-Za-z]$")


def norm_msgid(s):
    s = s.replace("&&", "\x00").replace("&", "").replace("\x00", "&").replace("_", "").replace("~", "")
    s = s.strip()
    s = re.sub(r"(\.\.\.|…|:)$", "", s).strip()
    return s


def norm_msgstr(s):
    s = nfc(s)
    s = re.sub(r"[(（]\s*[_&~]?[A-Za-z0-9]\s*[)）]", "", s)  # 儲存(_S), 開啟(&O)
    s = s.replace("&&", "\x00").replace("&", "").replace("\x00", "&").replace("_", "").replace("~", "")
    s = re.sub(r"(\.\.\.|…|:|：)+$", "", s).strip()
    s = re.sub(r"(?<=[^\x00-\x7f])\s+(?=[^\x00-\x7f])", "", s)
    return s


def l10n_vote(stop, rejected, stats):
    projects = []  # (family, project, [(msgid, msgstr)])
    for fam in ("gnome", "kde", "lo"):
        for d in sorted(glob.glob(os.path.join(RAW, fam, "*"))):
            pairs = []
            for p in glob.glob(os.path.join(d, "*.po")):
                pairs += [(i, s) for _, i, s in po_entries(p)]
            if pairs:
                projects.append((fam, os.path.basename(d), pairs))
    lo = [p for p in projects if p[0] == "lo"]  # LibreOffice: one team, one project (its modules would outvote the others)
    projects = [p for p in projects if p[0] != "lo"]
    if lo:
        projects.append(("lo", "libreoffice", [x for p in lo for x in p[2]]))
    en_dir, tw_dir = os.path.join(RAW, "moz", "en-US"), os.path.join(RAW, "moz", "zh-TW")
    moz = []
    for p in glob.glob(os.path.join(tw_dir, "**", "*.ftl"), recursive=True):
        q = os.path.join(en_dir, os.path.relpath(p, tw_dir))
        if os.path.exists(q):
            a, b = ftl_entries(q), ftl_entries(p)
            moz += [(a[k], b[k]) for k in a if k in b]
    if moz:
        projects.append(("moz", "firefox", moz))
    stats["l10n_projects"] = len(projects)
    stats["l10n_pairs"] = sum(len(p[2]) for p in projects)
    votes = collections.defaultdict(list)  # key -> [(family, project, msgstr, n_this, n_total, display msgid)]
    for fam, proj, pairs in projects:
        per = collections.defaultdict(collections.Counter)
        disp = {}
        for i, s in pairs:
            if "%" in i or "{" in i or "<" in i or "\n" in i or "$" in i or "=" in i:
                continue
            mi = norm_msgid(i)
            if not MSGID_OK.match(mi) or len(mi.split()) > 4 or len(mi) < 2:
                continue
            ms = norm_msgstr(s)
            if not ms or not HAN.search(ms) or ASCII_LETTER.search(ms) or len(ms) > 4 * len(mi) + 4:
                continue
            k = key(mi)
            per[k][ms] += 1
            disp.setdefault(k, collections.Counter())[mi] += 1
        for k, c in per.items():
            top, n = c.most_common(1)[0]
            if len(c) > 1 and c.most_common(2)[1][1] == n:  # a tie inside a project: it abstains
                continue
            votes[k].append((fam, proj, top, n, sum(c.values()), disp[k].most_common(1)[0][0]))
    rows = []
    for k, vs in votes.items():
        fams = {v[0] for v in vs}
        c = collections.Counter(v[2] for v in vs)
        top, n = c.most_common(1)[0]
        disp = collections.Counter(v[5] for v in vs).most_common(1)[0][0]
        why = None
        if k in stop:
            why = "stoplist"
        elif len(vs) < 3 or len(fams) < 2:
            why = "too-few"
        elif n / len(vs) < 0.75:
            why = "conflict"
        else:
            for f in fams:
                fc = collections.Counter(v[2] for v in vs if v[0] == f)
                if fc.most_common(1)[0][0] != top:
                    why = "conflict-family"
                    break
        if why:
            stats["rej_l10n_" + why] += 1
            if why != "too-few":
                rejected.append(("en", disp, " | ".join(f"{z}:{m}" for z, m in c.most_common(4)), "ui", why,
                                 f"l10n:{len(vs)}p/{len(fams)}f"))
            continue
        conf = "A" if len(fams) >= 3 and len(vs) >= 6 else "B"
        src = "l10n:" + "+".join(sorted(fams)) + f":{n}/{len(vs)}"
        rows.append((disp, top, "x", "ui", conf, src))
    return rows


# ---------------------------------------------------------------- curated + generated
def curated(no_eval, stats):
    rows = []
    for ln in open(os.path.join(HERE, "curated_en.tsv"), encoding="utf-8"):
        if ln.startswith("#") or not ln.strip():
            continue
        c = ln.rstrip("\n").split("\t")
        src, zh, kind = c[0], c[1], c[2]
        mode = c[3] if len(c) > 3 and c[3] else ("x" if " " not in src else "s")
        note = c[4] if len(c) > 4 else ""
        if note == "eval":
            stats["curated_eval_noted"] += 1
            if no_eval:
                continue
        rows.append((src, zh, mode, kind, "A", "curated:eval" if note == "eval" else "curated"))
    # Numbered transit signs.
    for n in range(1, 21):
        rows.append((f"Platform {n}", f"第{n}月台", "s", "transit", "A", "curated:gen"))
        rows.append((f"Exit {n}", f"{n}號出口", "s", "transit", "A", "curated:gen"))
    for n in range(1, 10):
        rows.append((f"Transfer to Line {n}", f"轉乘{n}號線", "s", "transit", "A", "curated:gen"))
        rows.append((f"Line {n} Platform", f"{n}號線月台", "s", "transit", "A", "curated:gen"))
        rows.append((f"To Line {n}", f"往{n}號線", "x", "transit", "A", "curated:gen"))
    for n in range(1, 6):
        rows.append((f"Terminal {n}", f"第{n}航廈", "x", "transit", "A", "curated:gen"))
    for c, z in (("Japan", "日本"), ("Korea", "韓國"), ("South Korea", "韓國"), ("China", "中國"), ("Taiwan", "台灣"),
                 ("the USA", "美國"), ("USA", "美國"), ("the U.S.A.", "美國"), ("Thailand", "泰國"), ("Vietnam", "越南"),
                 ("Italy", "義大利"), ("France", "法國"), ("Germany", "德國"), ("Spain", "西班牙"), ("Malaysia", "馬來西亞"),
                 ("Indonesia", "印尼"), ("the Philippines", "菲律賓"), ("Australia", "澳洲"), ("New Zealand", "紐西蘭"),
                 ("the UK", "英國"), ("Canada", "加拿大"), ("Mexico", "墨西哥"), ("India", "印度")):
        rows.append((f"Made in {c}", f"{z}製造", "s", "label", "A", "curated:gen"))
        rows.append((f"Product of {c}", f"{z}產品", "s", "label", "A", "curated:gen"))
    return rows


# ---------------------------------------------------------------- Wikidata
def wikidata(stop, rejected, stats):
    p = os.path.join(HERE, "wikidata_en.jsonl")
    if not os.path.exists(p):
        return []
    cands = collections.defaultdict(list)
    for ln in open(p, encoding="utf-8"):
        it = json.loads(ln)
        en, zh0 = nfc(it["en"]), nfc(it["tw"])
        zh0 = re.sub(r"\s*[（(][^）)]*[）)]\s*$", "", zh0).strip()
        zh = S2T.convert(zh0) if S2T else zh0  # a few zh-tw labels are written with simplified characters (洋蔥酱咖喱)
        kind = "dish" if "dish" in it["kinds"] else "drink" if "drink" in it["kinds"] else "food"
        why = None
        if not re.match(r"^[A-Za-z][A-Za-z' -]*[A-Za-z]$", en) or len(en.split()) > 4:
            why = "src"
        elif not re.fullmatch(r"[㐀-䶿一-鿿·]+", zh) or len(zh) > 12:
            why = "zh"
        elif re.search(r"(屬|科|目|綱|門|族)$", zh):
            why = "taxon"
        elif re.search(r"( dish| as food| beverage| product| products| plant)$", en, re.I):
            why = "category"
        elif key(en) in stop:
            why = "stoplist"
        if why:
            stats["rej_wd_" + why] += 1
            rejected.append(("en", en, zh, kind, why, "wikidata:" + it["qid"]))
            continue
        cands[key(en)].append((en, zh, kind, it["qid"]))
    rows = []
    for k, cs in cands.items():
        zhs = {c[1] for c in cs}
        if len(zhs) > 1:
            stats["rej_wd_ambiguous"] += 1
            rejected.append(("en", cs[0][0], " | ".join(sorted(zhs)), cs[0][2], "ambiguous", "wikidata:" + ",".join(c[3] for c in cs)))
            continue
        en, zh, kind, qid = cs[0]
        mode = "x" if " " not in en.strip() else "s"
        rows.append((en, zh, mode, kind, "A", f"wikidata:{qid}:zh-tw"))
    return rows


def main():
    no_eval = "--no-eval" in sys.argv
    stats = collections.Counter()
    rejected = []
    stop = load_stop()
    groups = [("curated", curated(no_eval, stats)), ("l10n", l10n_vote(stop, rejected, stats)), ("wikidata", wikidata(stop, rejected, stats))]
    seen = {}
    out = []
    for name, rows in groups:
        for r in rows:
            src, zh, mode, kind, conf, source = r
            k = key(src)
            if len(k) < 2:
                continue
            if k in seen:
                if seen[k][1] != zh:
                    stats[f"{name}_overridden"] += 1
                    if name != "curated":
                        rejected.append(("en", src, zh, kind, f"overridden-by-{seen[k][5].split(':')[0]}", source))
                continue
            if NEG.search(src) and mode != "x":
                mode = "x"
                stats["neg_forced_x"] += 1
            r = (src, zh, mode, kind, conf, source)
            seen[k] = r
            out.append(r)
    out.sort(key=lambda r: (r[4], 0 if r[5].startswith("curated") else 1 if r[5].startswith("l10n") else 2, KIND_PRI.get(r[3], 9), key(r[0])))
    name = "glossary_en_noeval.tsv" if no_eval else "glossary_en.tsv"
    with open(os.path.join(HERE, name), "w", encoding="utf-8", newline="\n") as f:
        f.write("# en -> zh-Hant (Taiwan). Built by build_glossary_en.py; sources and licences: SOURCES.md. Keys: any case.\n")
        f.write("src\tzh\tmode\tkind\tconf\tsource\n")
        for r in out:
            f.write("\t".join(r) + "\n")
    if no_eval:
        print(len(out), "rows ->", name)
        return
    with open(os.path.join(HERE, "rejected_en.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("lang\tsrc\tzh\tkind\twhy\tsource\n")
        for r in rejected:
            f.write("\t".join(r) + "\n")
    size = os.path.getsize(os.path.join(HERE, name))
    by = lambda i: dict(collections.Counter(r[i] for r in out).most_common())  # noqa: E731
    src_by = dict(collections.Counter(r[5].split(":")[0] + (":" + r[5].split(":")[1] if r[5].startswith("curated:") else "") for r in out))
    st = {"entries": len(out), "bytes": size, "mode": by(2), "kind": by(3), "conf": by(4), "source": src_by,
          "counts": dict(stats)}
    lines = [f"en: {len(out)} entries, {size / 1024:.0f} KiB; mode {st['mode']}; conf {st['conf']}",
             f"    kind {st['kind']}", f"    source {src_by}",
             f"    l10n: {stats['l10n_projects']} projects, {stats['l10n_pairs']} msgid/msgstr pairs",
             f"    rejected / merged: { {k: v for k, v in stats.items() if k.startswith('rej_') or 'overrid' in k or k == 'neg_forced_x'} }"]
    open(os.path.join(HERE, "stats_en.txt"), "w", encoding="utf-8").write("\n".join(lines) + "\n")
    json.dump(st, open(os.path.join(HERE, "stats_en.json"), "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print("\n".join(lines))


if __name__ == "__main__":
    main()
