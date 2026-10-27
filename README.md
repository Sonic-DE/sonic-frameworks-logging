<!--
  - SPDX-FileCopyrightText: None
  - SPDX-License-Identifier: CC0-1.0
-->

# Sonic Logging

Sonic Logging is the logging library for SonicDE. It installs a Qt message handler that gives every executable its own log file, sends messages to journald where it runs, and gives SonicDE code a small logging API. Logs stay usable without systemd.

See [Sonic-DE/sonic#16](https://github.com/Sonic-DE/sonic/issues/16) for the background.

## Where logs go

- **One file per executable, named after the program:**
  - programs running as root write `/var/log/sonic/<binary>.log`. The directory is created root-owned with mode 0755; an untrusted one (another owner, a symlink, writable by group or others) is moved aside to `/var/log/sonic.untrusted-<timestamp>`, never reused. Packagers can move it with `-DSONIC_LOGGING_ROOT_DIR=…`.
  - other programs write `~/.local/state/sonic/<binary>.log` (`$XDG_STATE_HOME/sonic/`), mode 0600.
- **journald**, in addition, when its socket exists. It speaks journald's native protocol directly, so it doesn't depend on libsystemd. Entries carry `PRIORITY`, `QT_CATEGORY`, `SONIC_COMPONENT` and, where available, `CODE_*`.
- **No syslog.**
- **Service managers:** when systemd, runit, s6, OpenRC's supervise-daemon or dinit captures a program's output, or it goes to `~/.xsession-errors`, lines go there instead and no own file is written. A shell pipe or redirect never counts as a service manager. A stalled reader costs log lines, never a hung program.
- **stderr** mirrors the lines when it is a terminal, and is the fallback when no file can be written.

Each line reads `<timestamp> (DD|II|WW|EE) [<source>] <message>`. The source is the category for the executable's own messages, the library's ID for messages from a library, and the binary name for messages without a category.

**Rotation:** by default a file is renamed to `<file>.last` once it passes 2 MiB, checked at most every 60 s. With `Options::rotateOnStartup`, it is rotated to `.last` on every run instead.

Debug and info messages from Qt itself and other noisy sources are dropped; warnings and errors never are.

## Usage

In an application:

```cmake
find_package(SonicLogging REQUIRED)
target_link_libraries(myapp PRIVATE Sonic::Logging)
```

```cpp
#include <Sonic/Logging/Logging>

SONIC_LOGGING_COMPONENT("org.sonicde.myapp")

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // Returns the handler that was in place before, for callers that restore it.
    Sonic::Logging::install({.rotateOnStartup = true});
    sonicInfo() << "started";
}
```

`install()` also works before the application exists. Root services can call `Sonic::Logging::prepareRootLogDirectory()` (for example from a `--prepare-log-directory` option) before their service manager opens a log inside `/var/log/sonic`.

In a plugin loaded by a host that doesn't install a handler itself:

```cpp
#include <Sonic/Logging/Logging>

SONIC_LOGGING_COMPONENT("org.sonicde.myplugin")
SONIC_LOGGING_AUTO_INSTALL()
```

`SONIC_LOGGING_AUTO_INSTALL(fn)` also runs `fn` once, even in plugins loaded after the application starts. An automatic install never replaces a handler the host installed, and an explicit `install()` always wins over it.

### Logging from SonicDE code

Declare the component once per library, plugin or executable, in exactly one `.cpp`, and log with `sonicDebug()`, `sonicInfo()`, `sonicWarning()` and `sonicCritical()`. A second declaration in the same binary, or none, is a link error. Debug messages are on by default only in debug builds of the declaring code; `QT_LOGGING_RULES="org.sonicde.myplugin.debug=true"` turns them on otherwise.

## Building

Sonic Logging is built as part of SonicDE with the [sonicde-builder](https://github.com/Sonic-DE/sonicde-builder):

```shell
./build-repo sonic-frameworks-logging
```

It needs Qt 6.11 or newer (Core) and Extra CMake Modules.
