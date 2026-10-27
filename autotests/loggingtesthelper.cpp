// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

// Runs one logging scenario in a fresh process, since install() configures a process once.
// Usage: loggingtesthelper <scenario> [--option value ...] [arguments...]
// Results the test checks are printed to stderr as "HELPER:<key>=<value>" lines.

#include "logging.h"
#include "logging_p.h"
#include "testlibs.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QLibrary>
#include <QRegularExpression>
#include <QSet>
#include <QThread>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>

SONIC_LOGGING_COMPONENT("org.sonicde.test.app")

namespace {
// A Qt category declared in the executable, as an application's own categories are.
const QLoggingCategory testShellCategory("kde.testshell");
const QLoggingCategory cacheCheckCategory("kde.cachecheck");
const QLoggingCategory qtTestCategory("qt.test");
const QLoggingCategory droppedCategory("kf.dropped");

struct Arguments {
    QString scenario;
    Sonic::Logging::Internal::TestConfig config;
    Sonic::Logging::Options options;
    bool setuid = false;
    int journalFailAfter = -1;
    QStringList positional;
};

void report(const QByteArray& key, const QByteArray& value)
{
    const QByteArray line = "HELPER:" + key + '=' + value + '\n';
    fputs(line.constData(), stderr);
    fflush(stderr);
}

Arguments parse(int argc, char** argv)
{
    Arguments arguments;
    if (argc < 2) {
        fputs("usage: loggingtesthelper <scenario> [options]\n", stderr);
        exit(2);
    }
    arguments.scenario = QString::fromLocal8Bit(argv[1]);
    for (int i = 2; i < argc; ++i) {
        const QByteArray option = argv[i];
        const auto value = [&]() {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for %s\n", option.constData());
                exit(2);
            }
            return QString::fromLocal8Bit(argv[++i]);
        };
        if (option == "--fs-root") {
            arguments.config.fsRoot = value();
        } else if (option == "--proc-root") {
            arguments.config.procRoot = value();
        } else if (option == "--parent-pid") {
            arguments.config.parentPid = value().toLongLong();
        } else if (option == "--state-dir") {
            arguments.config.stateDir = value();
        } else if (option == "--root") {
            arguments.config.isRoot = 1;
        } else if (option == "--trusted-uid") {
            arguments.config.trustedUid = value().toLongLong();
        } else if (option == "--parent-trusted-uid") {
            arguments.config.parentTrustedUid = value().toLongLong();
        } else if (option == "--journal") {
            arguments.config.journalSocketPath = value();
        } else if (option == "--rotate-on-startup") {
            arguments.options.rotateOnStartup = true;
        } else if (option == "--binary-name") {
            arguments.options.binaryName = value();
        } else if (option == "--setuid") {
            arguments.setuid = true;
        } else if (option == "--journal-fail-after") {
            arguments.journalFailAfter = value().toInt();
        } else {
            arguments.positional << QString::fromLocal8Bit(option);
        }
    }
    return arguments;
}

QString positional(const Arguments& arguments, int index, const QString& fallback = {})
{
    return index < arguments.positional.size() ? arguments.positional.at(index) : fallback;
}

void reportPath()
{
    report("PATH", Sonic::Logging::logFilePath().toLocal8Bit());
}

int customMessages = 0;
void customHandler(QtMsgType, const QMessageLogContext&, const QString&)
{
    ++customMessages;
}

// Qt reports its own default handler as the previous one when no custom handler is installed.
QtMessageHandler qtDefaultHandler()
{
    static const QtMessageHandler handler = [] {
        const QtMessageHandler current = qInstallMessageHandler(nullptr);
        return qInstallMessageHandler(current);
    }();
    return handler;
}

QByteArray handlerName(QtMessageHandler handler)
{
    if (!handler) {
        return "null";
    }
    if (handler == qtDefaultHandler()) {
        return "default";
    }
    if (handler == customHandler) {
        return "custom";
    }
    if (handler == Sonic::Logging::handleMessage) {
        return "sonic";
    }
    return "other";
}

QtMessageHandler currentHandler()
{
    const QtMessageHandler current = qInstallMessageHandler(nullptr);
    qInstallMessageHandler(current);
    return current;
}

QLibrary* loadPlugin()
{
    auto* plugin = new QLibrary(QStringLiteral(TEST_PLUGIN_PATH));
    if (!plugin->load()) {
        report("PLUGIN_ERROR", plugin->errorString().toLocal8Bit());
        exit(3);
    }
    return plugin;
}

void reportPluginLoads(QLibrary* plugin)
{
    using LoadsFunction = int (*)();
    const auto loads = reinterpret_cast<LoadsFunction>(plugin->resolve("sonicLoggingTestPluginLoads"));
    report("PLUGIN_LOADS", QByteArray::number(loads ? loads() : -1));
}

const QRegularExpression& lineFormat()
{
    static const QRegularExpression format(QStringLiteral("^\\S+ \\((DD|II|WW|EE)\\) \\[[^\\]]+\\] "));
    return format;
}

// 8 threads log numbered lines while this thread forces rotations; every line must survive exactly once.
int runThreads()
{
    Sonic::Logging::install();
    const QString path = Sonic::Logging::logFilePath();
    constexpr int threadCount = 8;
    constexpr int linesPerThread = 10000;

    // std::thread, not QThread, so ThreadSanitizer sees the thread start and join.
    std::atomic<int> finished{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < threadCount; ++t) {
        threads.emplace_back([t, &finished] {
            for (int n = 0; n < linesPerThread; ++n) {
                qWarning().noquote() << QStringLiteral("thread-%1-%2").arg(t).arg(n);
            }
            ++finished;
        });
    }

    QByteArray collected;
    while (finished.load() < threadCount) {
        Sonic::Logging::Internal::forceRotationForTesting();
        QFile last(path + QStringLiteral(".last"));
        if (last.open(QIODevice::ReadOnly)) {
            collected += last.readAll();
        }
        QThread::msleep(5);
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    QFile current(path);
    if (current.open(QIODevice::ReadOnly)) {
        collected += current.readAll();
    }

    QSet<QString> seen;
    int malformed = 0;
    int duplicates = 0;
    const QList<QByteArray> lines = collected.split('\n');
    for (const QByteArray& raw : lines) {
        if (raw.isEmpty()) {
            continue;
        }
        const QString line = QString::fromUtf8(raw);
        if (!lineFormat().match(line).hasMatch()) {
            ++malformed;
            continue;
        }
        const qsizetype marker = line.indexOf(QLatin1String("thread-"));
        if (marker < 0) {
            continue;
        }
        const QString id = line.mid(marker);
        if (seen.contains(id)) {
            ++duplicates;
        }
        seen.insert(id);
    }
    report("THREAD_LINES", QByteArray::number(seen.size()));
    report("THREAD_MALFORMED", QByteArray::number(malformed));
    report("THREAD_DUPLICATES", QByteArray::number(duplicates));
    return 0;
}

// One thread opens a library that declares a component while four threads log.
int runDlopenThreads()
{
    Sonic::Logging::install();
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([t, &stop] {
            int n = 0;
            while (!stop.load()) {
                qWarning().noquote() << QStringLiteral("dlopen-load-%1-%2").arg(t).arg(n++);
            }
        });
    }
    QThread::msleep(20);
    void* handle = dlopen(TEST_DLOPEN_PATH, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        report("DLOPEN_ERROR", dlerror());
        return 3;
    }
    using WarningFunction = void (*)(const char*);
    const auto warning = reinterpret_cast<WarningFunction>(dlsym(handle, "sonicLoggingTestDlopenWarning"));
    warning("from-dlopen-library");
    QThread::msleep(20);
    stop.store(true);
    for (std::thread& thread : threads) {
        thread.join();
    }
    reportPath();
    return 0;
}

// A thread holds the write lock while another forks; the child must still log and exit.
int runFork()
{
    Sonic::Logging::install();
    Sonic::Logging::Internal::setSinkDelayForTesting(1500);
    std::thread holder([] {
        qWarning("holding-the-lock");
    });
    QThread::msleep(100);

    const pid_t child = fork();
    if (child == 0) {
        qWarning("from-forked-child");
        _exit(0);
    }
    int status = 0;
    bool exited = false;
    for (int waited = 0; waited < 2000; waited += 10) {
        if (waitpid(child, &status, WNOHANG) == child) {
            exited = true;
            break;
        }
        QThread::msleep(10);
    }
    if (!exited) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
    }
    report("CHILD_EXITED", exited ? "1" : "0");
    holder.join();
    Sonic::Logging::Internal::setSinkDelayForTesting(0);
    reportPath();
    return 0;
}

struct LogsWhenDestroyed {
    ~LogsWhenDestroyed()
    {
        qWarning("from-static-destructor");
    }
};
}

int main(int argc, char** argv)
{
    const Arguments arguments = parse(argc, argv);
    Sonic::Logging::Internal::setForTesting(arguments.config);
    Sonic::Logging::Internal::setSetuidForTesting(arguments.setuid);
    Sonic::Logging::Internal::failJournalAfterForTesting(arguments.journalFailAfter);
    const QString& scenario = arguments.scenario;

    if (scenario == QLatin1String("prepare")) {
        report("PREPARE", Sonic::Logging::prepareRootLogDirectory() ? "1" : "0");
        return 0;
    }

    if (scenario == QLatin1String("before-app")) {
        Sonic::Logging::install(arguments.options);
        qWarning("before-the-application");
        QCoreApplication app(argc, argv);
        qWarning("after-the-application");
        reportPath();
        return 0;
    }

    if (scenario == QLatin1String("early-plugin") || scenario == QLatin1String("auto-then-explicit") || scenario == QLatin1String("custom-then-auto")
        || scenario == QLatin1String("default-then-auto")) {
        if (positional(arguments, 0) == QLatin1String("custom-first") || scenario == QLatin1String("custom-then-auto")) {
            qInstallMessageHandler(customHandler);
        }
        QLibrary* plugin = loadPlugin();
        QCoreApplication app(argc, argv);
        reportPluginLoads(plugin);
        if (scenario == QLatin1String("auto-then-explicit")) {
            report("PREVIOUS", handlerName(Sonic::Logging::install(arguments.options)));
            qWarning("auto-then-explicit-run");
            reportPath();
        } else if (scenario == QLatin1String("custom-then-auto")) {
            report("CURRENT", handlerName(currentHandler()));
            qWarning("to-the-custom-handler");
            report("CUSTOM_MESSAGES", QByteArray::number(customMessages));
        } else {
            report("CURRENT", handlerName(currentHandler()));
            qWarning("from-the-auto-install");
            reportPath();
        }
        return 0;
    }

    QCoreApplication app(argc, argv);

    if (scenario == QLatin1String("late-plugin")) {
        QLibrary* plugin = loadPlugin();
        reportPluginLoads(plugin);
        report("CURRENT", handlerName(currentHandler()));
        return 0;
    }

    if (scenario == QLatin1String("explicit-then-auto")) {
        Sonic::Logging::install(arguments.options);
        QLibrary* plugin = loadPlugin();
        reportPluginLoads(plugin);
        report("CURRENT", handlerName(currentHandler()));
        qWarning("explicit-then-auto-run");
        reportPath();
        return 0;
    }

    if (scenario == QLatin1String("restore")) {
        const QtMessageHandler previous = Sonic::Logging::install(arguments.options);
        report("FIRST", handlerName(previous));
        report("SECOND", handlerName(Sonic::Logging::install(arguments.options)));
        qInstallMessageHandler(previous);
        report("RESTORED", handlerName(currentHandler()));
        Sonic::Logging::install(arguments.options);
        report("REINSTALLED", handlerName(currentHandler()));
        qWarning("after-reinstall");
        reportPath();
        return 0;
    }

    if (scenario == QLatin1String("threads")) {
        return runThreads();
    }
    if (scenario == QLatin1String("dlopen-threads")) {
        return runDlopenThreads();
    }
    if (scenario == QLatin1String("fork")) {
        return runFork();
    }

    Sonic::Logging::install(arguments.options);
    reportPath();

    if (scenario == QLatin1String("log")) {
        const int count = positional(arguments, 0, QStringLiteral("1")).toInt();
        const QString text = positional(arguments, 1, QStringLiteral("line"));
        for (int i = 0; i < count; ++i) {
            qWarning().noquote() << text << i;
        }
    } else if (scenario == QLatin1String("hold")) {
        // Keeps this instance alive, holding the per-run rotation lock, until the test writes a byte.
        qWarning("hold-first");
        fputs("HOLD-READY\n", stderr);
        fflush(stderr);
        char byte;
        while (read(STDIN_FILENO, &byte, 1) < 0 && errno == EINTR) { }
        qWarning("hold-last");
    } else if (scenario == QLatin1String("formats")) {
        qDebug("format-debug");
        qInfo("format-info");
        qWarning("format-warning");
        qCritical("format-critical");
    } else if (scenario == QLatin1String("fatal")) {
        qFatal("format-fatal");
    } else if (scenario == QLatin1String("components")) {
        libaWarning(QStringLiteral("from-liba"));
        libbWarning(QStringLiteral("from-libb"));
        int evaluations = 0;
        libaDebug(&evaluations);
        libbDebug(QStringLiteral("libb-debug"));
        report("EVALUATIONS", QByteArray::number(evaluations));
        QMessageLogger().warning(testShellCategory) << "from-exe-category";
        sonicWarning() << "from-exe-component";
        libcReverseDomainWarning(QStringLiteral("from-libc-reverse"));
        libcOtherCategoryWarning(QStringLiteral("from-libc-other"));
        libcPlainWarning(QStringLiteral("from-libc-plain"));
        qWarning("from-exe-plain");
        QMessageLogger(nullptr, 0, nullptr, "qml").warning() << "from-qml";
        const int before = Sonic::Logging::Internal::dladdrCallsForTesting();
        for (int i = 0; i < 5; ++i) {
            QMessageLogger().warning(cacheCheckCategory) << "cache-check" << i;
        }
        report("DLADDR_CALLS", QByteArray::number(Sonic::Logging::Internal::dladdrCallsForTesting() - before));
    } else if (scenario == QLatin1String("filtering")) {
        QMessageLogger().debug(qtTestCategory) << "qt-category-debug";
        QMessageLogger().info(qtTestCategory) << "qt-category-info";
        QMessageLogger().warning(qtTestCategory) << "qt-category-warning";
        fakeQtMessages(QStringLiteral("fake-qt"));
        QMessageLogger().debug(droppedCategory) << "dropped-category-debug";
        QMessageLogger().warning(droppedCategory) << "dropped-category-warning";
        qDebug("substring QRhiBufferData debug");
        qWarning("substring QRhiBufferData warning");
        qDebug("plain-debug-kept");
    } else if (scenario == QLatin1String("buildtype")) {
        debugBuildMessages(QStringLiteral("debug-lib"));
        releaseBuildMessages(QStringLiteral("release-lib"));
    } else if (scenario == QLatin1String("journal")) {
        qWarning("journal-single");
        qWarning("journal-first\njournal-second");
    } else if (scenario == QLatin1String("stall")) {
        const QString padding(180, QLatin1Char('p'));
        for (int i = 0; i < 2000; ++i) {
            qWarning().noquote() << QStringLiteral("stall-%1").arg(i, 4, 10, QLatin1Char('0')) << padding;
        }
        fputs("STALL-DONE\n", stderr);
        fflush(stderr);
        char byte;
        while (read(STDIN_FILENO, &byte, 1) < 0 && errno == EINTR) { }
        qWarning("stall-final");
    } else if (scenario == QLatin1String("bigline")) {
        qWarning().noquote() << QStringLiteral("bigline-") + QString(10000, QLatin1Char('x'));
    } else if (scenario == QLatin1String("static-dtor")) {
        static LogsWhenDestroyed logsWhenDestroyed;
        qWarning("before-exit");
    } else if (scenario == QLatin1String("setuid")) {
        sonicDebug() << "setuid-sonic-debug";
        qDebug("setuid-qdebug");
        sonicInfo() << "setuid-sonic-info";
    } else if (scenario == QLatin1String("injection")) {
        qWarning().noquote() << QStringLiteral("a\n2026-01-01T00:00:00.000 (EE) [fake] b\x01");
    } else if (scenario == QLatin1String("writefail")) {
        Sonic::Logging::Internal::setWriteErrorForTesting(ENOSPC);
        qWarning("lost-first");
        qWarning("lost-second");
        Sonic::Logging::Internal::advanceClockForTesting(61000);
        Sonic::Logging::Internal::setWriteErrorForTesting(0);
        qWarning("kept-after-pause");
    } else if (scenario == QLatin1String("rotate-size")) {
        const QString chunk(1000, QLatin1Char('s'));
        for (int i = 0; i < 2200; ++i) {
            qWarning().noquote() << chunk;
        }
        if (positional(arguments, 0) == QLatin1String("advance")) {
            Sonic::Logging::Internal::advanceClockForTesting(61000);
        }
        qWarning("after-big");
    } else if (scenario == QLatin1String("reopen")) {
        qWarning("reopen-one");
        const QString path = Sonic::Logging::logFilePath();
        QFile::rename(path, path + QStringLiteral(".moved"));
        Sonic::Logging::Internal::advanceClockForTesting(61000);
        qWarning("reopen-two");
    } else {
        fprintf(stderr, "unknown scenario %s\n", qPrintable(scenario));
        return 2;
    }
    return 0;
}
