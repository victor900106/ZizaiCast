# Scores pm_translate_test --eval output against the reference specs
# (translate/testdata/eval/NAME.ref.json) and attributes every error to a layer.
#
#   py -3 translate/testdata/eval_metrics.py --img IMGDIR --run OCR.jsonl [--gtrun GT.jsonl] [--json OUT.json] [-v] [--gate gt|ocr]
#
#   IMGDIR   the pictures + NAME.gt.tsv (make_eval_scenes.py output)
#   OCR.jsonl  pm_translate_test --eval OCR.jsonl IMGDIR/*.png              (the app's path)
#   GT.jsonl   pm_translate_test --eval GT.jsonl --gt IMGDIR/*.png         (same, ground-truth lines)
#
# A unit (spec) is a table row / sentence / menu item / UI line.  Per unit:
#   covered  the share of its characters inside blocks that got a translation
#            (block boxes intersecting the unit's ground-truth boxes, text aligned)
#   omitted  characters not covered -> 漏翻; layer:
#              OCR      the recogniser did not read them
#              分段     read, but the block was left out (pickBlocks: kanji only,
#                       one letter, number …) or grouped away
#              後處理   translated but the result was rejected (translatedOk) / dropped
#   wrong    covered, but a `must` group or a key is missing -> 錯譯; layer:
#              OCR      the same unit passes on ground-truth lines (GT run)
#              後處理   the engine alone on the block (raw) passes, the app's result not
#              引擎     otherwise
#   keys     neg (否定 / 禁止: a negation word, near its object when `obj` is
#            given, and none of the `bad` patterns), num / unit / date / phone /
#            price / temp / time / pct (the value as written in `any`).
import argparse
import difflib
import glob
import json
import os
import re
import sys
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
NUMERIC = {"num", "unit", "date", "phone", "price", "temp", "time", "pct"}


def squeeze(s):
    o = []
    for c in s:
        if c.isspace() or c == "　":
            continue
        n = ord(c)
        if 0xFF01 <= n <= 0xFF5E:
            c = chr(n - 0xFEE0)
        if c in "·・･":
            c = "•"
        o.append(c)
    return "".join(o)


def norm_tx(s):
    """Translation text for key matching: no spaces, half-width ASCII, ~ for 〜."""
    s = squeeze(s)
    return s.replace("〜", "~").replace("～", "~").replace("–", "-").replace("—", "-")


def load_gt(path):
    out = []
    for ln in open(path, encoding="utf-8"):
        ln = ln.rstrip("\n")
        if "\t" not in ln:
            continue
        nums, text = ln.split("\t", 1)
        v = [float(x) for x in nums.split()]
        out.append({"b": v[:4], "t": text})
    return out


def find_unit_lines(unit, gt, used):
    """GT line indices forming the unit: a run of consecutive lines whose texts make the
    unit's text; else the lines whose text occurs in it (nearest to each other)."""
    U = squeeze(unit["src"])
    for i in range(len(gt)):
        acc = ""
        for j in range(i, min(len(gt), i + 16)):
            acc += squeeze(gt[j]["t"])
            if not U.startswith(acc):
                break
            if acc == U:
                idx = list(range(i, j + 1))
                if not any(k in used for k in idx):
                    return idx
                break
    cand = [k for k, g in enumerate(gt) if squeeze(g["t"]) and squeeze(g["t"]) in U and k not in used]
    if not cand:
        cand = [k for k, g in enumerate(gt) if squeeze(g["t"]) and squeeze(g["t"]) in U]
    # Drop duplicates far away from the rest (the same word twice on a label).
    if len(cand) > 1:
        ys = sorted((gt[k]["b"][1] + gt[k]["b"][3]) / 2 for k in cand)
        med = ys[len(ys) // 2]
        cand = [k for k in cand if abs((gt[k]["b"][1] + gt[k]["b"][3]) / 2 - med) < 0.08]
    return cand


def inter(a, b):
    w = min(a[2], b[2]) - max(a[0], b[0])
    h = min(a[3], b[3]) - max(a[1], b[1])
    return max(0.0, w) * max(0.0, h)


def area(a):
    return max(1e-9, (a[2] - a[0]) * (a[3] - a[1]))


def touches(box, region_boxes):
    for r in region_boxes:
        if inter(box, r) > 0.3 * min(area(box), area(r)):
            return True
    return False


def mark(U, T, cov):
    """Marks the characters of U that T contains (aligned runs of 2+ chars)."""
    T = squeeze(T)
    if not T:
        return
    sm = difflib.SequenceMatcher(None, U, T, autojunk=False)
    for a, b, n in sm.get_matching_blocks():
        if n >= 2 or (n == 1 and len(T) <= 2):
            for k in range(a, a + n):
                cov[k] = True


def has_any(tx, words):
    return any(norm_tx(w) in tx for w in words if not w.startswith("@"))


def counted(c):
    """Characters that count for 漏翻: letters and digits (not ※ ● 、 。 brackets)."""
    return unicodedata.category(c)[0] in "LN"


def part_omitted(U, part, tr_cov):
    """The source part of a must group / key mostly not translated (then it is 漏翻, not 錯譯)."""
    p = squeeze(part)
    at = U.find(p)
    if at < 0 or not p:
        return False
    return sum(1 for k in range(at, at + len(p)) if not tr_cov[k]) * 2 > len(p)


def key_ok(key, tx):
    t = norm_tx(tx)
    if key["type"] == "neg":
        neg = [norm_tx(w) for w in key["any"]]
        hit = [m.start() for w in neg for m in re.finditer(re.escape(w), t)]
        if not hit:
            return False
        if key.get("obj"):
            objs = [m.start() for o in key["obj"] for m in re.finditer(re.escape(norm_tx(o)), t)]
            if objs and not any(abs(h - o) <= 12 for h in hit for o in objs):
                return False
        raw = "".join(c for c in tx if not c.isspace())  # keeps 「，」 (the patterns stop at clause ends)
        for bad in key.get("bad", []):
            if re.search(bad, raw):
                return False
        return True
    return has_any(t, key["any"])


def unit_pass(unit, tx, U=None, tr_cov=None):
    """Missing must groups and wrong keys; parts that were not translated at all are skipped
    (must group ["@原材料名", …]: about that source part; keys: their src)."""
    t = norm_tx(tx)
    miss_must, bad_keys = [], []
    for g in unit.get("must", []):
        if tr_cov is not None and g and g[0].startswith("@") and part_omitted(U, g[0][1:], tr_cov):
            continue
        if not has_any(t, g):
            miss_must.append(g)
    for k in unit.get("keys", []):
        if tr_cov is not None and part_omitted(U, k["src"], tr_cov):
            continue
        if not key_ok(k, tx):
            bad_keys.append(k)
    return miss_must, bad_keys


def score_image(spec, gt, run):
    """Per unit: coverage / omission layer / pass, using one --eval record."""
    blocks = run["blocks"]
    lines = run["lines"]
    used = set()
    res = []
    for u in spec["units"]:
        U = squeeze(u["src"])
        idx = find_unit_lines(u, gt, used)
        used.update(idx)
        region = [gt[k]["b"] for k in idx]
        if not region:
            res.append({"id": u["id"], "error": "unit not found in gt"})
            continue
        # Pad the region a little (OCR boxes are a bit larger / smaller).
        region = [[r[0] - 0.004, r[1] - 0.002, r[2] + 0.004, r[3] + 0.002] for r in region]
        n = len(U)
        ocr_cov = [False] * n
        for l in lines:
            if touches(l["b"], region):
                mark(U, l["t"], ocr_cov)
        tr_cov = [False] * n
        skip_cov = {}  # reason -> chars
        rej_cov = [False] * n
        txs = []
        raws = []
        for b in sorted(blocks, key=lambda b: (b["b"][1], b["b"][0])):
            if not touches(b["b"], region):
                continue
            c = [False] * n
            mark(U, b["t"], c)
            if not any(c):
                continue
            if b["ok"]:
                for k in range(n):
                    tr_cov[k] |= c[k]
                txs.append(b["tx"])
                raws.append(b["raw"])
            elif b["skip"]:
                skip_cov.setdefault(b["skip"], [False] * n)
                for k in range(n):
                    skip_cov[b["skip"]][k] |= c[k]
            else:
                for k in range(n):
                    rej_cov[k] |= c[k]
        omitted = [k for k in range(n) if not tr_cov[k] and counted(U[k])]
        layers = {"OCR": 0, "分段": 0, "後處理": 0}
        reasons = {}
        for k in omitted:
            if not ocr_cov[k]:
                layers["OCR"] += 1
            elif rej_cov[k]:
                layers["後處理"] += 1
            else:
                layers["分段"] += 1
                r = next((r for r, c in skip_cov.items() if c[k]), "grouped / unmatched")
                reasons[r] = reasons.get(r, 0) + 1
        tx = "\n".join(txs)
        nc = sum(1 for c in U if counted(c))
        covered = 1 - len(omitted) / max(1, nc)
        miss_must, bad_keys = unit_pass(u, tx, U, tr_cov) if covered >= 0.3 else ([], [])
        raw_miss, raw_bad = unit_pass(u, "\n".join(raws), U, tr_cov) if covered >= 0.3 else ([], [])
        key_lost = [k["src"] for k in u.get("keys", []) if covered < 0.3 or part_omitted(U, k["src"], tr_cov)]
        res.append({"id": u["id"], "src": u["src"], "optional": bool(u.get("optional")), "chars": nc, "key_lost": key_lost,
                    "omitted": len(omitted), "omit_layers": layers, "omit_reasons": reasons, "covered": covered,
                    "tx": tx, "raw": "\n".join(raws), "miss_must": miss_must, "bad_keys": [k["src"] for k in bad_keys],
                    "bad_key_types": [k["type"] for k in bad_keys],
                    "raw_pass": covered >= 0.3 and not raw_miss and not raw_bad,
                    "wrong": covered >= 0.3 and bool(miss_must or bad_keys),
                    "keys": u.get("keys", [])})
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", required=True)
    ap.add_argument("--run", required=True)
    ap.add_argument("--gtrun")
    ap.add_argument("--spec", default=os.path.join(HERE, "eval"))
    ap.add_argument("--json")
    ap.add_argument("-v", action="store_true")
    ap.add_argument("--gate", choices=["gt", "ocr"],
                    help="fail (exit 1) when a total is over the limits in eval/gate.json for this run kind")
    a = ap.parse_args()
    runs = [json.loads(l) for l in open(a.run, encoding="utf-8") if l.strip()]
    gtruns = {}
    if a.gtrun:
        for l in open(a.gtrun, encoding="utf-8"):
            if l.strip():
                r = json.loads(l)
                gtruns[r["image"]] = r
    tot = {"chars": 0, "omitted": 0, "OCR": 0, "分段": 0, "後處理": 0, "units": 0, "wrong": 0,
           "wrong_OCR": 0, "wrong_後處理": 0, "wrong_引擎": 0, "keys": 0, "key_err": 0, "key_neg": 0, "key_num": 0,
           "key_other": 0, "key_missing": 0, "neg_keys": 0, "num_keys": 0}
    reasons = {}
    per_image = []
    for run in runs:
        name = run["image"]
        sp = os.path.join(a.spec, name + ".ref.json")
        if not os.path.exists(sp):
            print("no spec for", name)
            continue
        spec = json.load(open(sp, encoding="utf-8"))
        gt = load_gt(os.path.join(a.img, name + ".gt.tsv"))
        res = score_image(spec, gt, run)
        gres = {r["id"]: r for r in score_image(spec, gt, gtruns[name])} if name in gtruns else {}
        img = {"image": name, "chars": 0, "omitted": 0, "wrong": 0, "key_err": 0, "units": 0}
        for r in res:
            if "error" in r:
                print(name, r["id"], r["error"])
                continue
            if r["optional"]:
                continue
            img["units"] += 1
            img["chars"] += r["chars"]
            img["omitted"] += r["omitted"]
            for k, v in r["omit_layers"].items():
                tot[k] += v
            for k, v in r["omit_reasons"].items():
                reasons[k] = reasons.get(k, 0) + v
            layer = None
            if r["wrong"]:
                g = gres.get(r["id"])
                if g and g["covered"] >= 0.3 and not g["wrong"]:
                    layer = "OCR"
                elif r["raw_pass"]:
                    layer = "後處理"
                else:
                    layer = "引擎"
                tot["wrong_" + layer] += 1
                img["wrong"] += 1
            r["wrong_layer"] = layer
            # Keys: an omitted unit loses its keys too.
            for k in r["keys"]:
                tot["keys"] += 1
                tot["neg_keys" if k["type"] == "neg" else "num_keys" if k["type"] in NUMERIC else "keys"] += 0
                if k["type"] == "neg":
                    tot["neg_keys"] += 1
                elif k["type"] in NUMERIC:
                    tot["num_keys"] += 1
                if k["src"] in r["key_lost"]:
                    tot["key_err"] += 1
                    tot["key_missing"] += 1
                    img["key_err"] += 1
                elif k["src"] in r["bad_keys"]:
                    tot["key_err"] += 1
                    img["key_err"] += 1
                    tot["key_neg" if k["type"] == "neg" else "key_num" if k["type"] in NUMERIC else "key_other"] += 1
            if a.v and (r["omitted"] or r["wrong"]):
                why = []
                if r["omitted"]:
                    why.append("漏 %d/%d %s %s" % (r["omitted"], r["chars"], {k: v for k, v in r["omit_layers"].items() if v},
                                                  r["omit_reasons"]))
                if r["wrong"]:
                    why.append("錯[%s] must缺%s key錯%s" % (layer, r["miss_must"], r["bad_keys"]))
                print("  %s %s %s\n      -> %s" % (name, r["id"], "; ".join(why), r["tx"].replace("\n", " | ")))
        tot["chars"] += img["chars"]
        tot["omitted"] += img["omitted"]
        tot["units"] += img["units"]
        tot["wrong"] += img["wrong"]
        img["omit_rate"] = img["omitted"] / max(1, img["chars"])
        per_image.append(img)
        print("%-22s 漏翻率 %5.1f%%  錯譯 %2d/%2d  關鍵錯 %d" % (name, 100 * img["omit_rate"], img["wrong"], img["units"], img["key_err"]))
    rate = tot["omitted"] / max(1, tot["chars"])
    print("TOTAL 漏翻率 %.1f%% (%d/%d 字; OCR %d, 分段 %d, 後處理 %d)" % (100 * rate, tot["omitted"], tot["chars"], tot["OCR"],
                                                                     tot["分段"], tot["後處理"]))
    print("      分段原因 %s" % dict(sorted(reasons.items(), key=lambda x: -x[1])))
    print("      錯譯 %d/%d 單位 (OCR %d, 後處理 %d, 引擎 %d)；每張平均 %.2f" % (
        tot["wrong"], tot["units"], tot["wrong_OCR"], tot["wrong_後處理"], tot["wrong_引擎"], tot["wrong"] / max(1, len(per_image))))
    print("      關鍵資訊錯誤 %d/%d (否定 %d/%d, 數字單位 %d/%d, 其他 %d, 整段漏翻而遺失 %d)" % (
        tot["key_err"], tot["keys"], tot["key_neg"], tot["neg_keys"], tot["key_num"], tot["num_keys"], tot["key_other"],
        tot["key_missing"]))
    if a.json:
        json.dump({"total": tot, "omit_rate": rate, "reasons": reasons, "images": per_image}, open(a.json, "w", encoding="utf-8"),
                  ensure_ascii=False, indent=1)
    if a.gate:
        # Regression limits (ARCHITECTURE.md §7.3): negation flips 0, the rest
        # not above the P0 numbers.
        lim = json.load(open(os.path.join(a.spec, "gate.json"), encoding="utf-8"))[a.gate]
        got = dict(tot)
        got["omit_rate"] = rate
        bad = [(k, got[k], v) for k, v in lim.items() if not k.startswith("_") and got.get(k, 0) > v]
        for k, g, v in bad:
            print("GATE FAIL %s = %s > %s" % (k, round(g, 4) if isinstance(g, float) else g, v))
        print("GATE %s: %s" % (a.gate, "FAIL" if bad else "pass"))
        sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
