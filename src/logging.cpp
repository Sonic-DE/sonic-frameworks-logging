// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "logging.h"
#include "logging_p.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLockFile>
#include <QMutex>
#include <QStandardPaths>
#include <QThread>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>

#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <poll.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef Q_OS_LINUX
#include <sys/mman.h>
#endif

namespace Sonic::Logging {
namespace {
    constexpr qint64 rotationCheckIntervalMs = 60000;
    constexpr qint64 rotationSizeBytes = 2097152;
    constexpr qint64 writePauseMs = 60000;
    constexpr int stdoutPollMs = 100;
    const char defaultJournalSocket[] = "/run/systemd/journal/socket";

    struct Testing {
        Internal::TestConfig config;
        std::atomic<qint64> clockOffset{0};
        std::atomic<bool> setuid{false};
        std::atomic<int> writeError{0};
        std::atomic<int> sinkDelay{0};
        std::atomic<int> journalFailAfter{-1};
        std::atomic<int> dladdrCalls{0};
    };

    // Leaked on purpose: messages can arrive from other threads while the process exits.
    Testing& testing()
    {
        static Testing* instance = new Testing;
        return *instance;
    }

    qint64 nowMs()
    {
        static const auto start = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() + testing().clockOffset.load();
    }

    // Internal problems go to stderr; the handler never logs through Qt.
    void note(const QByteArray& text)
    {
        const QByteArray line = QByteArrayLiteral("sonic-logging: ") + text + '\n';
        fputs(line.constData(), stderr);
    }

    uid_t trustedUid()
    {
        const qint64 uid = testing().config.trustedUid;
        return uid >= 0 ? static_cast<uid_t>(uid) : 0;
    }

    uid_t parentTrustedUid()
    {
        const qint64 uid = testing().config.parentTrustedUid;
        return uid >= 0 ? static_cast<uid_t>(uid) : trustedUid();
    }

    bool runningAsRoot()
    {
        const int injected = testing().config.isRoot;
        return injected >= 0 ? injected == 1 : geteuid() == 0;
    }

    bool runningSetuid()
    {
        return testing().setuid.load() || getuid() != geteuid() || getgid() != getegid();
    }

    QByteArray utcStamp()
    {
        return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmss")).toLatin1();
    }

    QByteArray levelToken(QtMsgType type)
    {
        switch (type) {
        case QtDebugMsg:
            return QByteArrayLiteral("(DD)");
        case QtInfoMsg:
            return QByteArrayLiteral("(II)");
        case QtWarningMsg:
            return QByteArrayLiteral("(WW)");
        case QtCriticalMsg:
        case QtFatalMsg:
            return QByteArrayLiteral("(EE)");
        }
        return QByteArrayLiteral("(DD)");
    }

    // A newline continues the entry indented, so it can't look like a new entry; other control characters are shown escaped.
    QByteArray escaped(const QString& text)
    {
        const QByteArray utf8 = text.toUtf8();
        QByteArray out;
        out.reserve(utf8.size());
        for (const char c : utf8) {
            const auto u = static_cast<unsigned char>(c);
            if (c == '\n') {
                out.append("\n    ");
            } else if (u < 0x20 && c != '\t') {
                char buffer[5];
                snprintf(buffer, sizeof buffer, "\\x%02x", u);
                out.append(buffer);
            } else {
                out.append(c);
            }
        }
        return out;
    }

    QByteArray formatLine(QtMsgType type, const QString& source, const QString& message)
    {
        return QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toLatin1() + ' ' + levelToken(type) + " [" + escaped(source) + "] " + escaped(message)
            + '\n';
    }

    QString programShortName()
    {
#if defined(__GLIBC__) || defined(Q_OS_LINUX)
        return QString::fromLocal8Bit(program_invocation_short_name);
#else
        return QString::fromLocal8Bit(getprogname());
#endif
    }

    QString defaultBinaryName()
    {
        if (runningSetuid()) {
#ifdef Q_OS_LINUX
            return QFileInfo(QFile::symLinkTarget(QStringLiteral("/proc/self/exe"))).fileName();
#else
            return QString::fromLocal8Bit(getprogname());
#endif
        }
        return programShortName();
    }

    bool writeAll(int fd, const QByteArray& data)
    {
        qsizetype offset = 0;
        while (offset < data.size()) {
            const ssize_t written = ::write(fd, data.constData() + offset, data.size() - offset);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            offset += written;
        }
        return true;
    }

    // ---- Sources -------------------------------------------------------------

    struct Source {
        QString bracket;
        QString component;
        bool fromQt = false;
    };

    struct Registry {
        QMutex mutex; // the cache mutex; never held while writing
        QHash<const void*, QString> componentByBase;
        QHash<const void*, Source> sourceByCategory;
    };

    Registry& registry()
    {
        static Registry* instance = new Registry;
        return *instance;
    }

    int findMainBase(dl_phdr_info* info, size_t, void* data)
    {
        for (int i = 0; i < info->dlpi_phnum; ++i) {
            if (info->dlpi_phdr[i].p_type == PT_LOAD) {
                Dl_info dlInfo;
                if (dladdr(reinterpret_cast<const void*>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr), &dlInfo) != 0) {
                    *static_cast<const void**>(data) = dlInfo.dli_fbase;
                }
                break;
            }
        }
        return 1; // the first object reported is the main program
    }

    const void* mainProgramBase()
    {
        static const void* base = [] {
            const void* found = nullptr;
            dl_iterate_phdr(findMainBase, &found);
            return found;
        }();
        return base;
    }

    bool countedDladdr(const void* address, Dl_info* info)
    {
        testing().dladdrCalls.fetch_add(1);
        return dladdr(address, info) != 0;
    }

    bool isReverseDomain(const QString& name)
    {
        static const QStringList topLevelDomains = {
            QStringLiteral("org"),
            QStringLiteral("com"),
            QStringLiteral("net"),
            QStringLiteral("io"),
            QStringLiteral("dev"),
            QStringLiteral("edu"),
            QStringLiteral("gov"),
            QStringLiteral("info"),
            QStringLiteral("app"),
            QStringLiteral("eu"),
            QStringLiteral("de"),
            QStringLiteral("uk"),
            QStringLiteral("fr"),
            QStringLiteral("nl"),
            QStringLiteral("ch"),
            QStringLiteral("at"),
            QStringLiteral("it"),
            QStringLiteral("es"),
            QStringLiteral("se"),
            QStringLiteral("no"),
            QStringLiteral("fi"),
            QStringLiteral("dk"),
            QStringLiteral("pl"),
            QStringLiteral("cz"),
            QStringLiteral("ru"),
            QStringLiteral("jp"),
            QStringLiteral("cn"),
            QStringLiteral("us"),
            QStringLiteral("ca"),
            QStringLiteral("au"),
            QStringLiteral("br"),
            QStringLiteral("in"),
            QStringLiteral("me"),
            QStringLiteral("co"),
        };
        const qsizetype dot = name.indexOf(QLatin1Char('.'));
        return dot > 0 && dot < name.size() - 1 && topLevelDomains.contains(name.left(dot));
    }

    bool isOwnCategoryName(const char* category)
    {
        return qstrcmp(category, "default") == 0 || qstrcmp(category, "qml") == 0 || qstrcmp(category, "js") == 0;
    }

    // ---- State ---------------------------------------------------------------

    enum class Capture {
        None,
        Systemd,
        Runit,
        S6,
        OpenRC,
        Dinit,
        XSession,
    };

    const char* captureName(Capture capture)
    {
        switch (capture) {
        case Capture::None:
            return "none";
        case Capture::Systemd:
            return "systemd";
        case Capture::Runit:
            return "runit";
        case Capture::S6:
            return "s6";
        case Capture::OpenRC:
            return "OpenRC supervise-daemon";
        case Capture::Dinit:
            return "dinit";
        case Capture::XSession:
            return "the X session's .xsession-errors";
        }
        return "none";
    }

    // Read without locks by the handler; replaced, never modified, on reconfiguration.
    struct Config {
        Options options;
        QString binaryName;
        QString executableComponent;
        QString filePath;
        bool setuid = false;
    };

    // Sink state, guarded by writeMutex except where the fork path reads the descriptors.
    struct State {
        QMutex writeMutex;
        std::atomic<const Config*> config{nullptr};
        pid_t pid = 0;
        bool isRoot = false;
        Capture capture = Capture::None;
        bool stdoutIsFifo = false;
        std::atomic<int> fileFd{-1};
        bool fileToStderr = false;
        QLockFile* startupLock = nullptr;
        QString startupLockPath;
        bool rotationChecked = false;
        qint64 lastRotationCheck = 0;
        qint64 pausedUntil = 0;
        bool writeFailureReported = false;
        int journalFd = -1;
        bool journalEnabled = false;
        bool journalFailureReported = false;
        int journalSends = 0;
        bool stderrMirror = false;
        quint64 dropped = 0;
        bool dropReported = false;
        bool needNewline = false;
    };

    std::atomic<State*> g_state{nullptr};

    struct InstallState {
        QMutex mutex;
        bool explicitInstalled = false;
        bool autoInstalled = false;
        QtMessageHandler previous = nullptr;
    };

    InstallState& installState()
    {
        static InstallState* instance = new InstallState;
        return *instance;
    }

    // With no custom handler installed, Qt reports its own default handler as the previous one.
    QtMessageHandler qtDefaultHandler()
    {
        static const QtMessageHandler handler = [] {
            const QtMessageHandler current = qInstallMessageHandler(nullptr);
            return qInstallMessageHandler(current);
        }();
        return handler;
    }

    Source resolveSource(const char* category, const Config* config)
    {
        const QString name = QString::fromLatin1(category);
        if (isOwnCategoryName(category)) {
            return {config->binaryName, config->executableComponent.isEmpty() ? config->binaryName : config->executableComponent, false};
        }

        Registry& reg = registry();
        {
            QMutexLocker locker(&reg.mutex);
            const auto cached = reg.sourceByCategory.constFind(category);
            if (cached != reg.sourceByCategory.constEnd()) {
                return *cached;
            }
        }

        Source source;
        Dl_info info;
        if (!countedDladdr(category, &info) || !info.dli_fbase) {
            source = {name, name, name.startsWith(QLatin1String("qt."))};
        } else {
            const QString file = QFile::decodeName(info.dli_fname ? info.dli_fname : "");
            // Qt names all its categories qt.*, and its libraries libQt6*. Where a library is installed says
            // nothing about who wrote it: Qt's plugin directory also holds KDE's, SonicDE's and others' plugins.
            source.fromQt = name.startsWith(QLatin1String("qt.")) || QFileInfo(file).fileName().startsWith(QLatin1String("libQt6"));
            if (info.dli_fbase == mainProgramBase()) {
                source.bracket = name;
                source.component = config->executableComponent.isEmpty() ? config->binaryName : config->executableComponent;
            } else {
                QString id;
                {
                    QMutexLocker locker(&reg.mutex);
                    id = reg.componentByBase.value(info.dli_fbase);
                }
                if (id.isEmpty()) {
                    id = isReverseDomain(name) ? name : Internal::idFromLibraryFileName(file);
                }
                source.bracket = id;
                source.component = id;
            }
        }

        QMutexLocker locker(&reg.mutex);
        reg.sourceByCategory.insert(category, source);
        return source;
    }

    QString executableComponent()
    {
        Registry& reg = registry();
        QMutexLocker locker(&reg.mutex);
        return reg.componentByBase.value(mainProgramBase());
    }

    bool dropped(QtMsgType type, const char* category, const QString& message, const Source& source, const Config* config)
    {
        if (type != QtDebugMsg && type != QtInfoMsg) {
            return false;
        }
        if (source.fromQt) {
            return true;
        }
        if (type != QtDebugMsg) {
            return false;
        }
        // SonicDE's own debug messages are never filtered.
        if (qstrncmp(category, "org.sonicde.", 12) == 0) {
            return false;
        }
        const QString name = QString::fromLatin1(category);
        for (const QString& prefix : config->options.dropDebugCategoryPrefixes) {
            if (name.startsWith(prefix)) {
                return true;
            }
        }
        for (const QString& exact : config->options.dropDebugCategoryExact) {
            if (name == exact) {
                return true;
            }
        }
        for (const QString& substring : config->options.dropDebugMessageSubstrings) {
            if (message.contains(substring, Qt::CaseInsensitive)) {
                return true;
            }
        }
        return false;
    }

    // ---- Root directory ------------------------------------------------------

    QByteArray rootDirectoryPath()
    {
        return QFile::encodeName(testing().config.fsRoot + QStringLiteral(SONIC_LOGGING_ROOT_DIR));
    }

    QByteArray parentOf(const QByteArray& path)
    {
        const qsizetype slash = path.lastIndexOf('/');
        return slash > 0 ? path.left(slash) : QByteArrayLiteral("/");
    }

    bool trustedDirectory(const struct stat& st, uid_t owner)
    {
        return S_ISDIR(st.st_mode) && st.st_uid == owner && (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
    }

    bool prepareRootDirectory(QByteArray* notice)
    {
        if (!runningAsRoot()) {
            note(QByteArrayLiteral("not running as root; the root log directory is left alone"));
            return false;
        }
        const QByteArray directory = rootDirectoryPath();
        const QByteArray parent = parentOf(directory);

        struct stat parentStat;
        if (lstat(parent.constData(), &parentStat) != 0 || !trustedDirectory(parentStat, parentTrustedUid())) {
            note(parent + QByteArrayLiteral(" is not a root-owned directory closed to group and others; not writing a log file under it"));
            return false;
        }

        // Serialises root processes starting at once; flock works on a directory descriptor.
        const int parentFd = open(parent.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (parentFd < 0) {
            note(QByteArrayLiteral("cannot open ") + parent + QByteArrayLiteral(": ") + strerror(errno));
            return false;
        }
        while (flock(parentFd, LOCK_EX) != 0 && errno == EINTR) { }

        bool ok = false;
        for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
            struct stat st;
            if (lstat(directory.constData(), &st) != 0) {
                if (errno != ENOENT) {
                    note(QByteArrayLiteral("cannot inspect ") + directory + QByteArrayLiteral(": ") + strerror(errno));
                    break;
                }
                if (mkdir(directory.constData(), 0755) != 0) {
                    if (errno == EEXIST) {
                        continue;
                    }
                    note(QByteArrayLiteral("cannot create ") + directory + QByteArrayLiteral(": ") + strerror(errno));
                    break;
                }
                chmod(directory.constData(), 0755);
                ok = true;
                break;
            }

            if (trustedDirectory(st, trustedUid())) {
                if ((st.st_mode & 07777) != 0755) {
                    chmod(directory.constData(), 0755);
                }
                ok = true;
                break;
            }

            QByteArray reason;
            if (S_ISLNK(st.st_mode)) {
                reason = QByteArrayLiteral("it is a symlink");
            } else if (!S_ISDIR(st.st_mode)) {
                reason = QByteArrayLiteral("it is not a directory");
            } else if (st.st_uid != trustedUid()) {
                reason = QByteArrayLiteral("it is owned by uid ") + QByteArray::number(st.st_uid);
            } else {
                reason = QByteArrayLiteral("it is writable by group or others");
            }
            const QByteArray aside = directory + QByteArrayLiteral(".untrusted-") + utcStamp();
            if (rename(directory.constData(), aside.constData()) != 0) {
                if (errno == ENOENT) {
                    continue;
                }
                note(QByteArrayLiteral("cannot move untrusted ") + directory + QByteArrayLiteral(" aside: ") + strerror(errno));
                break;
            }
            const QByteArray text = QByteArrayLiteral("moved untrusted ") + directory + QByteArrayLiteral(" to ") + aside + QByteArrayLiteral(" because ") + reason;
            note(text);
            if (notice) {
                *notice = text;
            }
        }

        flock(parentFd, LOCK_UN);
        close(parentFd);
        return ok;
    }

    // ---- Files ---------------------------------------------------------------

    void moveAside(const QByteArray& path)
    {
        const QByteArray aside = path + QByteArrayLiteral(".untrusted-") + utcStamp();
        if (rename(path.constData(), aside.constData()) == 0) {
            note(QByteArrayLiteral("moved untrusted ") + path + QByteArrayLiteral(" to ") + aside);
        }
    }

    // Opens for append without following a symlink or blocking on a FIFO; checks the result is a plain file of ours.
    int openLogFile(const QByteArray& path, bool root)
    {
        const mode_t mode = root ? 0644 : 0600;
        for (int attempt = 0; attempt < 2; ++attempt) {
            const int fd = open(path.constData(), O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, mode);
            if (fd >= 0) {
                struct stat st;
                const uid_t owner = root ? trustedUid() : geteuid();
                if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == owner) {
                    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
                    fchmod(fd, mode);
                    return fd;
                }
                close(fd);
            } else if (errno != ELOOP && errno != ENXIO) {
                note(QByteArrayLiteral("cannot open ") + path + QByteArrayLiteral(": ") + strerror(errno));
                return -1;
            }
            if (attempt == 0) {
                moveAside(path);
            }
        }
        note(QByteArrayLiteral("cannot open ") + path + QByteArrayLiteral(" as a plain file; not writing a log file"));
        return -1;
    }

    void rotateFile(const QByteArray& path)
    {
        const QByteArray last = path + QByteArrayLiteral(".last");
        unlink(last.constData());
        rename(path.constData(), last.constData());
    }

    // ---- Capture by a service manager ----------------------------------------

    QString readComm(qint64 pid)
    {
        const QString procRoot = testing().config.procRoot.isEmpty() ? QStringLiteral("/proc") : testing().config.procRoot;
        QFile file(procRoot + QLatin1Char('/') + QString::number(pid) + QStringLiteral("/comm"));
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return QString::fromLocal8Bit(file.readAll()).trimmed();
    }

    bool journalStreamIsStderr()
    {
        const QByteArray value = qgetenv("JOURNAL_STREAM");
        const qsizetype colon = value.indexOf(':');
        if (colon <= 0) {
            return false;
        }
        bool devOk = false;
        bool inoOk = false;
        const qulonglong device = value.left(colon).toULongLong(&devOk);
        const qulonglong inode = value.mid(colon + 1).toULongLong(&inoOk);
        struct stat st;
        return devOk && inoOk && fstat(STDERR_FILENO, &st) == 0 && st.st_dev == device && st.st_ino == inode;
    }

    Capture detectCapture(bool* stdoutIsFifo)
    {
        *stdoutIsFifo = false;
        if (journalStreamIsStderr()) {
            return Capture::Systemd;
        }

        struct stat out;
        if (fstat(STDOUT_FILENO, &out) != 0) {
            return Capture::None;
        }
        const bool fifo = S_ISFIFO(out.st_mode);
        const bool regular = S_ISREG(out.st_mode);
        *stdoutIsFifo = fifo;

#ifdef Q_OS_LINUX
        const qint64 parentPid = testing().config.parentPid >= 0 ? testing().config.parentPid : getppid();
        const QString parent = readComm(parentPid);
        if (fifo && parent == QLatin1String("runsv")) {
            return Capture::Runit;
        }
        if (fifo && parent == QLatin1String("s6-supervise")) {
            return Capture::S6;
        }
        if ((fifo || regular) && parent == QLatin1String("supervise-daemon")) {
            return Capture::OpenRC;
        }
        if ((fifo || regular) && parentPid == 1 && readComm(1) == QLatin1String("dinit")) {
            return Capture::Dinit;
        }
        if (regular && QFileInfo(QFile::symLinkTarget(QStringLiteral("/proc/self/fd/1"))).fileName() == QLatin1String(".xsession-errors")) {
            return Capture::XSession;
        }
#endif
        return Capture::None;
    }

    // Writes without being killed by SIGPIPE when the reader has gone.
    ssize_t writeNoSigpipe(int fd, const char* data, size_t size)
    {
        sigset_t pipeSet;
        sigset_t oldSet;
        sigemptyset(&pipeSet);
        sigaddset(&pipeSet, SIGPIPE);
        sigset_t pending;
        sigpending(&pending);
        const bool alreadyPending = sigismember(&pending, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &pipeSet, &oldSet);
        const ssize_t written = ::write(fd, data, size);
        const int savedErrno = errno;
        if (written < 0 && savedErrno == EPIPE && !alreadyPending) {
            const timespec zero{0, 0};
            sigtimedwait(&pipeSet, nullptr, &zero);
        }
        pthread_sigmask(SIG_SETMASK, &oldSet, nullptr);
        errno = savedErrno;
        return written;
    }

    // One poll + write round per chunk, so a stalled reader costs lines, never a hung program. Only the first
    // line of an outage waits up to 100 ms; while lines are being dropped, the pipe is checked without waiting.
    bool writePolled(State* state, const QByteArray& data)
    {
        const int timeout = state->dropped > 0 ? 0 : stdoutPollMs;
        qsizetype offset = 0;
        while (offset < data.size()) {
            pollfd fds{STDOUT_FILENO, POLLOUT, 0};
            const int ready = poll(&fds, 1, timeout);
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready <= 0 || (fds.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                if (offset > 0) {
                    state->needNewline = true;
                }
                return false;
            }
            const ssize_t written = writeNoSigpipe(STDOUT_FILENO, data.constData() + offset, data.size() - offset);
            if (written < 0) {
                if (errno == EINTR || errno == EAGAIN) {
                    continue;
                }
                if (offset > 0) {
                    state->needNewline = true;
                }
                return false;
            }
            offset += written;
        }
        return true;
    }

    // ---- journald ------------------------------------------------------------

    QByteArray journalSocketPath()
    {
        return testing().config.journalSocketPath.isEmpty() ? QByteArray(defaultJournalSocket) : QFile::encodeName(testing().config.journalSocketPath);
    }

    int openJournal()
    {
        const QByteArray path = journalSocketPath();
        struct stat st;
        if (lstat(path.constData(), &st) != 0 || !S_ISSOCK(st.st_mode)) {
            return -1;
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (static_cast<size_t>(path.size()) >= sizeof(address.sun_path)) {
            return -1;
        }
        memcpy(address.sun_path, path.constData(), path.size());
        const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return -1;
        }
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            close(fd);
            return -1;
        }
        const int bufferSize = 8 * 1024 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize));
        return fd;
    }

    int journalPriority(QtMsgType type)
    {
        switch (type) {
        case QtDebugMsg:
            return 7;
        case QtInfoMsg:
            return 6;
        case QtWarningMsg:
            return 4;
        case QtCriticalMsg:
            return 3;
        case QtFatalMsg:
            return 2;
        }
        return 7;
    }

#ifdef Q_OS_LINUX
    bool sendJournalMemfd(int journalFd, const QByteArray& entry)
    {
        const int memfd = memfd_create("sonic-logging-journal", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (memfd < 0) {
            return false;
        }
        bool ok = writeAll(memfd, entry)
            && fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) == 0;
        if (ok) {
            union {
                cmsghdr header;
                char buffer[CMSG_SPACE(sizeof(int))];
            } control{};
            msghdr message{};
            message.msg_control = control.buffer;
            message.msg_controllen = sizeof(control.buffer);
            cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cmsg), &memfd, sizeof(int));
            ok = sendmsg(journalFd, &message, MSG_NOSIGNAL) >= 0;
        }
        close(memfd);
        return ok;
    }
#endif

    // Returns false when journald could not take the entry; the sink is then off for the rest of the process.
    bool sendJournal(State* state, const Config* config, QtMsgType type, const QMessageLogContext& context, const QString& message, const Source& source)
    {
        if (!state->journalEnabled) {
            return false;
        }
        QByteArray entry;
        entry += Internal::encodeJournalField("MESSAGE", message.toUtf8());
        entry += Internal::encodeJournalField("PRIORITY", QByteArray::number(journalPriority(type)));
        entry += Internal::encodeJournalField("SYSLOG_IDENTIFIER", config->binaryName.toUtf8());
        entry += Internal::encodeJournalField("SONIC_COMPONENT", source.component.toUtf8());
        if (context.category && *context.category) {
            entry += Internal::encodeJournalField("QT_CATEGORY", context.category);
        }
        if (context.file) {
            entry += Internal::encodeJournalField("CODE_FILE", context.file);
            entry += Internal::encodeJournalField("CODE_LINE", QByteArray::number(context.line));
            if (context.function) {
                entry += Internal::encodeJournalField("CODE_FUNC", context.function);
            }
        }

        bool ok = false;
        const int failAfter = testing().journalFailAfter.load();
        if (failAfter >= 0 && state->journalSends >= failAfter) {
            errno = EIO;
        } else if (send(state->journalFd, entry.constData(), entry.size(), MSG_NOSIGNAL) >= 0) {
            ok = true;
        }
#ifdef Q_OS_LINUX
        else if (errno == EMSGSIZE) {
            ok = sendJournalMemfd(state->journalFd, entry);
        }
#endif
        if (ok) {
            ++state->journalSends;
            return true;
        }
        if (!state->journalFailureReported) {
            state->journalFailureReported = true;
            note(QByteArrayLiteral("journald send failed (") + strerror(errno) + QByteArrayLiteral("); not using journald any more"));
        }
        close(state->journalFd);
        state->journalFd = -1;
        state->journalEnabled = false;
        return false;
    }

    // ---- Sinks ---------------------------------------------------------------

    // Pauses for 60 s after a failure, reporting only the first one, so a full disk never makes the handler loop.
    void writeWithBackoff(State* state, int fd, const QByteArray& line)
    {
        const qint64 now = nowMs();
        if (state->pausedUntil > now) {
            return;
        }
        const int injected = testing().writeError.load();
        bool ok;
        if (injected != 0) {
            errno = injected;
            ok = false;
        } else {
            ok = writeAll(fd, line);
        }
        if (ok) {
            return;
        }
        if (!state->writeFailureReported) {
            state->writeFailureReported = true;
            note(QByteArrayLiteral("writing the log failed (") + strerror(errno) + QByteArrayLiteral("); pausing for 60 s"));
        }
        state->pausedUntil = now + writePauseMs;
    }

    void reopenFile(State* state, const Config* config)
    {
        const int old = state->fileFd.exchange(-1);
        if (old >= 0) {
            close(old);
        }
        const int fd = openLogFile(QFile::encodeName(config->filePath), state->isRoot);
        state->fileFd.store(fd);
        state->fileToStderr = fd < 0;
    }

    void checkRotation(State* state, const Config* config, bool force)
    {
        if (state->capture != Capture::None || config->filePath.isEmpty()) {
            return;
        }
        const qint64 now = nowMs();
        if (!force && state->rotationChecked && now - state->lastRotationCheck < rotationCheckIntervalMs) {
            return;
        }
        state->rotationChecked = true;
        state->lastRotationCheck = now;

        const QByteArray path = QFile::encodeName(config->filePath);
        const int fd = state->fileFd.load();
        struct stat open;
        struct stat onDisk;
        if (fd < 0 || fstat(fd, &open) != 0 || lstat(path.constData(), &onDisk) != 0 || open.st_dev != onDisk.st_dev || open.st_ino != onDisk.st_ino) {
            // Another instance or logrotate moved the file.
            reopenFile(state, config);
            return;
        }
        if (force || (!config->options.rotateOnStartup && open.st_size > rotationSizeBytes)) {
            rotateFile(path);
            reopenFile(state, config);
        }
    }

    void writeDroppedNotice(State* state, const Config* config)
    {
        const QByteArray notice = formatLine(QtWarningMsg, config->binaryName, QString::number(state->dropped) + QStringLiteral(" log lines dropped: output pipe full"));
        if (writePolled(state, notice)) {
            state->dropped = 0;
        }
    }

    void writeStdout(State* state, const Config* config, const QByteArray& line)
    {
        if (!state->stdoutIsFifo) {
            writeWithBackoff(state, STDOUT_FILENO, line);
            return;
        }
        if (state->needNewline) {
            if (!writePolled(state, QByteArrayLiteral("\n"))) {
                ++state->dropped;
                return;
            }
            state->needNewline = false;
        }
        if (state->dropped > 0) {
            writeDroppedNotice(state, config);
        }
        if (state->dropped > 0 || !writePolled(state, line)) {
            ++state->dropped;
            if (!state->dropReported) {
                state->dropReported = true;
                note(QByteArrayLiteral("the output pipe is full; dropping log lines"));
            }
        }
    }

    void writeSinks(State* state, const Config* config, QtMsgType type, const QMessageLogContext& context, const QString& message, const Source& source)
    {
        const QByteArray line = formatLine(type, source.bracket, message);
        bool lineOnStderr = false;

        switch (state->capture) {
        case Capture::Systemd:
            // stderr is the journal stream, so it is the fallback when the native socket fails.
            if (!sendJournal(state, config, type, context, message, source)) {
                writeAll(STDERR_FILENO, line);
            }
            return;
        case Capture::None:
            checkRotation(state, config, false);
            if (state->fileFd.load() >= 0) {
                writeWithBackoff(state, state->fileFd.load(), line);
                if (type == QtFatalMsg) {
                    fsync(state->fileFd.load());
                }
            } else {
                writeAll(STDERR_FILENO, line);
                lineOnStderr = true;
            }
            break;
        default:
            writeStdout(state, config, line);
            break;
        }

        sendJournal(state, config, type, context, message, source);

        if (state->stderrMirror && !lineOnStderr) {
            writeAll(STDERR_FILENO, line);
        }
    }

    // A forked child can't take locks another parent thread may hold: one write to the inherited descriptor.
    void writeForkedChild(State* state, const Config* config, QtMsgType type, const char* category, const QString& message)
    {
        const QString bracket = isOwnCategoryName(category) ? config->binaryName : QString::fromLatin1(category);
        const QByteArray line = formatLine(type, bracket, message);
        int fd = state->fileFd.load();
        if (state->capture != Capture::None && state->capture != Capture::Systemd) {
            fd = STDOUT_FILENO;
        } else if (fd < 0) {
            fd = STDERR_FILENO;
        }
        writeAll(fd, line);
    }

    // ---- Configuration -------------------------------------------------------

    QString userLogDirectory()
    {
        if (!testing().config.stateDir.isEmpty()) {
            return testing().config.stateDir + QStringLiteral("/sonic");
        }
        const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
        // HOME unset makes Qt fall back to "/" and the state location to /.local/state.
        if (base.isEmpty() || QDir::isRelativePath(base) || base == QLatin1String("/")
            || (qEnvironmentVariableIsEmpty("XDG_STATE_HOME") && QDir::homePath() == QDir::rootPath())) {
            note(QByteArrayLiteral("no usable home directory for the log file; logging to stderr"));
            return {};
        }
        return base + QStringLiteral("/sonic");
    }

    QString chooseFilePath(State* state, const QString& binaryName, QByteArray* notice)
    {
        if (state->isRoot || runningSetuid()) {
            if (!prepareRootDirectory(notice)) {
                return {};
            }
            return QFile::decodeName(rootDirectoryPath()) + QLatin1Char('/') + binaryName + QStringLiteral(".log");
        }
        const QString directory = userLogDirectory();
        if (directory.isEmpty()) {
            return {};
        }
        if (!QDir().mkpath(directory)
            || !QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
            note(QByteArrayLiteral("cannot create ") + QFile::encodeName(directory) + QByteArrayLiteral(" with mode 0700; logging to stderr"));
            return {};
        }
        return directory + QLatin1Char('/') + binaryName + QStringLiteral(".log");
    }

    // Rotates on every run when this is the first live instance of the binary; the lock is held until exit.
    void rotateOnStartup(State* state, const QString& filePath)
    {
        const QString lockPath = filePath + QStringLiteral(".lock");
        if (state->startupLock && state->startupLockPath == lockPath) {
            return;
        }
        auto* lock = new QLockFile(lockPath); // leaked with the rest of the state
        lock->setStaleLockTime(0);
        if (!lock->tryLock(0)) {
            delete lock;
            return;
        }
        state->startupLock = lock;
        state->startupLockPath = lockPath;
        const QByteArray path = QFile::encodeName(filePath);
        struct stat st;
        if (lstat(path.constData(), &st) == 0 && S_ISREG(st.st_mode)) {
            rotateFile(path);
        } else {
            const QByteArray last = path + QByteArrayLiteral(".last");
            unlink(last.constData());
        }
    }

    // Called with the write lock held (or before the handler is installed).
    void configure(State* state, const Options& options)
    {
        auto* config = new Config; // never deleted: the handler reads configs without locks
        config->options = options;
        config->setuid = runningSetuid();
        config->binaryName = Internal::cleanBinaryName(options.binaryName.isEmpty() || config->setuid ? defaultBinaryName() : options.binaryName);
        config->executableComponent = executableComponent();

        state->pid = getpid();
        state->isRoot = runningAsRoot();
        state->capture = detectCapture(&state->stdoutIsFifo);

        if (config->setuid) {
            QLoggingCategory::setFilterRules(QString());
        }

        QByteArray notice;
        if (state->capture == Capture::None) {
            config->filePath = chooseFilePath(state, config->binaryName, &notice);
        }

        if (state->journalFd >= 0) {
            close(state->journalFd);
            state->journalFd = -1;
        }
        state->journalEnabled = false;
        if (options.journald || state->capture == Capture::Systemd) {
            state->journalFd = openJournal();
            state->journalEnabled = state->journalFd >= 0;
        }
        state->stderrMirror = options.mirrorToStderrWhenTty && isatty(STDERR_FILENO) && !journalStreamIsStderr();

        if (!config->filePath.isEmpty()) {
            if (options.rotateOnStartup) {
                rotateOnStartup(state, config->filePath);
            }
            reopenFile(state, config);
            state->rotationChecked = false;
        } else {
            const int old = state->fileFd.exchange(-1);
            if (old >= 0) {
                close(old);
            }
            state->fileToStderr = state->capture == Capture::None;
        }

        state->config.store(config);
        g_state.store(state);

        QMessageLogContext context;
        if (!notice.isEmpty()) {
            writeSinks(state, config, QtWarningMsg, context, QString::fromLocal8Bit(notice), Source{config->binaryName, config->binaryName, false});
        }
        if (state->capture != Capture::None) {
            writeSinks(state,
                config,
                QtInfoMsg,
                context,
                QStringLiteral("output captured by %1; not writing an own log file").arg(QLatin1String(captureName(state->capture))),
                Source{config->binaryName, config->binaryName, false});
        }
    }

    State* stateForInstall()
    {
        State* state = g_state.load();
        return state ? state : new State; // leaked: messages can arrive from other threads while the process exits
    }
}

// ---- Public API ----------------------------------------------------------

void handleMessage(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    State* state = g_state.load();
    const Config* config = state ? state->config.load() : nullptr;
    const char* category = context.category ? context.category : "default";

    if (!config) {
        const QString bracket = isOwnCategoryName(category) ? programShortName() : QString::fromLatin1(category);
        fputs(formatLine(type, bracket, message).constData(), stderr);
        return;
    }

    if (getpid() != state->pid) {
        writeForkedChild(state, config, type, category, message);
        return;
    }

    if (config->setuid && type == QtDebugMsg) {
        return;
    }

    const Source source = resolveSource(category, config);
    if (dropped(type, category, message, source, config)) {
        return;
    }

    QMutexLocker locker(&state->writeMutex);
    if (const int delay = testing().sinkDelay.load(); delay > 0) {
        QThread::msleep(delay);
    }
    writeSinks(state, state->config.load(), type, context, message, source);
}

QtMessageHandler install(const Options& options)
{
    InstallState& installs = installState();
    QMutexLocker installLocker(&installs.mutex);

    if (installs.explicitInstalled) {
        // Already ours: change nothing. Restored away by the caller: install again with the same file.
        const QtMessageHandler current = qInstallMessageHandler(handleMessage);
        return current == handleMessage ? nullptr : current;
    }

    State* state = stateForInstall();
    {
        QMutexLocker writeLocker(&state->writeMutex);
        configure(state, options);
    }
    installs.explicitInstalled = true;

    if (installs.autoInstalled) {
        qInstallMessageHandler(handleMessage);
        return installs.previous;
    }
    installs.previous = qInstallMessageHandler(handleMessage);
    return installs.previous;
}

QString logFilePath()
{
    State* state = g_state.load();
    const Config* config = state ? state->config.load() : nullptr;
    return config ? config->filePath : QString();
}

bool prepareRootLogDirectory()
{
    return prepareRootDirectory(nullptr);
}

QString rootLogDirectory()
{
    return QStringLiteral(SONIC_LOGGING_ROOT_DIR);
}

namespace Internal {
    void autoInstall()
    {
        InstallState& installs = installState();
        QMutexLocker installLocker(&installs.mutex);
        if (installs.explicitInstalled || installs.autoInstalled) {
            return;
        }
        const QtMessageHandler qtDefault = qtDefaultHandler();
        const QtMessageHandler previous = qInstallMessageHandler(handleMessage);
        if (previous && previous != qtDefault) {
            // The host has its own handler; leave it in charge.
            qInstallMessageHandler(previous);
            return;
        }
        State* state = stateForInstall();
        {
            QMutexLocker writeLocker(&state->writeMutex);
            configure(state, Options{});
        }
        installs.autoInstalled = true;
        installs.previous = previous;
    }

    void registerComponent(const void* address, const char* id)
    {
        Dl_info info;
        if (dladdr(address, &info) == 0 || !info.dli_fbase) {
            return;
        }
        Registry& reg = registry();
        QMutexLocker locker(&reg.mutex);
        reg.componentByBase.insert(info.dli_fbase, QString::fromLatin1(id));
    }

    void setForTesting(const TestConfig& config)
    {
        testing().config = config;
    }

    void advanceClockForTesting(qint64 msecs)
    {
        testing().clockOffset.fetch_add(msecs);
    }

    void setSetuidForTesting(bool setuid)
    {
        testing().setuid.store(setuid);
    }

    void setWriteErrorForTesting(int error)
    {
        testing().writeError.store(error);
    }

    void setSinkDelayForTesting(int msecs)
    {
        testing().sinkDelay.store(msecs);
    }

    void failJournalAfterForTesting(int sends)
    {
        testing().journalFailAfter.store(sends);
    }

    void forceRotationForTesting()
    {
        State* state = g_state.load();
        if (!state) {
            return;
        }
        QMutexLocker locker(&state->writeMutex);
        checkRotation(state, state->config.load(), true);
    }

    int dladdrCallsForTesting()
    {
        return testing().dladdrCalls.load();
    }

    QString idFromLibraryFileName(const QString& path)
    {
        QString name = QFileInfo(path).fileName();
        const qsizetype so = name.indexOf(QLatin1String(".so"));
        if (so > 0) {
            name.truncate(so);
        }
        if (name.startsWith(QLatin1String("libKF6"))) {
            return QStringLiteral("org.kde.kf6.") + name.mid(6).toLower();
        }
        if (name.startsWith(QLatin1String("libQt6"))) {
            return QStringLiteral("org.qt-project.qt6.") + name.mid(6).toLower();
        }
        return name;
    }

    QByteArray encodeJournalField(const QByteArray& key, const QByteArray& value)
    {
        if (!value.contains('\n')) {
            return key + '=' + value + '\n';
        }
        QByteArray field = key + '\n';
        quint64 length = value.size();
        char bytes[8];
        for (int i = 0; i < 8; ++i) {
            bytes[i] = static_cast<char>(length & 0xff);
            length >>= 8;
        }
        field.append(bytes, 8);
        field += value;
        field += '\n';
        return field;
    }

    QString cleanBinaryName(const QString& name)
    {
        QString cleaned;
        cleaned.reserve(name.size());
        for (const QChar c : name) {
            const char16_t u = c.unicode();
            const bool allowed = (u >= u'A' && u <= u'Z') || (u >= u'a' && u <= u'z') || (u >= u'0' && u <= u'9') || u == u'.' || u == u'_' || u == u'+' || u == u'-';
            cleaned.append(allowed ? c : QLatin1Char('_'));
        }
        if (cleaned.isEmpty() || cleaned == QLatin1String(".") || cleaned == QLatin1String("..")) {
            return QStringLiteral("unknown");
        }
        return cleaned.left(64);
    }
}
}
