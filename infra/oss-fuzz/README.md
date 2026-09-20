# OSS-Fuzz integration for LOOK

[OSS-Fuzz](https://github.com/google/oss-fuzz) is Google's free continuous-fuzzing
service for open-source projects. LOOK already ships six libFuzzer targets for its
hand-written wire parsers (`cpp/tests/fuzz/`), run briefly on every push
(`.github/workflows/sanitizers.yml`). OSS-Fuzz turns that into **continuous** fuzzing
at scale, with automatic crash reports and regression tracking — the highest-leverage
way to keep the parser attack surface honest without a paid audit.

This directory holds the three files OSS-Fuzz needs, ready to submit.

## Fuzz targets

| Target | Parser seam |
|---|---|
| `fuzz_http_request` | `look/http_parse.h` |
| `fuzz_json_str` | `look/json_str_parse.h` |
| `fuzz_mysql_lenenc` | `look/mysql_wire_parse.h` |
| `fuzz_pg_parse_error` | `look/pg_parse.h` |
| `fuzz_smtp_addr` | `look/smtp_parse.h` |
| `fuzz_imap_parse` | `look/imap_parse.h` |

Each is a single translation unit including only its pure header seam plus a
`LLVMFuzzerTestOneInput` entry point — no full-binary link needed.

## How to apply (one-time)

1. Fork `github.com/google/oss-fuzz`.
2. Create `projects/look/` and copy these three files into it:
   `project.yaml`, `Dockerfile`, `build.sh` (`chmod +x build.sh`).
3. **Confirm the primary contact.** `project.yaml` uses `security@codlook.com`; it
   must be a monitored inbox — OSS-Fuzz emails crash reports there, and the address
   has to be verifiable to the project. Add `auto_ccs:` for extra maintainers if wanted.
4. Test the integration locally (needs Docker):
   ```bash
   cd oss-fuzz
   python3 infra/helper.py build_image look
   python3 infra/helper.py build_fuzzers --sanitizer address look
   python3 infra/helper.py check_build look
   python3 infra/helper.py run_fuzzer look fuzz_http_request -- -runs=100000
   ```
5. Open a PR to `google/oss-fuzz` with just the `projects/look/` directory. The
   OSS-Fuzz team reviews new projects manually; approval is required before it runs.

## Keeping it in sync

`build.sh` compiles whatever `cpp/tests/fuzz/*.cpp` targets are listed in its
`CORPUS` map. When a new fuzzer is added to the repo, add its line there (and to
`sanitizers.yml`) so OSS-Fuzz picks it up. Seed corpora are zipped from the
`corpus_*` directories when present.

## Cheaper interim step

Until OSS-Fuzz approves the project, the in-repo CI fuzz job already runs each target
under `-fsanitize=fuzzer,address,undefined` on every push. OSS-Fuzz adds duration,
scale, and history — not the first line of defense.
