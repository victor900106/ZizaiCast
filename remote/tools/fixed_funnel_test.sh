#!/usr/bin/env bash
# 常駐外網 (0.8.2) end-to-end through the PUBLIC Funnel address (curl --resolve to
# the Funnel relay IPs from public DNS, so nothing goes over the tailnet).
# Usage: fixed_funnel_test.sh <host> <port> <right password>
# Runs against a pm_remote_spike started with --fixed-password ... --funnel <port>.
H=${1:?host, e.g. your-pc.your-tailnet.ts.net}; P=${2:-8443}; PW=${3:?password}
IP=$(curl -s -H 'accept: application/dns-json' "https://cloudflare-dns.com/dns-query?name=$H&type=A" | grep -o '"data":"[0-9.]*"' | head -1 | cut -d'"' -f4)
U="https://$H:$P"
C() { curl -s --resolve "$H:$P:$IP" -o /tmp/fx.body -w '%{http_code} %{time_total}' "$@"; }
echo "public relay IP: $IP  url: $U/"
echo "--- 1. page / (no password yet)"
r=$(C "$U/"); echo "GET / -> $r  fixed-config: $(grep -o '"fixed":true' /tmp/fx.body | head -1)  verified:false: $(grep -o '"verified":false' /tmp/fx.body | head -1)"
for p in /manifest.webmanifest /apple-touch-icon.png /icon.png /help; do r=$(C "$U$p"); echo "GET $p -> $r ($(wc -c </tmp/fx.body) bytes)"; done
echo "manifest: $(cat /tmp/fx.body | head -c 0)$(C "$U/manifest.webmanifest" >/dev/null; cat /tmp/fx.body)"
echo "--- 2. no login: the API is closed"
r=$(C -X POST "$U/k/-/session" -d '{"device":"fixedtest-0001"}'); echo "POST /k/-/session -> $r $(cat /tmp/fx.body)"
r=$(C -X POST "$U/k/1999999999.abcd.ffff/check"); echo "POST /k/<forged>/check -> $r $(cat /tmp/fx.body)"
r=$(C "$U/abcdef/"); echo "GET /<LAN-style token>/ -> $r"
echo "--- 3. wrong password once, then right"
r=$(C -X POST "$U/login" -d '{"password":"wrong-one"}'); echo "wrong #1 -> $r $(cat /tmp/fx.body)"
r=$(C -X POST "$U/login" -d "{\"password\":\"$PW\"}"); echo "right -> $r $(cat /tmp/fx.body | sed 's/"token":"\([^"]\{12\}\)[^"]*"/"token":"\1..."/')"
TOK=$(grep -o '"token":"[^"]*"' /tmp/fx.body | cut -d'"' -f4)
r=$(C -X POST "$U/k/$TOK/check"); echo "POST /k/<token>/check -> $r"
L=${TOK: -1}; [ "$L" = a ] && N=b || N=a; r=$(C -X POST "$U/k/${TOK%?}$N/check"); echo "POST /k/<token, last char changed>/check -> $r"
echo "--- 4. with the login: a session goes straight in (the spike would wait 10 minutes for 允許)"
r=$(curl -s --max-time 20 --resolve "$H:$P:$IP" -o /tmp/fx.body -w '%{http_code} %{time_total}' -X POST "$U/k/$TOK/session" -d '{"device":"fixedtest-0002","q":0}')
echo "POST /k/<token>/session -> $r $(head -c 120 /tmp/fx.body | tr -d '\r\n')..."
echo "--- 5. five wrong in a row -> locked 15 minutes (growing delay)"
for i in 1 2 3 4 5; do r=$(C -X POST "$U/login" -d "{\"password\":\"wrong-$i\"}"); echo "wrong #$i -> $r $(cat /tmp/fx.body)"; done
r=$(C -X POST "$U/login" -d "{\"password\":\"$PW\"}"); echo "right while locked -> $r $(cat /tmp/fx.body)"
r=$(C -X POST "$U/k/$TOK/check"); echo "the earlier login while locked -> $r (still valid: locking stops new guesses only)"
echo "TOKEN=$TOK" > /tmp/fx.tok
