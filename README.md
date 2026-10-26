<!--
  - SPDX-FileCopyrightText: None
  - SPDX-License-Identifier: CC0-1.0
-->

# Sonic Logging

Sonic Logging is the logging library for SonicDE. It installs a Qt message handler that sends each component's log output to the right place for the running system, so logs stay usable without systemd.

> **Status:** in development. The API below is the planned design and may still change. See [Sonic-DE/sonic#16](https://github.com/Sonic-DE/sonic/issues/16).

## Where logs go

It uses the first of these that's available:

1. **journald**, when it's running. It speaks journald's native socket protocol directly, so it doesn't depend on libsystemd. Messages keep their priority, Qt logging category and source location.
2. **syslog**, on systems with a syslog daemon.
3. **A log file**, otherwise:
   - system daemons write under `/var/log/sonic/`, and user processes under `$XDG_STATE_HOME/sonic/`;
   - the file is rotated on every start-up and whenever it grows past a size limit;
   - per-user logs are created with mode 0600.
4. **stderr**, as a last resort. Output is also mirrored there when running in a terminal.

Noisy debug output from Qt, KDE Frameworks and KWin internals is filtered out by default. Warnings and errors are never filtered.

## Usage

In an application:

```cmake
find_package(SonicLogging REQUIRED)
target_link_libraries(myapp PRIVATE Sonic::Logging)
```

```cpp
#include <Sonic/Logging/Logging>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    Sonic::Logging::install({.identifier = QStringLiteral("myapp")});
    // ...
}
```

In a plugin loaded by a host that doesn't install a handler itself:

```cpp
#include <Sonic/Logging/Logging>

SONIC_LOGGING_AUTO_INSTALL("desktop-interface")
```

## Building

Sonic Logging is built as part of SonicDE with the [sonicde-builder](https://github.com/Sonic-DE/sonicde-builder):

```shell
./build-repo sonic-frameworks-logging
```

It needs Qt 6.11 or newer, Extra CMake Modules and [Sonic Init Info](https://github.com/Sonic-DE/sonic-frameworks-init-info).
