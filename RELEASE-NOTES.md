# Sonic Frameworks Logging Release Notes

Sonic Frameworks Logging gives every SonicDE program one consistent way to record what it is doing, which makes problems easier to track down.

## Unreleased

- Works on FreeBSD and GhostBSD too: messages are recorded the same way there, including when a service manager such as runit, s6 or OpenRC's supervisor collects a program's output.
- Services run by OpenRC's supervisor on Linux are now recognised, so their messages go to the service's own log instead of a separate log file.

## 6.31.0 (2026-11-10)

- New SonicDE framework: messages go to the system journal where one is running, otherwise to tidy per-program log files (system programs under /var/log/sonic, your own programs in your home folder) that are rotated automatically. Programs can choose the size at which their log is rotated.
- Works the same under every init system SonicDE supports: systemd, OpenRC, runit, s6 and dinit.
- A "fatal" message is always recorded as a critical error before the program stops, so the reason is never lost.
- Command-line tools can also show their messages in the output of the build or script that runs them, not only when run from a terminal, so a failing step still says why.
- Tools that run while software is being built can skip their log file entirely, so building packages never leaves log files behind on the build machine.
- Behind the scenes: its developer documentation now appears on the SonicDE website.
