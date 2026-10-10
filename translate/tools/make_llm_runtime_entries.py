#!/usr/bin/env python3
"""Prints the PM_LLM_RUNTIME_ENTRY lines of src/llm_engine_models.inc.

  py make_llm_runtime_entries.py llama-b11514-bin-win-cpu-x64.zip

For every PM_LLM_RUNTIME_FILE of the .inc: where its zip record (local file
header + deflated data) lies in the archive, and the SHA-256 of exactly those
bytes.  llm_engine_store.cpp fetches just those byte ranges (HTTP Range) and
checks them against the pin; the unpacked file is checked again against the
PM_LLM_RUNTIME_FILE SHA-256.  Run it again (and review the diff) with every
new pinned release; the full-archive pin stays the fallback.
"""
import hashlib, re, struct, sys, zipfile, os

zpath = sys.argv[1]
inc = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'llm_engine_models.inc')
names = re.findall(r'^PM_LLM_RUNTIME_FILE\("([^"]+)"', open(inc, encoding='utf-8').read(), re.M)
data = open(zpath, 'rb').read()
z = zipfile.ZipFile(zpath)
for n in names:
    i = z.getinfo(n)
    off = i.header_offset
    sig, ver, flag, meth, t, d, crc, cs, us, nl, el = struct.unpack('<IHHHHHIIIHH', data[off:off + 30])
    assert sig == 0x04034b50 and flag & 8 == 0 and meth in (0, 8), n  # no data descriptor
    assert cs == i.compress_size and us == i.file_size and crc == i.CRC, n
    ln = 30 + nl + el + cs
    print('PM_LLM_RUNTIME_ENTRY("%s", %dull, %dull, "%s")' % (n, off, ln, hashlib.sha256(data[off:off + ln]).hexdigest()))
