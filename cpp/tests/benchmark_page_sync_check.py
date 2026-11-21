#!/usr/bin/env python3
"""The benchmarks page shows the same numbers as BENCHMARK.md.

The numbers are published twice: BENCHMARK.md (the record, with commands and raw files) and the
website's benchmarks.html. Both were edited by hand, so a re-measurement could reach one and not the
other and the site would show figures the record no longer holds.

This check reads the newest "## Results" section of BENCHMARK.md — its trivial-JSON, database and
language-core tables — and compares every number in them with the matching row of the page's
Workload 1, Workload 2 and Workload 0 tables: requests/s at c=100 and c=1000, p99 at both, RSS at
c=1000; for the language core the LOOK, Node and PHP times and the LOOK / Node ratio. A runtime or a
benchmark missing on either side is a failure too. Older values after an arrow ("105,520 <- 93,858")
are history, not the current number, and are ignored.

Usage: benchmark_page_sync_check.py [BENCHMARK.md] [benchmarks.html]
"""
import html
import re
import sys
from html.parser import HTMLParser

MD = sys.argv[1] if len(sys.argv) > 1 else "BENCHMARK.md"
PAGE = sys.argv[2] if len(sys.argv) > 2 else "docs/look.codlook.com/benchmarks.html"


def number(cell):
    """The current value of a cell: '**105,520** ← 93,858' -> '105520'; '1.06 s' -> '1.06 s'."""
    cell = html.unescape(cell).split("←")[0]
    cell = re.sub(r"[*`]", "", cell).replace(",", "").strip()
    m = re.match(r"(\d+(?:\.\d+)?)\s*(ms|s|MB|×)?", cell)
    if not m:
        return None
    return m.group(1) + (" " + m.group(2) if m.group(2) in ("ms", "s", "MB") else "")


def runtime_key(label):
    """'LOOK 1.1.4' / 'Go 1.22 net/http' / 'Node.js 22 cluster×2' -> 'look 1.1.4' / 'go' / 'node'."""
    label = re.sub(r"[*`]", "", html.unescape(label)).strip()
    first = label.split()[0].lower().split(".")[0]
    if first == "look":
        return "look " + label.split()[1]
    return first


# ── BENCHMARK.md: the newest results section ─────────────────────────────────────────────
text = open(MD, encoding="utf-8").read()
sections = re.split(r"^## ", text, flags=re.M)
newest = next((s for s in sections if s.startswith("Results")), None)
if newest is None:
    sys.exit("FAIL: no '## Results' section in " + MD)
title = newest.splitlines()[0]


def md_table(after):
    """Rows (lists of cells) of the first table after the line starting with `after`."""
    i = newest.find(after)
    if i < 0:
        sys.exit("FAIL: no table '%s' in the section '%s' of %s" % (after, title, MD))
    rows = []
    for line in newest[i:].splitlines()[1:]:
        if line.startswith("|"):
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if not set("".join(cells)) <= set("-: "):
                rows.append(cells)
        elif rows:
            break
    return rows[1:]   # without the header


# ── benchmarks.html: the three workload tables ───────────────────────────────────────────
class Tables(HTMLParser):
    def __init__(self):
        super().__init__()
        self.tables, self.heading, self.in_h2, self.row, self.cell = {}, "", False, None, None

    def handle_starttag(self, tag, attrs):
        if tag == "h2":
            self.in_h2, self.heading = True, ""
        elif tag == "table":
            self.tables[self.heading] = []
        elif tag == "tr":
            self.row = []
        elif tag in ("td", "th") and self.row is not None:
            self.cell = ""

    def handle_endtag(self, tag):
        if tag == "h2":
            self.in_h2 = False
        elif tag in ("td", "th") and self.cell is not None:
            self.row.append(re.sub(r"\s+", " ", self.cell).strip())
            self.cell = None
        elif tag == "tr" and self.row is not None:
            if self.row and self.tables:
                self.tables[list(self.tables)[-1]].append(self.row)
            self.row = None

    def handle_data(self, data):
        if self.in_h2:
            self.heading += data
        if self.cell is not None:
            self.cell += data


p = Tables()
p.feed(open(PAGE, encoding="utf-8").read())


def page_table(prefix):
    for heading, rows in p.tables.items():
        if heading.strip().startswith(prefix):
            return rows[1:]   # without the header
    sys.exit("FAIL: no table under a heading starting with '%s' in %s" % (prefix, PAGE))


failures = []
checked = 0


def compare(what, md_rows, page_rows, md_key, page_key, md_cols, page_cols, value=number):
    global checked
    page_by = {page_key(r): r for r in page_rows}
    md_keys = set()
    for r in md_rows:
        k = md_key(r)
        md_keys.add(k)
        pr = page_by.get(k)
        if pr is None:
            failures.append("%s: '%s' is in BENCHMARK.md, not on the page" % (what, r[0]))
            continue
        for name, mi, pi in zip(md_cols[0], md_cols[1], page_cols):
            a, b = value(r[mi]), value(pr[pi])
            checked += 1
            if a is None or a != b:
                failures.append("%s, %s, %s: BENCHMARK.md %s, page %s" % (what, r[0], name, r[mi], pr[pi]))
    for k, pr in page_by.items():
        if k not in md_keys:
            failures.append("%s: '%s' is on the page, not in BENCHMARK.md" % (what, pr[0]))


WEB_COLS = (["c=100", "p99 @100", "c=1000", "p99 @1000", "RSS @1000"], [1, 2, 3, 4, 5])
WEB_PAGE_COLS = [2, 4, 3, 5, 6]   # page: c=50, c=100, c=1000, p99@100, p99@1000, RAM
compare("trivial JSON", md_table("**Trivial JSON**"), page_table("Workload 1"),
        lambda r: runtime_key(r[0]), lambda r: runtime_key(r[0]), WEB_COLS, WEB_PAGE_COLS)
compare("one DB row", md_table("**One DB row**"), page_table("Workload 2"),
        lambda r: runtime_key(r[0]), lambda r: runtime_key(r[0]), WEB_COLS, WEB_PAGE_COLS)
compare("language core", md_table("**Language core**"), page_table("Workload 0"),
        lambda r: r[0].strip("*` "), lambda r: r[0].strip(),
        (["LOOK", "Node", "PHP", "LOOK / Node"], [1, 2, 3, 4]), [1, 2, 3, 4],
        # the record's core table is in milliseconds without a unit; the page writes "112 ms"
        value=lambda c: (number(c) or "").split(" ")[0] or None)

if failures:
    print("FAIL: benchmarks.html and BENCHMARK.md ('%s') disagree:" % title)
    for f in failures:
        print("  " + f)
    print("Update both from the same measurement (BENCHMARK.md is the record).")
    sys.exit(1)
print("OK: benchmarks.html shows the %d numbers of BENCHMARK.md '%s'" % (checked, title))
