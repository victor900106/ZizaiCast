# Builds wikidata_labels.jsonl from whatever raw/ holds (items_*.json + labels_*.json), for a partial
# (time-boxed) fetch: items without a fetched label batch are left out.
import glob, json, os
HERE = os.path.dirname(os.path.abspath(__file__))
import fetch_wikidata as fw
kinds = {}
for name, kind, _ in fw.SETS:
    p = os.path.join(HERE, "raw", "items_" + name + ".json")
    if os.path.exists(p):
        for r in json.load(open(p, encoding="utf-8")):
            kinds.setdefault(r["item"].rsplit("/", 1)[1], set()).add((kind, name))
by = {}
for p in glob.glob(os.path.join(HERE, "raw", "labels_*.json")):
    for r in json.load(open(p, encoding="utf-8")):
        q = r["item"].rsplit("/", 1)[1]
        by.setdefault(q, set()).add((r["lang"], r["t"], int(r["a"])))
with open(os.path.join(HERE, "wikidata_labels.jsonl"), "w", encoding="utf-8") as out:
    for q in sorted(by, key=lambda q: int(q[1:])):
        if q not in kinds:
            continue
        ls = sorted(by[q])
        out.write(json.dumps({"qid": q, "kinds": sorted({k for k, _ in kinds[q]}), "sets": sorted({s for _, s in kinds[q]}),
                              "labels": [[l, t] for l, t, a in ls if a == 0], "aliases": [[l, t] for l, t, a in ls if a == 1]},
                             ensure_ascii=False) + "\n")
print(len(by), "items with labels of", len(kinds), "selected")
