# Quality of the local LLM engine (translate/src/llm_engine.h) on the
# translation evaluation units (translate/testdata/eval/*.ref.json, the same
# must groups / keys as eval_metrics.py) + translate/tools/llm_eval_extra.ref.json.
#
#   py -3 translate/tools/llm_eval.py make SEGMENTS.tsv [--refs DIR]
#   pm_llm_eval --model qwen3.5-2b-q4km --in SEGMENTS.tsv --out RUN.jsonl [--pictures -1] [--ctx]
#   py -3 translate/tools/llm_eval.py score RUN.jsonl [RUN2.jsonl ...] [--refs DIR] [--by-lang] [-v]
#
# --fallback RUN: the text of another run (Bergamot's, pm_translate_test
# --text) where this one has none - what the app would show.
# --refs DIR: another set of NAME.ref.json (e.g. launch/_work/eval_web/ref,
# the real-world pictures); the segments are the units' source text (what a
# perfect segmentation of the ground-truth lines gives), one picture = one
# file (ids NAME:unit, so --pictures -1 translates a picture per call).
# Per run: units passing (every must group + every key), negation keys kept,
# number keys kept, chrF against the reference, speed / memory from the
# summary; --by-lang: the same per source language, and the per-picture
# latency (pic_ms) p50 / p95.
import glob
import json
import os
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
TESTDATA = os.path.join(HERE, "..", "testdata")
sys.path.insert(0, TESTDATA)
from eval_metrics import key_ok, norm_tx, unit_pass  # noqa: E402

NUMERIC = {"num", "unit", "date", "phone", "price", "temp", "time", "pct"}


REFS = None
FALLBACK = None  # --fallback RUN.jsonl: its text where this run has none (e.g. Bergamot's)


def specs():
    if REFS:
        files = sorted(glob.glob(os.path.join(REFS, "*.ref.json")))
    else:
        files = sorted(glob.glob(os.path.join(TESTDATA, "eval", "*.ref.json"))) + [os.path.join(HERE, "llm_eval_extra.ref.json")]
    for f in files:
        d = json.load(open(f, encoding="utf-8"))
        prev = ""
        for u in d["units"]:
            lang = u.get("lang", d.get("lang")) or os.path.basename(f).split("_")[0]
            yield d["name"] + ":" + u["id"], lang, u, prev
            prev = u["src"]


def esc(s):
    return s.replace("\t", " ").replace("\r", "").replace("\n", "\\n")


def make(out):
    n = 0
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        for uid, lang, u, prev in specs():
            if lang not in ("ja", "ko", "en", "zh-Hans"):
                continue
            f.write("\t".join([uid, lang, esc(u["src"]), esc(prev), ""]) + "\n")
            n += 1
    print(f"{n} segments -> {out}")


def chrf(hyp, ref, n=3, beta=2.0):
    h, r = norm_tx(hyp), norm_tx(ref)
    if not h or not r:
        return 0.0
    ps, rs = [], []
    for k in range(1, n + 1):
        hc = Counter(h[i:i + k] for i in range(len(h) - k + 1))
        rc = Counter(r[i:i + k] for i in range(len(r) - k + 1))
        m = sum((hc & rc).values())
        if hc:
            ps.append(m / sum(hc.values()))
        if rc:
            rs.append(m / sum(rc.values()))
    p = sum(ps) / len(ps) if ps else 0
    rr = sum(rs) / len(rs) if rs else 0
    return 0.0 if p + rr == 0 else (1 + beta ** 2) * p * rr / (beta ** 2 * p + rr)


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * q))] if v else 0


def score(run, verbose=False, by_lang=False):
    units = {uid: u for uid, _, u, _ in specs()}
    res, summary = [], {}
    fb = {}
    if FALLBACK:
        for ln in open(FALLBACK, encoding="utf-8", errors="replace"):
            d = json.loads(ln)
            if "id" in d:
                fb[d["id"]] = d["out"]
    for ln in open(run, encoding="utf-8", errors="replace"):
        d = json.loads(ln)
        if d.get("summary"):
            summary = d
        else:
            if not d["out"] and d["id"] in fb:
                d["out"] = fb[d["id"]]  # no LLM answer (budget, untranslated): what the app shows instead
            res.append(d)
    ok = neg = negok = num = numok = 0
    cf = []
    fails = []
    for d in res:
        u = units.get(d["id"])
        if not u:
            continue
        tx = d["out"]
        mm, bk = unit_pass(u, tx)
        if not mm and not bk:
            ok += 1
        else:
            fails.append((d["id"], u["src"], tx, mm, [k["type"] + ":" + k["src"] for k in bk]))
        for k in u.get("keys", []):
            if k["type"] == "neg":
                neg += 1
                negok += key_ok(k, tx)
            elif k["type"] in NUMERIC:
                num += 1
                numok += key_ok(k, tx)
        if u.get("ref"):
            cf.append(chrf(tx, u["ref"]))
    n = len(res)
    s = summary
    print(f"{os.path.basename(run)}: units {ok}/{n} pass ({100 * ok / max(n, 1):.0f}%), negation {negok}/{neg}, "
          f"numbers {numok}/{num}, chrF {100 * sum(cf) / max(len(cf), 1):.1f}; "
          f"load {s.get('load_ms', 0) / 1000:.1f} s, mean {s.get('mean_ms', 0):.0f} ms, p95 {s.get('p95_ms', 0):.0f} ms, "
          f"peak {s.get('peak_ws_mb', 0):.0f} MB")
    if by_lang:
        langs = {uid: lang for uid, lang, _, _ in specs()}
        for L in sorted(set(langs.get(d["id"], "?") for d in res)):
            rs = [d for d in res if langs.get(d["id"]) == L and d["id"] in units]
            okL = sum(1 for d in rs if not any(unit_pass(units[d["id"]], d["out"])))
            pics = {}
            for d in rs:
                pics[d["id"].split(":")[0]] = d.get("pic_ms", d.get("ms", 0))
            pv = list(pics.values())
            print(f"    {L}: units {okL}/{len(rs)} pass ({100 * okL / max(len(rs), 1):.0f}%), wrong {len(rs) - okL}; "
                  f"{len(pv)} pictures, per picture p50 {pct(pv, 0.5):.0f} ms p95 {pct(pv, 0.95):.0f} ms max {max(pv) if pv else 0:.0f} ms")
    if verbose:
        for uid, src, tx, mm, bk in fails:
            print(f"  FAIL {uid}: {src}\n       -> {tx}\n       must {mm} keys {bk}")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    args = sys.argv[1:]
    if "--refs" in args:
        k = args.index("--refs")
        REFS = args[k + 1]
        del args[k:k + 2]
    if "--fallback" in args:
        k = args.index("--fallback")
        FALLBACK = args[k + 1]
        del args[k:k + 2]
    v = "-v" in args
    bl = "--by-lang" in args
    args = [x for x in args if x not in ("-v", "--by-lang")]
    if len(args) >= 2 and args[0] == "make":
        make(args[1])
    elif len(args) >= 2 and args[0] == "score":
        for r in args[1:]:
            score(r, v, bl)
    else:
        print(__doc__ if __doc__ else "usage: llm_eval.py make OUT.tsv | score RUN.jsonl... [-v]")
        sys.exit(2)
