# Step 1b of the glossary build (English): Wikidata (CC0) foods / dishes / drinks / ingredients that have both an
# English label and a zh-tw label, -> raw/en_*.json and wikidata_en.jsonl (qid, kind, en label, zh-tw label).
#
#   py fetch_wikidata_en.py [--refresh]
#
# Uses wd.py (one query at a time, >= 2 s apart, Retry-After / exponential backoff, cached in raw/), with a
# project-only User-Agent and no other identifying data in the request.
import json
import os
import sys

import wd

wd.UA = "ZizaiCast-glossary/1.0 (github.com/victor900106/ZizaiCast)"
HERE = os.path.dirname(os.path.abspath(__file__))

ROOTS = [("food", "Q2095"), ("dish", "Q746549"), ("drink", "Q40050"), ("ingredient", "Q25403900"),
         ("confectionery", "Q2920963"), ("seasoning", "Q2596997"), ("fruit", "Q3314483"), ("vegetable", "Q11004"),
         ("cheese", "Q10943"), ("bread", "Q7802"), ("sauce", "Q178359"), ("soup", "Q41415"), ("pasta", "Q178"),
         ("cocktail", "Q134768"), ("beer_style", "Q1069467"), ("dessert", "Q182940"), ("salad", "Q9266"),
         ("sandwich", "Q28803"), ("fish_food", "Q152"), ("seafood", "Q192935"), ("meat", "Q10990"),
         ("spice", "Q42527"), ("herb", "Q207123"), ("nut", "Q11009"), ("cake", "Q13276")]


def q(pattern):
    return ("SELECT DISTINCT ?item ?en ?tw WHERE { " + pattern +
            ' ?item rdfs:label ?tw FILTER(LANG(?tw) = "zh-tw") ?item rdfs:label ?en FILTER(LANG(?en) = "en") }')


def main():
    refresh = "--refresh" in sys.argv
    by = {}
    for kind, root in ROOTS:
        for how, pat in (("sub", f"?item wdt:P279+ wd:{root} ."), ("inst", f"?item wdt:P31/wdt:P279* wd:{root} .")):
            try:
                rows = wd.query(f"en_{kind}_{how}", q(pat), refresh=refresh)
            except RuntimeError as e:
                print(f"!! {kind}_{how}: {str(e)[:160]}", file=sys.stderr)
                continue
            for r in rows:
                qid = r["item"].rsplit("/", 1)[1]
                d = by.setdefault(qid, {"qid": qid, "kinds": set(), "en": r["en"], "tw": r["tw"]})
                d["kinds"].add(kind)
    with open(os.path.join(HERE, "wikidata_en.jsonl"), "w", encoding="utf-8") as f:
        for qid in sorted(by, key=lambda x: int(x[1:])):
            d = by[qid]
            d["kinds"] = sorted(d["kinds"])
            f.write(json.dumps(d, ensure_ascii=False) + "\n")
    print(len(by), "items", file=sys.stderr)


if __name__ == "__main__":
    main()
