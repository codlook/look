#!/usr/bin/env python3
"""
Vendored-dependency CVE gate for LOOK.

Reads a CycloneDX SBOM (infra/sbom/look-vendored.cdx.json) listing the vendored /
statically-linked third-party components with a CPE and pinned version, then queries
the authoritative NVD CVE API by CPE. NVD's cpeName match is version-loose (it can
return CVEs whose configuration lists the product with an unbounded version), so each
returned CVE is re-evaluated here against the pinned version using the cpeMatch version
ranges (versionStart/End Including/Excluding) or an exact criteria version. Only CVEs
whose vulnerable range genuinely contains the pinned version are reported as APPLICABLE.

Exit code:
  0  no applicable CVE (unbounded-wildcard-only matches are reported as REVIEW, not fail)
  1  at least one applicable CVE  (CI turns red)
  2  operational error (NVD unreachable, bad SBOM)

Usage:
  nvd_cve_check.py [path/to/sbom.cdx.json]
Env:
  NVD_API_KEY  optional; raises the NVD rate limit (recommended in CI).
"""
import json, os, sys, time, urllib.request, urllib.parse, urllib.error

NVD = "https://services.nvd.nist.gov/rest/json/cves/2.0"

def vkey(v):
    """Tolerant version -> comparable tuple. '3.47.2' -> (3,47,2); trailing letters
    become a fractional-ish tail so '1.0.1f' > '1.0.1'."""
    out = []
    for part in str(v).split('.'):
        num = ''
        suf = ''
        for ch in part:
            if ch.isdigit() and not suf:
                num += ch
            else:
                suf += ch
        out.append((int(num) if num else 0, suf))
    return out

def vcmp(a, b):
    A, B = vkey(a), vkey(b)
    for i in range(max(len(A), len(B))):
        x = A[i] if i < len(A) else (0, '')
        y = B[i] if i < len(B) else (0, '')
        if x[0] != y[0]: return -1 if x[0] < y[0] else 1
        if x[1] != y[1]: return -1 if x[1] < y[1] else 1
    return 0

def in_range(ver, m):
    """Does `ver` fall in this cpeMatch's vulnerable range?"""
    crit = m.get("criteria", "")
    parts = crit.split(":")
    crit_ver = parts[5] if len(parts) > 5 else "*"
    has_bounds = any(k in m for k in
        ("versionStartIncluding","versionStartExcluding","versionEndIncluding","versionEndExcluding"))
    if crit_ver not in ("*", "-") and not has_bounds:
        return ("EXACT" if vcmp(ver, crit_ver) == 0 else None)
    if not has_bounds and crit_ver in ("*", "-"):
        return "WILDCARD"   # unbounded — low confidence, report as REVIEW
    if "versionStartIncluding" in m and vcmp(ver, m["versionStartIncluding"]) < 0: return None
    if "versionStartExcluding" in m and vcmp(ver, m["versionStartExcluding"]) <= 0: return None
    if "versionEndIncluding" in m and vcmp(ver, m["versionEndIncluding"]) > 0: return None
    if "versionEndExcluding" in m and vcmp(ver, m["versionEndExcluding"]) >= 0: return None
    return "RANGE"

def product_of(cpe):
    p = cpe.split(":")
    return (p[3], p[4]) if len(p) > 4 else ("", "")

def applicable(cve, want_vendor, want_product, ver):
    verdicts = set()
    for conf in cve.get("configurations", []):
        for node in conf.get("nodes", []):
            for m in node.get("cpeMatch", []):
                if not m.get("vulnerable", False): continue
                vend, prod = product_of(m.get("criteria", ""))
                if vend != want_vendor or prod != want_product: continue
                r = in_range(ver, m)
                if r: verdicts.add(r)
    if verdicts & {"EXACT", "RANGE"}: return "APPLICABLE"
    if "WILDCARD" in verdicts: return "REVIEW"
    return None

def nvd_get(cpe):
    url = NVD + "?" + urllib.parse.urlencode({"cpeName": cpe, "resultsPerPage": 200})
    req = urllib.request.Request(url, headers={"User-Agent": "LOOK-dep-cve-check"})
    key = os.environ.get("NVD_API_KEY")
    if key: req.add_header("apiKey", key)
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=40) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            if e.code in (403, 503) and attempt < 3:
                time.sleep(8 * (attempt + 1)); continue
            raise
    raise RuntimeError("NVD unreachable")

def main():
    sbom_path = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(os.path.dirname(__file__), "look-vendored.cdx.json")
    with open(sbom_path, encoding="utf-8") as f:
        sbom = json.load(f)

    comps = [c for c in sbom.get("components", []) if c.get("cpe") and c.get("version")]
    if not comps:
        print("No components with a CPE + version in the SBOM — nothing to scan.")
        return 0

    fail, review = [], []
    for c in comps:
        vend, prod = product_of(c["cpe"])
        ver = c["version"]
        print(f"\n== {c['name']} {ver}  ({vend}:{prod}) ==")
        data = nvd_get(c["cpe"])
        total = data.get("totalResults", 0)
        hits_a, hits_r = [], []
        for item in data.get("vulnerabilities", []):
            cve = item.get("cve", {})
            cid = cve.get("id", "?")
            verdict = applicable(cve, vend, prod, ver)
            if verdict == "APPLICABLE": hits_a.append(cid)
            elif verdict == "REVIEW":   hits_r.append(cid)
        print(f"   NVD returned {total} CVE(s); applicable to {ver}: "
              f"{len(hits_a)}, needs-review(unbounded): {len(hits_r)}")
        for cid in sorted(set(hits_a)): print(f"     APPLICABLE  {cid}"); fail.append((c['name'], ver, cid))
        for cid in sorted(set(hits_r)): print(f"     REVIEW      {cid}"); review.append((c['name'], ver, cid))
        time.sleep(6)   # NVD rate limit courtesy

    print("\n" + "=" * 60)
    if fail:
        print(f"FAIL: {len(fail)} applicable CVE(s) in pinned vendored versions:")
        for n, v, c in fail: print(f"  {n} {v}: {c}  https://nvd.nist.gov/vuln/detail/{c}")
        print("Action: bump the vendored copy (THIRD_PARTY.md + infra/sbom + rebuild/re-release).")
        return 1
    print("OK: no CVE applies to the pinned vendored versions.")
    if review:
        print(f"NOTE: {len(review)} unbounded-version match(es) to eyeball (not failing):")
        for n, v, c in review: print(f"  {n} {v}: {c}")
    return 0

if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(2)
