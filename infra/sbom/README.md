# Vendored-dependency CVE watch

LOOK has minimal external runtime dependencies, but a few third-party components are
**vendored** (compiled in) or **statically linked** into release binaries — see
[../../THIRD_PARTY.md](../../THIRD_PARTY.md). "No runtime dependency to install" does
**not** mean they never need patching: a CVE against a pinned version ships inside every
release until the vendored copy is bumped. Static release binaries are especially
exposed — unlike the RPM (which links the OS `openssl-libs` and gets `dnf update`), a
statically-linked artifact stays vulnerable until it is rebuilt with a newer copy.

## How it works

- `look-vendored.cdx.json` — a CycloneDX SBOM listing each vendored component with its
  CPE and pinned version. **Single source of truth for the scan; keep versions in sync
  with THIRD_PARTY.md when you bump a copy.**
- `nvd_cve_check.py` — queries the NVD CVE API by CPE and re-evaluates each result
  against the pinned version using the cpeMatch version ranges (NVD's cpeName match is
  version-loose), so only CVEs whose vulnerable range genuinely contains the pinned
  version are reported. Exit 1 on an applicable CVE.
- `.github/workflows/deps-cve.yml` — runs it weekly (+ manual dispatch) and opens/updates
  a `deps-cve` issue on findings. It is a schedule, not a push gate: a red run is a
  signal to bump, not a merge blocker.

Run locally:

```bash
python3 infra/sbom/nvd_cve_check.py infra/sbom/look-vendored.cdx.json
# NVD_API_KEY in the env raises the rate limit.
```

## When it finds something — how to bump

1. Replace the vendored source under `cpp/src/<component>/` with the new upstream release
   (SQLite amalgamation, miniz, linenoise) — or bump the pinned OpenSSL used by the
   portable/Docker static build.
2. Update the version in **both** `THIRD_PARTY.md` and `look-vendored.cdx.json`.
3. Rebuild and run the full guard suite; then re-release so the shipped artifacts carry
   the patched copy.
