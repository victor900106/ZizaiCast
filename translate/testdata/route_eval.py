# Routing policies for the local LLM, simulated on pm_translate_test --eval
# runs made with PM_EVAL_ALT=1 (every ja / ko block has "tx" = the pipeline's
# translation and "alt" = the same block with the LLM's translation of every
# engine piece, "alt_ms" its LLM time, "alt_flags" the checks it fails).
#
#   py -3 translate/testdata/route_eval.py --img IMGDIR --spec SPECDIR RUN.jsonl [--policy NAME] [-v]
#
# Per policy: units fixed / broken against the pipeline (tx everywhere),
# wrong units, negation / number key errors, LLM time per picture (p50 / p95,
# pipeline time included).  The policies are the ones translator.cpp can run
# (same features): see POLICIES.
import argparse
import copy
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import eval_metrics as em  # noqa: E402

NUMERIC = em.NUMERIC


def chrf(a, b, n=3, beta=2.0):
    """chrF (character n-grams 1..n, recall-weighted) of hypothesis a against b; 0..1."""
    a = "".join(c for c in a if not c.isspace())
    b = "".join(c for c in b if not c.isspace())
    if not a or not b:
        return 0.0
    ps, rs = [], []
    for k in range(1, n + 1):
        ga, gb = {}, {}
        for i in range(len(a) - k + 1):
            ga[a[i:i + k]] = ga.get(a[i:i + k], 0) + 1
        for i in range(len(b) - k + 1):
            gb[b[i:i + k]] = gb.get(b[i:i + k], 0) + 1
        m = sum(min(v, gb.get(g, 0)) for g, v in ga.items())
        if ga:
            ps.append(m / sum(ga.values()))
        if gb:
            rs.append(m / sum(gb.values()))
    p = sum(ps) / max(1, len(ps))
    r = sum(rs) / max(1, len(rs))
    if p + r == 0:
        return 0.0
    return (1 + beta * beta) * p * r / (beta * beta * p + r)


def body(tx):
    """The translation without the verified-facts line."""
    return tx.split("\n⚠")[0]


# Characters Chinese uses to spell foreign sounds: a run of them in a pivot
# output is usually a transliterated word the engine did not know (雅基托里,
# 美索江) - the cheap "garbage" signal.
TRANSLIT = set("阿埃艾安奧巴拜班邦比彼波伯布查達戴丹德迪蒂多杜爾法菲弗福伽蓋岡戈格古哈漢赫霍吉基加賈傑卡凱坎科克庫拉萊蘭勞雷里利林隆魯羅洛馬邁曼梅蒙米密莫姆納奈南尼諾帕佩皮普奇喬切薩塞桑瑟沙舍斯蘇索塔泰坦特提托圖瓦韋維溫沃西希謝辛雅亞楊伊因尤約扎贊澤茲祖佐")


def translit_run(t):
    best = cur = 0
    for c in t:
        cur = cur + 1 if c in TRANSLIT else 0
        best = max(best, cur)
    return best


def kept_runs(src, tx):
    """Runs of 2+ characters the translation copied from the source (names, kanji
    terms: 美十, 八橋, GODIVA): the LLM must keep them too."""
    out = []
    s = "".join(c for c in src if not c.isspace())
    t = "".join(c for c in tx if not c.isspace())
    i = 0
    while i < len(t):
        k = 0
        while i + k < len(t) and t[i:i + k + 1] in s:
            k += 1
        if k >= 2 and not re.fullmatch(r"[0-9.,%()（）:：/~\-]+", t[i:i + k]):
            out.append(t[i:i + k])
            i += k
        else:
            i += 1
    return out


def guard(b, tx, alt):
    """Reasons the LLM's text must not replace the pipeline's (cheap, rule-only)."""
    why = []
    if re.search(r"[぀-ヿ가-힯]", alt):
        why.append("script")              # kana / hangul left (an untranslated part)
    t = re.sub(r"\s", "", tx)
    a = re.sub(r"\s", "", alt)
    if len(a) < 0.6 * len(t):
        why.append("shorter")             # content dropped (帳戶設定 -> 設定)
    for r in kept_runs(b["t"], tx):
        if r not in a and r.lower() not in a.lower():
            why.append("names")           # a name / copied term changed (美十 -> Mitsui)
            break
    return why


def features(b, src_lang):
    tx, alt = body(b["tx"]), b.get("alt", "")
    return {
        "guard": guard(b, tx, alt) if alt else [],
        "has_alt": bool(alt) and b.get("eng", "") != "",
        "agree": chrf(tx, alt) if alt else 1.0,
        "alt_ok": not b.get("alt_flags", "").strip(),
        "uncertain": b.get("uncertain", False),
        "translit": translit_run(tx),
        "alt_translit": translit_run(alt),
        "src_len": len(re.sub(r"\s", "", b["t"])),
        "ratio": len(tx) / max(1, len(re.sub(r"\s", "", b["t"]))),
        "lang": src_lang,
    }


def pick_alt(f, name, th):
    """True: the LLM's text replaces the pipeline's for this block."""
    if not f["has_alt"] or not f["alt_ok"]:
        return False
    if name == "pipeline":
        return False
    if name.endswith("+g"):
        if f["guard"]:
            return False
        name = name[:-2]
    if name == "llm-all":
        return True
    if name == "disagree":           # (a) both run, LLM where they differ
        return f["agree"] < th
    if name == "translit":           # (c) transliteration garbage only
        return f["translit"] >= 3 and f["alt_translit"] < f["translit"]
    if name == "short":              # (c) short items (menu / sign) only
        return f["src_len"] <= th
    if name == "disagree+translit":
        return f["agree"] < th or (f["translit"] >= 3 and f["alt_translit"] < f["translit"])
    if name == "uncertain":          # LLM only where the checks still fail
        return f["uncertain"]
    raise ValueError(name)


def cost_needs_all(name):
    if name.endswith("+g"):
        name = name[:-2]
    return name in ("llm-all", "disagree", "disagree+translit")


def lang_of(b):
    l = b.get("lang", "")
    return l


def apply(run, name, th):
    r = copy.deepcopy(run)
    llm_ms = 0
    routed = 0
    for b in r["blocks"]:
        if not b.get("alt") or not b.get("eng"):
            continue
        f = features(b, lang_of(b))
        if cost_needs_all(name) or pick_alt(f, name, th):
            llm_ms += b.get("alt_ms", 0)
        if pick_alt(f, name, th):
            routed += 1
            b["tx"] = b["alt"]  # passes the checks (alt_ok): no facts line
    return r, llm_ms, routed


def score(runs, specs, gts, name, th):
    units = {}
    tot = {"wrong": 0, "neg": 0, "num": 0, "routed": 0}
    times = []
    for run in runs:
        nm = run["image"]
        if nm not in specs:
            continue
        r, ms, routed = apply(run, name, th)
        base_llm = sum(b.get("alt_ms", 0) for b in run["blocks"] if b.get("alt"))
        pipe_ms = max(0, run.get("tr_ms", 0) - base_llm)
        times.append(pipe_ms + ms)
        tot["routed"] += routed
        for u in em.score_image(specs[nm], gts[nm], r):
            if "error" in u or u["optional"]:
                continue
            ok = u["covered"] >= 0.3 and not u["wrong"]
            units[(nm, u["id"])] = (ok, u)
            if u["wrong"]:
                tot["wrong"] += 1
            for t in u["bad_key_types"]:
                if t == "neg":
                    tot["neg"] += 1
                elif t in NUMERIC:
                    tot["num"] += 1
    times.sort()
    p = lambda q: times[min(len(times) - 1, int(q * len(times)))] if times else 0
    return units, tot, p(0.5), p(0.95)


POLICIES = [("pipeline", 0), ("llm-all", 0), ("llm-all+g", 0), ("short+g", 10), ("disagree+g", 0.3), ("disagree+g", 0.5), ("uncertain", 0), ("translit", 0), ("short", 6), ("short", 10),
            ("disagree", 0.2), ("disagree", 0.3), ("disagree", 0.4), ("disagree", 0.5), ("disagree+translit", 0.3)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", required=True)
    ap.add_argument("--spec", default=os.path.join(HERE, "eval"))
    ap.add_argument("--policy")
    ap.add_argument("--th", type=float, default=0.3)
    ap.add_argument("-v", action="store_true")
    ap.add_argument("--budget", action="store_true")
    ap.add_argument("--target", type=float, default=2500)
    ap.add_argument("--speed", type=float, default=1.0)
    ap.add_argument("--ocr", action="append")
    ap.add_argument("runs", nargs="+")
    a = ap.parse_args()
    runs = [json.loads(l) for f in a.runs for l in open(f, encoding="utf-8") if l.strip()]
    specs, gts = {}, {}
    for run in runs:
        sp = os.path.join(a.spec, run["image"] + ".ref.json")
        if os.path.exists(sp):
            specs[run["image"]] = json.load(open(sp, encoding="utf-8"))
            gts[run["image"]] = em.load_gt(os.path.join(a.img, run["image"] + ".gt.tsv"))
    if a.budget:
        budget_main(a, runs, specs, gts)
        return
    base, btot, _, _ = score(runs, specs, gts, "pipeline", 0)
    pols = [(a.policy, a.th)] if a.policy else POLICIES
    print("%-22s %6s %6s %6s %6s %5s %5s %5s %7s %8s %8s" % ("policy", "routed", "fixed", "broken", "net", "ok", "wrong", "neg",
                                                           "num", "p50 ms", "p95 ms"))
    for name, th in pols:
        units, tot, p50, p95 = score(runs, specs, gts, name, th)
        fixed = [k for k, (ok, u) in units.items() if ok and k in base and not base[k][0]]
        broken = [k for k, (ok, u) in units.items() if not ok and k in base and base[k][0]]
        label = name + ("@%g" % th if th else "")
        nok = sum(1 for ok, u in units.values() if ok)
        print("%-22s %6d %6d %6d %+6d %5d %5d %5d %7d %8.0f %8.0f" % (label, tot["routed"], len(fixed), len(broken),
                                                                   len(fixed) - len(broken), nok, tot["wrong"], tot["neg"],
                                                                   tot["num"], p50, p95))
        if a.v:
            for k in fixed:
                print("   + %s %s: %s" % (k[0], k[1], units[k][1]["tx"][:60].replace("\n", " | ")))
            for k in broken:
                print("   - %s %s: %s  (was %s)" % (k[0], k[1], units[k][1]["tx"][:60].replace("\n", " | "),
                                                    base[k][1]["tx"][:40].replace("\n", " | ")))




# ---- Budgeted policies (the owner's 2.5 s target / 3 s cap per picture) ----
# python route_eval.py --budget [--ocr OCRRUN.jsonl] [--speed 0.2] RUN.jsonl ...
# Per picture: available = target - OCR - pipeline; candidate blocks by class
# order (U uncertain, T transliteration garbage, S<n> source of <= n
# characters, shortest first), each costing its measured LLM time x speed,
# taken while they fit.  speed 1 = this CPU (Qwen3.5-2B Q4, 4 threads),
# 0.2 = a GPU / batched backend 5x faster (assumption until measured).
def budget_apply(run, classes, accept, ocr_ms, target, speed):
    r = copy.deepcopy(run)
    base_llm = sum(b.get("alt_ms", 0) for b in run["blocks"] if b.get("alt"))
    pipe = max(0, run.get("tr_ms", 0) - base_llm)
    avail = target - ocr_ms - pipe
    cands = []
    for k, b in enumerate(r["blocks"]):
        if not b.get("alt") or not b.get("eng"):
            continue
        f = features(b, b.get("lang", ""))
        for ci, c in enumerate(classes):
            hit = (c == "U" and f["uncertain"]) or (c == "T" and f["translit"] >= 3) or \
                  (c.startswith("S") and f["src_len"] <= int(c[1:]))
            if hit:
                cands.append((ci, f["src_len"], k, f))
                break
    cands.sort()
    used = 0
    routed = 0
    for ci, ln, k, f in cands:
        b = r["blocks"][k]
        cost = b.get("alt_ms", 0) * speed
        if used + cost > avail:
            continue
        used += cost
        if not f["alt_ok"]:
            continue
        if "script" in accept and "script" in f["guard"]:
            continue
        if "shorter" in accept and "shorter" in f["guard"]:
            continue
        if "names" in accept and "names" in f["guard"]:
            continue
        if classes[ci] == "T" and f["alt_translit"] >= f["translit"]:
            continue
        b["tx"] = b["alt"]
        routed += 1
    return r, ocr_ms + pipe + used, routed


def budget_main(a, runs, specs, gts):
    ocr = {}
    for f in a.ocr or []:
        for l in open(f, encoding="utf-8"):
            if l.strip():
                j = json.loads(l)
                ocr[j["image"]] = j.get("ocr_ms", 0)
    base, _, _, _ = score(runs, specs, gts, "pipeline", 0)
    print("%-26s %6s %6s %6s %6s %5s %5s %5s %8s %8s" % ("policy (budget %d ms, speed %g)" % (a.target, a.speed), "routed", "fixed",
                                                         "broken", "net", "ok", "neg", "num", "p50 ms", "p95 ms"))
    for classes, accept in [((), ()), (("U",), ("script",)), (("U", "T"), ("script",)), (("U", "T", "S6"), ("script",)),
                            (("U", "T", "S10"), ("script",)), (("U", "T", "S10"), ("script", "shorter")),
                            (("U", "T", "S10"), ("script", "shorter", "names")), (("U", "S10"), ("script",)),
                            (("U", "T", "S20"), ("script",)), (("U", "T", "S999"), ("script",))]:
        units, times, nroute, neg, num = {}, [], 0, 0, 0
        for run in runs:
            nm = run["image"]
            if nm not in specs:
                continue
            r, ms, routed = budget_apply(run, list(classes), accept, ocr.get(nm, run.get("ocr_ms", 0)), a.target, a.speed)
            times.append(ms)
            nroute += routed
            for u in em.score_image(specs[nm], gts[nm], r):
                if "error" in u or u["optional"]:
                    continue
                units[(nm, u["id"])] = (u["covered"] >= 0.3 and not u["wrong"], u)
                neg += sum(1 for t in u["bad_key_types"] if t == "neg")
                num += sum(1 for t in u["bad_key_types"] if t in NUMERIC)
        times.sort()
        p = lambda q: times[min(len(times) - 1, int(q * len(times)))] if times else 0
        fixed = sum(1 for k, (ok, u) in units.items() if ok and not base[k][0])
        broken = sum(1 for k, (ok, u) in units.items() if not ok and base[k][0])
        label = "+".join(classes) or "none"
        label += " [" + ",".join(accept) + "]" if accept else ""
        print("%-26s %6d %6d %6d %+6d %5d %5d %5d %8.0f %8.0f" % (label, nroute, fixed, broken, fixed - broken,
                                                               sum(1 for ok, u in units.values() if ok), neg, num, p(0.5), p(0.95)))


if __name__ == "__main__":
    main()
