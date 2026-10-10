# Polite Wikidata Query Service client: one query at a time, a descriptive
# User-Agent, a pause between queries, exponential backoff on 429 / 5xx
# (Retry-After honoured), results cached in raw/NAME.json (a rerun does not
# query again unless --refresh).
import json
import os
import sys
import time

import requests

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(HERE, "raw")
URL = "https://query.wikidata.org/sparql"
UA = "ZizaiCastGlossaryBuilder/0.1 (offline terminology build for a GPL-3 screen translator; python-requests)"
PAUSE = 2.0
_last = [0.0]


def query(name, sparql, refresh=False, tries=6):
    os.makedirs(RAW, exist_ok=True)
    path = os.path.join(RAW, name + ".json")
    if os.path.exists(path) and not refresh:
        return json.load(open(path, encoding="utf-8"))
    wait = 5.0
    for k in range(tries):
        dt = time.time() - _last[0]
        if dt < PAUSE:
            time.sleep(PAUSE - dt)
        t0 = time.time()
        try:
            r = requests.post(URL, data={"query": sparql, "format": "json"},
                              headers={"User-Agent": UA, "Accept": "application/sparql-results+json"}, timeout=90)
        except requests.RequestException as e:
            print(f"  {name}: {e!r}, retry in {wait:.0f}s", file=sys.stderr)
            _last[0] = time.time()
            time.sleep(wait)
            wait *= 2
            continue
        _last[0] = time.time()
        if r.status_code == 200:
            try:
                rows = r.json()["results"]["bindings"]
            except ValueError as e:  # a response cut off on the way
                print(f"  {name}: bad JSON ({e}), retry in {wait:.0f}s", file=sys.stderr)
                time.sleep(wait)
                wait *= 2
                continue
            out = [{k2: v["value"] for k2, v in row.items()} for row in rows]
            json.dump(out, open(path, "w", encoding="utf-8"), ensure_ascii=False)
            print(f"  {name}: {len(out)} rows, {time.time() - t0:.1f}s", file=sys.stderr)
            return out
        ra = r.headers.get("Retry-After")
        delay = float(ra) if ra and ra.isdigit() else wait
        print(f"  {name}: HTTP {r.status_code} ({r.text[:120]!r}), retry in {delay:.0f}s", file=sys.stderr)
        if r.status_code == 400 or "TimeoutException" in r.text:  # bad / too heavy query: retrying does not help
            raise RuntimeError(f"HTTP {r.status_code}: {r.text[:300]}")
        time.sleep(delay)
        wait *= 2
    raise RuntimeError(f"{name}: gave up")
