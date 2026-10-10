# Step 1 of the glossary build: Wikidata (CC0) items of the classes a phone
# camera sees on Japanese / Korean menus, signs, stations and shop fronts, and
# their ja / ko / zh-* / en labels (+ ja / ko aliases).
#
#   py fetch_wikidata.py [--refresh]      -> raw/items_*.json, raw/labels_*.json, wikidata_labels.jsonl
#
# Item sets: (kind, SPARQL pattern binding ?item).  Every set is limited to
# items that have a ja or ko label (and, for places / companies, a country).
import json
import os
import sys

import wd

HERE = os.path.dirname(os.path.abspath(__file__))
JP, KR = "wd:Q17", "wd:Q884"

LANG_LABEL = 'FILTER EXISTS { ?item rdfs:label ?srcl FILTER(LANG(?srcl) IN ("ja","ko")) }'
NOT_DISSOLVED = "FILTER NOT EXISTS { ?item wdt:P576 [] } FILTER NOT EXISTS { ?item wdt:P582 [] }"


def sub(root):  # subclasses (food items are mostly classes: soju is a subclass of distilled beverage)
    return f"?item wdt:P279+ wd:{root} ."


def inst(root):
    return f"?item wdt:P31/wdt:P279* wd:{root} ."


def located(root, country, extra=""):
    return f"?item wdt:P31/wdt:P279* wd:{root} ; wdt:P17 {country} . {extra}"


SETS = []
# Foods, dishes, drinks, ingredients (classes and instances).
for kind, root in [("food", "Q2095"), ("dish", "Q746549"), ("drink", "Q40050"), ("ingredient", "Q25403900"),
                   ("confectionery", "Q2920963"), ("seasoning", "Q2596997"), ("fruit", "Q3314483"),
                   ("vegetable", "Q11004")]:
    SETS.append((f"{kind}_sub", kind, sub(root)))
    SETS.append((f"{kind}_inst", kind, inst(root)))
# Japanese / Korean cuisine (dish items tagged with the cuisine, whatever their class).
SETS.append(("cuisine_jp", "dish", "?item wdt:P2012 wd:Q234138 ."))
SETS.append(("cuisine_kr", "dish", "?item wdt:P2012 wd:Q192820 ."))
SETS.append(("origin_food_jp", "food", f"?item wdt:P495 {JP} ; wdt:P279/wdt:P279* wd:Q2095 ."))
SETS.append(("origin_food_kr", "food", f"?item wdt:P495 {KR} ; wdt:P279/wdt:P279* wd:Q2095 ."))
# Railway: stations (metro stations are a subclass), lines.
for c, cc in (("jp", JP), ("kr", KR)):
    SETS.append((f"station_{c}", "station", located("Q55488", cc)))
    SETS.append((f"line_{c}", "line", located("Q728937", cc)))
    SETS.append((f"metroline_{c}", "line", located("Q15079663", cc)))
    SETS.append((f"airport_{c}", "landmark", located("Q1248784", cc)))
# Administrative areas still in use (prefectures, cities, wards, towns; Korean provinces, cities, gu, dong).
SETS.append(("admin_jp", "place", located("Q56061", JP, NOT_DISSOLVED)))
SETS.append(("admin_kr", "place", located("Q56061", KR, NOT_DISSOLVED)))
# Neighbourhoods / districts people go to (渋谷, 秋葉原, 명동, 홍대).
for c, cc in (("jp", JP), ("kr", KR)):
    SETS.append((f"neigh_{c}", "place", located("Q123705", cc)))
    SETS.append((f"district_{c}", "place", located("Q149621", cc)))
# Landmarks.
for c, cc in (("jp", JP), ("kr", KR)):
    for kind, root in [("tourist", "Q570116"), ("temple", "Q160742"), ("shrine", "Q845945"), ("castle", "Q23413"),
                       ("park", "Q22698"), ("museum", "Q33506"), ("palace", "Q16560"), ("tower", "Q12518"),
                       ("mall", "Q31374404"), ("amusement", "Q194195"), ("university", "Q3918"), ("market", "Q330284"),
                       ("bridge", "Q12280"), ("stadium", "Q483110"), ("zoo", "Q43501"), ("aquarium", "Q2281788")]:
        SETS.append((f"{kind}_{c}", "landmark", located(root, cc, NOT_DISSOLVED)))
# Companies and brands (with some notability: >= 4 Wikipedia / other sitelinks).
for c, cc in (("jp", JP), ("kr", KR)):
    SETS.append((f"company_{c}", "company",
                 f"?item wdt:P31/wdt:P279* wd:Q4830453 ; wdt:P17 {cc} ; wikibase:sitelinks ?sl . FILTER(?sl >= 4) {NOT_DISSOLVED}"))
    SETS.append((f"brand_{c}", "brand",
                 f"?item wdt:P31/wdt:P279* wd:Q431289 . {{ ?item wdt:P17 {cc} }} UNION {{ ?item wdt:P495 {cc} }} "
                 f"?item wikibase:sitelinks ?sl . FILTER(?sl >= 2)"))
    SETS.append((f"product_{c}", "brand",
                 f"?item wdt:P31/wdt:P279* wd:Q2424752 ; wdt:P495 {cc} ; wikibase:sitelinks ?sl . FILTER(?sl >= 2)"))
    SETS.append((f"chain_{c}", "brand",
                 f"{{ ?item wdt:P31/wdt:P279* wd:Q507619 }} UNION {{ ?item wdt:P31/wdt:P279* wd:Q18509232 }} "
                 f"?item wdt:P17 {cc} ."))

LANGS = ["ja", "ko", "zh-tw", "zh-hant", "zh-hk", "zh", "zh-hans", "zh-cn"]


def items_query(pattern):
    return f"SELECT DISTINCT ?item WHERE {{ {pattern} {LANG_LABEL} }}"


def labels_query_lang(qids):
    # SPARQL JSON keeps the language tag in xml:lang; wd.query flattens it away, so ask for it explicitly.
    vals = " ".join("wd:" + q for q in qids)
    langs = ",".join(f'"{l}"' for l in LANGS)
    return (f"SELECT ?item ?t ?lang ?a WHERE {{ VALUES ?item {{ {vals} }} "
            f"{{ ?item rdfs:label ?t BIND(0 AS ?a) }} UNION "
            f'{{ ?item skos:altLabel ?t FILTER(LANG(?t) IN ("ja","ko")) BIND(1 AS ?a) }} '
            f"BIND(LANG(?t) AS ?lang) FILTER(?lang IN ({langs})) }}")


def main():
    refresh = "--refresh" in sys.argv
    only = [a for a in sys.argv[1:] if not a.startswith("--")]
    kinds = {}  # qid -> set of (kind, set name)
    for name, kind, pattern in SETS:
        if only and name not in only:
            continue
        if "--cached" in sys.argv and not os.path.exists(os.path.join(wd.RAW, "items_" + name + ".json")):
            print(f"-- {name}: not fetched (time-box)", file=sys.stderr)
            continue
        try:
            rows = wd.query("items_" + name, items_query(pattern), refresh=refresh)
        except RuntimeError as e:
            print(f"!! {name}: {str(e)[:200]}", file=sys.stderr)
            continue
        for r in rows:
            q = r["item"].rsplit("/", 1)[1]
            kinds.setdefault(q, set()).add((kind, name))
    qids = sorted(kinds, key=lambda q: int(q[1:]))
    print(f"{len(qids)} items", file=sys.stderr)
    out = open(os.path.join(HERE, "wikidata_labels.jsonl"), "w", encoding="utf-8")
    B = 400
    for i in range(0, len(qids), B):
        batch = qids[i:i + B]
        try:
            rows = wd.query(f"labels_{batch[0]}_{len(batch)}", labels_query_lang(batch), refresh=refresh)
        except RuntimeError as e:
            print(f"!! labels batch {i}: {str(e)[:200]}", file=sys.stderr)
            continue
        by = {}
        for r in rows:
            q = r["item"].rsplit("/", 1)[1]
            by.setdefault(q, []).append((r["lang"], r["t"], int(r["a"])))
        for q in batch:
            ls = by.get(q, [])
            out.write(json.dumps({"qid": q, "kinds": sorted({k for k, _ in kinds[q]}), "sets": sorted({s for _, s in kinds[q]}),
                                  "labels": [[l, t] for l, t, a in ls if a == 0],
                                  "aliases": [[l, t] for l, t, a in ls if a == 1]}, ensure_ascii=False) + "\n")
    out.close()


if __name__ == "__main__":
    main()
