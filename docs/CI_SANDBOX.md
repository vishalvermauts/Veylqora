# CI sandbox: what the jobs need, what failed, and what is set now

The CI jobs run in one cloud sandbox, `shell/veylqora-ci`, created by
`scripts/ci.sh`. This page records what the jobs need from it, what was missing,
why runs failed, and the settings in use. Update it when a setting changes.

## What the jobs need

| Need | Jobs | Setting |
|---|---|---|
| Compiler, CMake, Ninja, clang-format, clang-tidy, libFuzzer runtime | format, gcc, clang, san, fuzz, bench | apt packages installed by `sbx_provision` |
| Ubuntu and Debian package archives | provisioning | `**.ubuntu.com`, `**.debian.org` in the allowlist |
| GitHub: clone of the repository | every job | `github.com`, `codeload.github.com`, `**.githubusercontent.com` |
| Grammar sources fetched at configure time (raw GitHub) | every C++ job | `raw.githubusercontent.com` (covered by `**.githubusercontent.com`) |
| Python packages (MCP SDK, FastMCP, pytest) | mcp, e2e | `pypi.org`, `files.pythonhosted.org` |
| npm packages for the dashboard | web | `registry.npmjs.org`, `**.npmjs.org` |
| Node.js 22 runtime | web | PyPI wheel `nodejs-wheel-binaries==22.20.0` (Ubuntu's Node is too old for Vite 7) |
| Clean run time for a one-hour fuzz run | fuzz (about 65 min) | sandbox TTL of 2 hours at creation |
| CPU and memory for compile and fuzz | all | 4 vCPU and 8 GiB (`medium`) |

## What was missing, and why runs failed

1. **Default lifetime too short.** The sandbox was created with a 30-minute TTL.
   An hour-long fuzz run outlived it, so the sandbox hibernated mid-run and the
   job failed. Starting a stopped sandbox also gives it a fresh 30-minute TTL.
   The extension I added at the start of a run was not checked, so a failed
   extension went unnoticed.
2. **Stopping race.** A new run started while the previous sandbox was still
   stopping. Every exec was refused with "sandbox is stopping". This failed
   several certification runs in a row.
3. **npm blocked.** The network allowlist was fixed when the sandbox was created
   and did not include the npm registry. Web tests could not install packages.
4. **The 24-hour ceiling.** A sandbox cannot have its TTL extended past 24 hours
   from its creation. An old sandbox therefore cannot be relied on for long runs.
5. **Not in the plan.** The plan named jobs and gates but not what the sandbox
   had to provide: lifetime, network hosts, Node runtime, or sizing. I did not
   check these against the jobs before each long run.

## Fixes now in place

- `ci.sh` creates the sandbox with a 2-hour TTL, `medium` sizing, and an
  allowlist that includes PyPI and npm.
- `run_remote` waits for the sandbox to settle (not "stopping") before any exec.
- A fuzz run extends the TTL first, and stops with an error if the extension
  fails, instead of running and hibernating.
- A `web` job installs Node from a pinned PyPI wheel and runs `npm ci`,
  `npm run build` and `npm test`.
- The account-level cloud policy allows `registry.npmjs.org` and `*.npmjs.org`.

## Found while enabling it

- A sandbox listing can read "stopped" while a shutdown is still finishing. Exec
  is refused until it does, so the script now probes with a real exec before a run.
- A new sandbox can hold the apt lock during its first-boot updates. Provisioning
  retries apt for up to three minutes.
- The Node wheel's own `npm` launcher is broken. The web job runs npm's entry
  point through the wheel's Node instead.

Verified: the full default suite (format, gcc, clang, san, certs, mcp, fuzz,
eval, e2e, web) passes on the recreated sandbox.

## Standing limits

- A sandbox is recreated when it is older than about 20 hours, since the
  24-hour ceiling cannot be extended. Recreate it with
  `sbx --cloud rm veylqora-ci` and let the next run create it.
- Stop the sandbox after every run (`ci.sh` does this). It bills while running.
- The sandbox holds only rebuildable caches (the CI clone and build directories).
  Nothing in it is kept: results are committed to git.
