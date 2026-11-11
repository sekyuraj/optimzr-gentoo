# Security

Two things in this repo run as root. Both are small C programs with fixed
allow-lists, and both are meant to be read before you install them.

## optimzrd (the Power Control daemon)

- Runs as root under OpenRC, listens on `/run/optimzr/optimzrd.sock`
  (directory 0750, socket 0660, group from `/etc/conf.d/optimzrd`).
- Every connection is checked with `SO_PEERCRED`: root, or a user whose
  group list contains that group. Anyone else gets `err not allowed`.
- Clients can only pick from fixed profiles (`profile 50..100`,
  `net <name>`, `auto on|off`, `stock`). They never send a path, a value or
  a command line. Lines are capped at 256 bytes.
- The only programs it runs are `ethtool` and `tc`, from fixed paths, with a
  fixed argv and an empty environment.
- It records the original value of everything before the first change
  (`/var/lib/optimzr/optimzrd.stock`) and writes them back on `stock` and when
  the service stops.

## optimzr-root (the setup helper)

Optional. It exists so the benchmarks and the kernel install can run without a
root shell. `root-helper/setup.sh` installs it with a sudoers rule for one
user and that one binary. It has fixed commands with allow-lists (sysfs
paths, sysctl prefixes, install destinations named `optimzr*`, a list of
packages and services, and kernel installs whose release contains
`gentoo-power` and isn't the running kernel), backs up any file it replaces,
logs to `/var/log/optimzr-root.log`, and refuses to touch sudoers or itself.
`setup.sh --remove` takes it away again.

Be clear about what it is: it can install binaries into `/usr/local/sbin`,
edit `/etc/default/grub` and install kernels, so the user it's set up for
effectively has root. It's a guard against mistakes (wrong path, wrong
kernel, a typo in a sysctl), not a security boundary. Only set it up for an
account that already has sudo, and remove it when you're done.

## What this project does not do

- No mitigations=off, no disabled thermal limits, no voltage changes, no
  MSR or SMU writes. Fan curves stay in the BIOS: on boards where ACPI also
  uses the Super-I/O chip, writing PWM from Linux at the same time is unsafe.
- No network access from the daemon or the app.

## Reporting

Open a GitHub issue. If it's something that shouldn't be public, say so in
the issue without the details and you'll get a way to send them.
