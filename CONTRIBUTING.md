# Contributing

- C for programs, POSIX sh (or bash for the benchmark runner) for glue.
  No Python.
- Every tuning change needs numbers: run `bench/run.sh <label> <suite>`
  before and after and put both in the pull request. Changes that don't
  measure better get dropped, however reasonable they sound.
- Don't add anything that disables a protection: thermal limits, CPU
  mitigations, voltage limits, signature checks.
- Root-side code (optimzrd, optimzr-root) takes no paths or values from
  clients. New knobs go in the fixed tables.
- Test the daemon without root first: `app/build/optimzrd -n -s /tmp/o.sock`
  and `OPTIMZR_SOCK=/tmp/o.sock app/build/optimzr ...` log every write
  instead of doing it.
- Keep machine-specific data out of commits: serial numbers, MAC and IP
  addresses, UUIDs, host and user names. `bench/results/*/env.txt` and the
  logs are git-ignored for that reason.
