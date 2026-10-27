// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

// Expected bytes are built with plain QByteArray concatenation.
#undef QT_USE_QSTRINGBUILDER

#include "logging.h"
#include "logging_p.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {
const QString helperPath = QStringLiteral(HELPER_PATH);
const QString helperName = QFileInfo(helperPath).fileName();

struct Result {
    int exitCode = -1;
    QProcess::ExitStatus exitStatus = QProcess::NormalExit;
    QByteArray out;
    QByteArray err;

    QByteArray value(const QByteArray& key) const
    {
        const QByteArray marker = "HELPER:" + key + '=';
        for (const QByteArray& line : err.split('\n')) {
            if (line.startsWith(marker)) {
                return line.mid(marker.size());
            }
        }
        return "<missing>";
    }
};

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QList<QByteArray> linesOf(const QByteArray& content)
{
    QList<QByteArray> lines = content.split('\n');
    if (!lines.isEmpty() && lines.last().isEmpty()) {
        lines.removeLast();
    }
    return lines;
}

int countOf(const QByteArray& content, const QByteArray& needle)
{
    return content.count(needle);
}

const QRegularExpression& lineFormat()
{
    static const QRegularExpression format(QStringLiteral("^\\S+ \\((DD|II|WW|EE)\\) \\[[^\\]]+\\] "));
    return format;
}

bool allLinesFormatted(const QByteArray& content)
{
    for (const QByteArray& line : linesOf(content)) {
        if (!lineFormat().match(QString::fromUtf8(line)).hasMatch()) {
            qWarning() << "unformatted line:" << line.left(200);
            return false;
        }
    }
    return true;
}

// The line in content that contains needle, or an empty array.
QByteArray lineWith(const QByteArray& content, const QByteArray& needle)
{
    for (const QByteArray& line : linesOf(content)) {
        if (line.contains(needle)) {
            return line;
        }
    }
    return {};
}

mode_t modeOf(const QString& path)
{
    struct stat st;
    return lstat(QFile::encodeName(path).constData(), &st) == 0 ? (st.st_mode & 07777) : 0;
}

QStringList entriesMatching(const QString& directory, const QString& pattern)
{
    return QDir(directory).entryList({pattern}, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
}

class Sandbox {
public:
    Sandbox()
    {
        QDir().mkpath(rootLogParent());
        chmod(QFile::encodeName(dir.path() + QStringLiteral("/root/var")).constData(), 0755);
        chmod(QFile::encodeName(rootLogParent()).constData(), 0755);
    }

    QString path(const QString& relative = {}) const
    {
        return relative.isEmpty() ? dir.path() : dir.path() + QLatin1Char('/') + relative;
    }
    QString stateDir() const
    {
        return path(QStringLiteral("state"));
    }
    QString userLog(const QString& name = helperName) const
    {
        return stateDir() + QStringLiteral("/sonic/") + name + QStringLiteral(".log");
    }
    QString rootLogParent() const
    {
        return path(QStringLiteral("root/var/log"));
    }
    QString rootLogDir() const
    {
        return path(QStringLiteral("root")) + Sonic::Logging::rootLogDirectory();
    }
    QString rootLog(const QString& name = helperName) const
    {
        return rootLogDir() + QLatin1Char('/') + name + QStringLiteral(".log");
    }
    QString journal() const
    {
        return path(QStringLiteral("journal.sock"));
    }

    QStringList userArgs() const
    {
        return {QStringLiteral("--state-dir"), stateDir(), QStringLiteral("--journal"), path(QStringLiteral("no-journal"))};
    }
    QStringList rootArgs() const
    {
        return {QStringLiteral("--root"),
            QStringLiteral("--fs-root"),
            path(QStringLiteral("root")),
            QStringLiteral("--trusted-uid"),
            QString::number(getuid()),
            QStringLiteral("--journal"),
            path(QStringLiteral("no-journal"))};
    }

    QProcessEnvironment environment() const
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char* name : {"JOURNAL_STREAM", "QT_LOGGING_RULES", "QT_LOGGING_CONF", "QT_MESSAGE_PATTERN", "XDG_STATE_HOME"}) {
            env.remove(QString::fromLatin1(name));
        }
        env.insert(QStringLiteral("HOME"), path(QStringLiteral("home")));
        return env;
    }

    QTemporaryDir dir;
};

Result run(const QStringList& arguments, const QProcessEnvironment& env, const std::function<void(QProcess&)>& setup = {}, const QString& program = helperPath)
{
    QProcess process;
    process.setProcessEnvironment(env);
    if (setup) {
        setup(process);
    }
    process.start(program, arguments);
    Result result;
    if (!process.waitForFinished(120000)) {
        process.kill();
        process.waitForFinished();
    }
    result.exitCode = process.exitCode();
    result.exitStatus = process.exitStatus();
    result.out = process.readAllStandardOutput();
    result.err = process.readAllStandardError();
    return result;
}

Result run(const Sandbox& sandbox, const QString& scenario, const QStringList& extra = {}, bool root = false)
{
    return run(QStringList{scenario} + (root ? sandbox.rootArgs() : sandbox.userArgs()) + extra, sandbox.environment());
}

QStringList environmentList(const QProcessEnvironment& env)
{
    return env.toStringList();
}

// posix_spawn with explicit descriptors and argv[0], for rows QProcess can't set up.
pid_t spawnRaw(const QByteArray& argv0, const QStringList& arguments, const QProcessEnvironment& env, int stdinFd, int stdoutFd, int stderrFd)
{
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, stdinFd, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stdoutFd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stderrFd, STDERR_FILENO);

    QList<QByteArray> argvStorage{argv0};
    for (const QString& argument : arguments) {
        argvStorage << QFile::encodeName(argument);
    }
    QList<char*> argv;
    for (QByteArray& argument : argvStorage) {
        argv << argument.data();
    }
    argv << nullptr;

    QList<QByteArray> envStorage;
    for (const QString& entry : environmentList(env)) {
        envStorage << entry.toLocal8Bit();
    }
    QList<char*> envp;
    for (QByteArray& entry : envStorage) {
        envp << entry.data();
    }
    envp << nullptr;

    pid_t pid = -1;
    const QByteArray program = QFile::encodeName(helperPath);
    if (posix_spawn(&pid, program.constData(), &actions, nullptr, argv.data(), envp.data()) != 0) {
        pid = -1;
    }
    posix_spawn_file_actions_destroy(&actions);
    return pid;
}

QByteArray drain(int fd, int timeoutMs)
{
    QByteArray data;
    char buffer[65536];
    for (;;) {
        pollfd fds{fd, POLLIN, 0};
        if (poll(&fds, 1, timeoutMs) <= 0) {
            break;
        }
        const ssize_t n = read(fd, buffer, sizeof buffer);
        if (n <= 0) {
            break;
        }
        data.append(buffer, n);
    }
    return data;
}

QByteArray readUntil(int fd, const QByteArray& marker, int timeoutMs)
{
    QByteArray data;
    QElapsedTimer timer;
    timer.start();
    char buffer[4096];
    while (!data.contains(marker) && timer.elapsed() < timeoutMs) {
        pollfd fds{fd, POLLIN, 0};
        if (poll(&fds, 1, 100) <= 0) {
            continue;
        }
        const ssize_t n = read(fd, buffer, sizeof buffer);
        if (n <= 0) {
            break;
        }
        data.append(buffer, n);
    }
    return data;
}

bool waitForChild(pid_t pid, int timeoutMs, int* status)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (waitpid(pid, status, WNOHANG) == pid) {
            return true;
        }
        QThread::msleep(10);
    }
    kill(pid, SIGKILL);
    waitpid(pid, status, 0);
    return false;
}

void writeComm(const QString& procRoot, qint64 pid, const QByteArray& comm)
{
    const QString directory = procRoot + QLatin1Char('/') + QString::number(pid);
    QDir().mkpath(directory);
    QFile file(directory + QStringLiteral("/comm"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(comm + '\n');
}

int bindJournal(const QString& path)
{
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const QByteArray encoded = QFile::encodeName(path);
    memcpy(address.sun_path, encoded.constData(), encoded.size());
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

QList<QByteArray> receiveAll(int fd)
{
    QList<QByteArray> datagrams;
    char buffer[65536];
    for (;;) {
        const ssize_t n = recv(fd, buffer, sizeof buffer, MSG_DONTWAIT);
        if (n < 0) {
            break;
        }
        datagrams << QByteArray(buffer, n);
    }
    return datagrams;
}
}

class LoggingTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void fileNameIds_data();
    void fileNameIds();
    void cleanBinaryName();
    void journalFieldEncoding();
    void lineFormatPerLevel();
    void fatalLine();
    void componentFatalLine();
    void userLocation();
    void symlinkName();
    void argvZeroIsCleaned();
    void setuidNameFromExecutable();
    void rootDirectoryCreated();
    void rootDirectoryModeReset();
    void rootDirectoryUntrusted_data();
    void rootDirectoryUntrusted();
    void rootDirectoryOtherOwner();
    void rootLogFileSymlink();
    void rootParentUntrusted();
    void rootDirectoryRace();
    void prepareRootLogDirectory();
    void rotationOnEveryRun();
    void rotationOnEveryRunWhileLocked();
    void rotationBySize_data();
    void rotationBySize();
    void inodeReopen();
    void components();
    void componentDebugRule();
    void filtering();
    void buildTypeDefault();
    void journald();
    void capture_data();
    void capture();
    void journalFailsPartway();
    void stalledPipe();
    void longLineThroughSlowPipe();
    void installAndRestore();
    void autoThenExplicit();
    void autoThenExplicitWithCustomHandler();
    void explicitThenAuto();
    void customHandlerKeptByAuto();
    void defaultHandlerReplacedByAuto();
    void beforeApplication();
    void latePluginLoad();
    void afterStaticDestruction();
    void threads();
    void concurrentProcesses();
    void dlopenWhileLogging();
    void forkedChild();
    void setuidEmulation();
    void injection();
    void noHome();
    void writeFailure();
};

void LoggingTest::fileNameIds_data()
{
    QTest::addColumn<QString>("file");
    QTest::addColumn<QString>("id");
    QTest::newRow("kf6") << QStringLiteral("/usr/lib64/libKF6KIOCore.so.6.31.0") << QStringLiteral("org.kde.kf6.kiocore");
    QTest::newRow("qt6") << QStringLiteral("libQt6Core.so.6") << QStringLiteral("org.qt-project.qt6.core");
    QTest::newRow("other library") << QStringLiteral("libfoo.so.1") << QStringLiteral("libfoo");
    QTest::newRow("plugin") << QStringLiteral("/usr/lib64/qt6/plugins/kf6/kded/kded_keyboard.so") << QStringLiteral("kded_keyboard");
}

void LoggingTest::fileNameIds()
{
    QFETCH(QString, file);
    QFETCH(QString, id);
    QCOMPARE(Sonic::Logging::Internal::idFromLibraryFileName(file), id);
}

void LoggingTest::cleanBinaryName()
{
    using Sonic::Logging::Internal::cleanBinaryName;
    QCOMPARE(cleanBinaryName(QStringLiteral("soniclogin-helper")), QStringLiteral("soniclogin-helper"));
    QCOMPARE(cleanBinaryName(QStringLiteral("../x\ny")), QStringLiteral(".._x_y"));
    QCOMPARE(cleanBinaryName(QString()), QStringLiteral("unknown"));
    QCOMPARE(cleanBinaryName(QStringLiteral(".")), QStringLiteral("unknown"));
    QCOMPARE(cleanBinaryName(QStringLiteral("..")), QStringLiteral("unknown"));
    QCOMPARE(cleanBinaryName(QString(100, QLatin1Char('a'))).size(), 64);
}

void LoggingTest::journalFieldEncoding()
{
    using Sonic::Logging::Internal::encodeJournalField;
    QCOMPARE(encodeJournalField("MESSAGE", "hello"), QByteArray("MESSAGE=hello\n"));
    const QByteArray expected = QByteArray("MESSAGE\n") + QByteArray("\x03\x00\x00\x00\x00\x00\x00\x00", 8) + QByteArray("a\nb\n");
    QCOMPARE(encodeJournalField("MESSAGE", "a\nb"), expected);
}

void LoggingTest::lineFormatPerLevel()
{
    Sandbox sandbox;
    const Result result = run(sandbox, QStringLiteral("formats"));
    QCOMPARE(result.exitCode, 0);
    const QByteArray content = readFile(sandbox.userLog());
    QVERIFY(allLinesFormatted(content));
    const QString stamp = QStringLiteral("^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d\\.\\d{3} ");
    const QList<QPair<QByteArray, QByteArray>> expected = {{"(DD)", "format-debug"}, {"(II)", "format-info"}, {"(WW)", "format-warning"}, {"(EE)", "format-critical"}};
    for (const auto& [level, message] : expected) {
        const QRegularExpression pattern(stamp + QRegularExpression::escape(QString::fromLatin1(level + " [") + helperName + QStringLiteral("] ") + QString::fromLatin1(message))
            + QLatin1Char('$'));
        QVERIFY2(pattern.match(QString::fromUtf8(lineWith(content, message))).hasMatch(), message.constData());
    }
}

void LoggingTest::fatalLine()
{
    Sandbox sandbox;
    const Result result = run(sandbox, QStringLiteral("fatal"));
    QCOMPARE(result.exitStatus, QProcess::CrashExit);
    QVERIFY(lineWith(readFile(sandbox.userLog()), "format-fatal").contains("(EE) [" + helperName.toLatin1() + "] format-fatal"));
}

void LoggingTest::componentFatalLine()
{
    // sonicFatal() logs a critical line under its component and aborts, even
    // when the rules switch every level of that component off.
    Sandbox sandbox;
    QProcessEnvironment env = sandbox.environment();
    env.insert(QStringLiteral("QT_LOGGING_RULES"), QStringLiteral("org.sonicde.test.app.*=false"));
    const Result result = run(QStringList{QStringLiteral("component-fatal")} + sandbox.userArgs(), env);
    QCOMPARE(result.exitStatus, QProcess::CrashExit);
    QVERIFY(lineWith(readFile(sandbox.userLog()), "from-component-fatal").contains("(EE) [org.sonicde.test.app] from-component-fatal"));
}

void LoggingTest::userLocation()
{
    Sandbox sandbox;
    const Result result = run(sandbox, QStringLiteral("log"));
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(QString::fromLocal8Bit(result.value("PATH")), sandbox.userLog());
    QCOMPARE(modeOf(sandbox.stateDir() + QStringLiteral("/sonic")), mode_t(0700));
    QCOMPARE(modeOf(sandbox.userLog()), mode_t(0600));
}

void LoggingTest::symlinkName()
{
    Sandbox sandbox;
    const QString link = sandbox.path(QStringLiteral("renamed-helper"));
    QVERIFY(QFile::link(helperPath, link));
    const Result result = run(QStringList{QStringLiteral("log")} + sandbox.userArgs(), sandbox.environment(), {}, link);
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(QString::fromLocal8Bit(result.value("PATH")), sandbox.userLog(QStringLiteral("renamed-helper")));
    QVERIFY(QFile::exists(sandbox.userLog(QStringLiteral("renamed-helper"))));
}

void LoggingTest::argvZeroIsCleaned()
{
    Sandbox sandbox;
    const int devNull = open("/dev/null", O_RDWR | O_CLOEXEC);
    const pid_t pid = spawnRaw("../x\ny", QStringList{QStringLiteral("log")} + sandbox.userArgs(), sandbox.environment(), devNull, devNull, devNull);
    QVERIFY(pid > 0);
    int status = 0;
    QVERIFY(waitForChild(pid, 30000, &status));
    close(devNull);
    // The directories are dropped, then the rest is cleaned.
    QVERIFY(QFile::exists(sandbox.userLog(QStringLiteral("x_y"))));
    QCOMPARE(QDir(sandbox.stateDir()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{QStringLiteral("sonic")});
}

void LoggingTest::setuidNameFromExecutable()
{
    Sandbox sandbox;
    const QString link = sandbox.path(QStringLiteral("chosen-by-caller"));
    QVERIFY(QFile::link(helperPath, link));
    const Result result = run(QStringList{QStringLiteral("log"), QStringLiteral("--setuid")} + sandbox.rootArgs(), sandbox.environment(), {}, link);
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(QString::fromLocal8Bit(result.value("PATH")), sandbox.rootLog());
}

void LoggingTest::rootDirectoryCreated()
{
    Sandbox sandbox;
    const Result result = run(sandbox, QStringLiteral("log"), {}, true);
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(modeOf(sandbox.rootLogDir()), mode_t(0755));
    QCOMPARE(QString::fromLocal8Bit(result.value("PATH")), sandbox.rootLog());
    QCOMPARE(modeOf(sandbox.rootLog()), mode_t(0644));
}

void LoggingTest::rootDirectoryModeReset()
{
    Sandbox sandbox;
    QVERIFY(QDir().mkpath(sandbox.rootLogDir()));
    chmod(QFile::encodeName(sandbox.rootLogDir()).constData(), 0750);
    QCOMPARE(run(sandbox, QStringLiteral("log"), {}, true).exitCode, 0);
    QCOMPARE(modeOf(sandbox.rootLogDir()), mode_t(0755));
    QVERIFY(QFile::exists(sandbox.rootLog()));
}

void LoggingTest::rootDirectoryUntrusted_data()
{
    QTest::addColumn<QString>("kind");
    QTest::newRow("group-writable") << QStringLiteral("group-writable");
    QTest::newRow("symlink") << QStringLiteral("symlink");
    QTest::newRow("regular file") << QStringLiteral("file");
}

void LoggingTest::rootDirectoryUntrusted()
{
    QFETCH(QString, kind);
    Sandbox sandbox;
    const QString target = sandbox.path(QStringLiteral("elsewhere"));
    if (kind == QLatin1String("group-writable")) {
        QVERIFY(QDir().mkpath(sandbox.rootLogDir()));
        chmod(QFile::encodeName(sandbox.rootLogDir()).constData(), 0775);
        QFile old(sandbox.rootLogDir() + QStringLiteral("/old.log"));
        QVERIFY(old.open(QIODevice::WriteOnly));
        old.write("old content\n");
    } else if (kind == QLatin1String("symlink")) {
        QVERIFY(QDir().mkpath(target));
        QVERIFY(QFile::link(target, sandbox.rootLogDir()));
    } else {
        QFile file(sandbox.rootLogDir());
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not a directory\n");
    }

    const Result result = run(sandbox, QStringLiteral("log"), {}, true);
    QCOMPARE(result.exitCode, 0);

    const QStringList aside = entriesMatching(sandbox.rootLogParent(), QStringLiteral("sonic.untrusted-*"));
    QCOMPARE(aside.size(), 1);
    const QString asidePath = sandbox.rootLogParent() + QLatin1Char('/') + aside.first();
    if (kind == QLatin1String("group-writable")) {
        QCOMPARE(readFile(asidePath + QStringLiteral("/old.log")), QByteArray("old content\n"));
    } else if (kind == QLatin1String("symlink")) {
        QVERIFY(QFileInfo(asidePath).isSymLink());
        QVERIFY(QDir(target).isEmpty());
    } else {
        QCOMPARE(readFile(asidePath), QByteArray("not a directory\n"));
    }
    QCOMPARE(modeOf(sandbox.rootLogDir()), mode_t(0755));
    QVERIFY(QFileInfo(sandbox.rootLogDir()).isDir() && !QFileInfo(sandbox.rootLogDir()).isSymLink());
    QVERIFY(result.err.contains("moved untrusted"));
    QVERIFY(linesOf(readFile(sandbox.rootLog())).value(0).contains("moved untrusted"));
}

void LoggingTest::rootDirectoryOtherOwner()
{
    Sandbox sandbox;
    QVERIFY(QDir().mkpath(sandbox.rootLogDir()));
    // The directory belongs to the test user; a different trusted uid makes it "owned by another user".
    QStringList arguments = QStringList{QStringLiteral("log")} + sandbox.rootArgs();
    arguments[arguments.indexOf(QStringLiteral("--trusted-uid")) + 1] = QString::number(getuid() + 1);
    arguments << QStringLiteral("--parent-trusted-uid") << QString::number(getuid());
    run(arguments, sandbox.environment());
    QCOMPARE(entriesMatching(sandbox.rootLogParent(), QStringLiteral("sonic.untrusted-*")).size(), 1);
    QVERIFY(QFileInfo(sandbox.rootLogDir()).isDir());
}

void LoggingTest::rootLogFileSymlink()
{
    Sandbox sandbox;
    QVERIFY(QDir().mkpath(sandbox.rootLogDir()));
    chmod(QFile::encodeName(sandbox.rootLogDir()).constData(), 0755);
    const QString target = sandbox.path(QStringLiteral("target.txt"));
    {
        QFile file(target);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("untouched\n");
    }
    QVERIFY(QFile::link(target, sandbox.rootLog()));
    QCOMPARE(run(sandbox, QStringLiteral("log"), {}, true).exitCode, 0);
    QCOMPARE(readFile(target), QByteArray("untouched\n"));
    QVERIFY(!QFileInfo(sandbox.rootLog()).isSymLink());
    QVERIFY(QFileInfo(sandbox.rootLog()).isFile());
    QCOMPARE(entriesMatching(sandbox.rootLogDir(), helperName + QStringLiteral(".log.untrusted-*")).size(), 1);
}

void LoggingTest::rootParentUntrusted()
{
    Sandbox sandbox;
    chmod(QFile::encodeName(sandbox.rootLogParent()).constData(), 0775);
    const Result result = run(sandbox, QStringLiteral("log"), {}, true);
    QCOMPARE(result.exitCode, 0);
    QVERIFY(!QFile::exists(sandbox.rootLogDir()));
    QVERIFY(result.err.contains("is not a root-owned directory"));
    QVERIFY(result.value("PATH").isEmpty());
}

void LoggingTest::rootDirectoryRace()
{
    Sandbox sandbox;
    QVERIFY(QDir().mkpath(sandbox.rootLogDir()));
    chmod(QFile::encodeName(sandbox.rootLogDir()).constData(), 0777);

    QProcess first;
    QProcess second;
    for (QProcess* process : {&first, &second}) {
        process->setProcessEnvironment(sandbox.environment());
        process->start(helperPath, QStringList{QStringLiteral("log"), QStringLiteral("1"), process == &first ? QStringLiteral("first") : QStringLiteral("second")} + sandbox.rootArgs());
    }
    QVERIFY(first.waitForFinished(30000));
    QVERIFY(second.waitForFinished(30000));
    QCOMPARE(entriesMatching(sandbox.rootLogParent(), QStringLiteral("sonic.untrusted-*")).size(), 1);
    const QByteArray content = readFile(sandbox.rootLog());
    QVERIFY(content.contains("first 0"));
    QVERIFY(content.contains("second 0"));
}

void LoggingTest::prepareRootLogDirectory()
{
    Sandbox sandbox;
    Result result = run(sandbox, QStringLiteral("prepare"), {}, true);
    QCOMPARE(result.value("PREPARE"), QByteArray("1"));
    QCOMPARE(modeOf(sandbox.rootLogDir()), mode_t(0755));

    struct stat before;
    QCOMPARE(lstat(QFile::encodeName(sandbox.rootLogDir()).constData(), &before), 0);
    result = run(sandbox, QStringLiteral("prepare"), {}, true);
    QCOMPARE(result.value("PREPARE"), QByteArray("1"));
    struct stat after;
    QCOMPARE(lstat(QFile::encodeName(sandbox.rootLogDir()).constData(), &after), 0);
    QCOMPARE(after.st_ino, before.st_ino);
    QCOMPARE(after.st_mode, before.st_mode);
    QVERIFY(entriesMatching(sandbox.rootLogParent(), QStringLiteral("sonic.untrusted-*")).isEmpty());

    chmod(QFile::encodeName(sandbox.rootLogDir()).constData(), 0777);
    result = run(sandbox, QStringLiteral("prepare"), {}, true);
    QCOMPARE(result.value("PREPARE"), QByteArray("1"));
    QCOMPARE(entriesMatching(sandbox.rootLogParent(), QStringLiteral("sonic.untrusted-*")).size(), 1);
    QCOMPARE(modeOf(sandbox.rootLogDir()), mode_t(0755));

    Sandbox notRoot;
    QStringList arguments = notRoot.rootArgs();
    arguments.removeAll(QStringLiteral("--root"));
    result = run(QStringList{QStringLiteral("prepare")} + arguments, notRoot.environment());
    QCOMPARE(result.value("PREPARE"), QByteArray("0"));
    QVERIFY(!QFile::exists(notRoot.rootLogDir()));
}

void LoggingTest::rotationOnEveryRun()
{
    Sandbox sandbox;
    for (const QString& name : {QStringLiteral("run1"), QStringLiteral("run2"), QStringLiteral("run3")}) {
        QCOMPARE(run(sandbox, QStringLiteral("log"), {QStringLiteral("--rotate-on-startup"), QStringLiteral("1"), name}).exitCode, 0);
    }
    const QByteArray current = readFile(sandbox.userLog());
    const QByteArray last = readFile(sandbox.userLog() + QStringLiteral(".last"));
    QVERIFY(current.contains("run3 0") && !current.contains("run2") && !current.contains("run1"));
    QVERIFY(last.contains("run2 0") && !last.contains("run1") && !last.contains("run3"));

    // Size never rotates these binaries.
    Sandbox big;
    QCOMPARE(run(big, QStringLiteral("rotate-size"), {QStringLiteral("--rotate-on-startup"), QStringLiteral("advance")}).exitCode, 0);
    QVERIFY(!QFile::exists(big.userLog() + QStringLiteral(".last")));
    QVERIFY(QFileInfo(big.userLog()).size() > 2097152);
}

void LoggingTest::rotationOnEveryRunWhileLocked()
{
    Sandbox sandbox;
    QCOMPARE(run(sandbox, QStringLiteral("log"), {QStringLiteral("--rotate-on-startup"), QStringLiteral("1"), QStringLiteral("earlier")}).exitCode, 0);

    QProcess holder;
    holder.setProcessEnvironment(sandbox.environment());
    holder.start(helperPath, QStringList{QStringLiteral("hold"), QStringLiteral("--rotate-on-startup")} + sandbox.userArgs());
    QVERIFY(holder.waitForStarted());
    holder.setReadChannel(QProcess::StandardError);
    QByteArray holderErr;
    while (!holderErr.contains("HOLD-READY") && holder.waitForReadyRead(10000)) {
        holderErr += holder.readAllStandardError();
    }
    QVERIFY(holderErr.contains("HOLD-READY"));

    QCOMPARE(run(sandbox, QStringLiteral("log"), {QStringLiteral("--rotate-on-startup"), QStringLiteral("1"), QStringLiteral("while-locked")}).exitCode, 0);
    holder.write("x");
    QVERIFY(holder.waitForFinished(10000));

    const QByteArray current = readFile(sandbox.userLog());
    QVERIFY(current.contains("hold-first"));
    QVERIFY(current.contains("while-locked 0"));
    QVERIFY(current.contains("hold-last"));
    QVERIFY(readFile(sandbox.userLog() + QStringLiteral(".last")).contains("earlier 0"));
}

void LoggingTest::rotationBySize_data()
{
    QTest::addColumn<bool>("root");
    QTest::newRow("user") << false;
    QTest::newRow("root") << true;
}

void LoggingTest::rotationBySize()
{
    QFETCH(bool, root);
    Sandbox sandbox;
    const QString log = root ? sandbox.rootLog() : sandbox.userLog();
    for (const QString& name : {QStringLiteral("append1"), QStringLiteral("append2"), QStringLiteral("append3")}) {
        QCOMPARE(run(sandbox, QStringLiteral("log"), {QStringLiteral("1"), name}, root).exitCode, 0);
    }
    const QByteArray appended = readFile(log);
    QVERIFY(appended.contains("append1 0") && appended.contains("append2 0") && appended.contains("append3 0"));
    QVERIFY(!QFile::exists(log + QStringLiteral(".last")));

    // Under 60 s: no rotation, however large.
    QCOMPARE(run(sandbox, QStringLiteral("rotate-size"), {}, root).exitCode, 0);
    QVERIFY(!QFile::exists(log + QStringLiteral(".last")));
    QVERIFY(readFile(log).contains("after-big"));

    // Past 60 s: the next message rotates.
    Sandbox advanced;
    const QString advancedLog = root ? advanced.rootLog() : advanced.userLog();
    QCOMPARE(run(advanced, QStringLiteral("rotate-size"), {QStringLiteral("advance")}, root).exitCode, 0);
    const QByteArray last = readFile(advancedLog + QStringLiteral(".last"));
    QVERIFY(last.size() > 2097152);
    QVERIFY(!last.contains("after-big"));
    const QByteArray current = readFile(advancedLog);
    QVERIFY(current.contains("after-big"));
    QVERIFY(current.size() < 4096);
}

void LoggingTest::inodeReopen()
{
    Sandbox sandbox;
    QCOMPARE(run(sandbox, QStringLiteral("reopen")).exitCode, 0);
    const QByteArray moved = readFile(sandbox.userLog() + QStringLiteral(".moved"));
    const QByteArray current = readFile(sandbox.userLog());
    QVERIFY(moved.contains("reopen-one") && !moved.contains("reopen-two"));
    QVERIFY(current.contains("reopen-two"));
}

void LoggingTest::components()
{
    Sandbox sandbox;
    const Result result = run(sandbox, QStringLiteral("components"));
    QCOMPARE(result.exitCode, 0);
    const QByteArray content = readFile(sandbox.userLog());
    QVERIFY(allLinesFormatted(content));
    const QByteArray helper = helperName.toLatin1();
    const QList<QPair<QByteArray, QByteArray>> expected = {
        {"from-liba", "[org.sonicde.test.liba]"},
        {"from-libb", "[org.sonicde.test.libb]"},
        {"from-exe-category", "[kde.testshell]"},
        {"from-exe-component", "[org.sonicde.test.app]"},
        {"from-libc-reverse", "[org.example.test.lib]"},
        {"from-libc-other", "[sonicloggingtestlibc]"},
        {"from-libc-plain", "[" + helper + "]"},
        {"from-exe-plain", "[" + helper + "]"},
        {"from-qml", "[" + helper + "]"},
    };
    for (const auto& [message, bracket] : expected) {
        QVERIFY2(lineWith(content, message).contains(bracket + ' ' + message), (message + " should carry " + bracket).constData());
    }
    QCOMPARE(result.value("EVALUATIONS"), QByteArray("1"));
    QCOMPARE(result.value("DLADDR_CALLS"), QByteArray("1"));
}

void LoggingTest::componentDebugRule()
{
    Sandbox sandbox;
    QProcessEnvironment env = sandbox.environment();
    env.insert(QStringLiteral("QT_LOGGING_RULES"), QStringLiteral("org.sonicde.test.liba.debug=false"));
    const Result result = run(QStringList{QStringLiteral("components")} + sandbox.userArgs(), env);
    QCOMPARE(result.exitCode, 0);
    const QByteArray content = readFile(sandbox.userLog());
    QVERIFY(!content.contains("liba debug"));
    QCOMPARE(result.value("EVALUATIONS"), QByteArray("0"));
    QVERIFY(content.contains("[org.sonicde.test.libb] libb-debug"));
    QVERIFY(content.contains("from-liba"));
}

void LoggingTest::filtering()
{
    Sandbox sandbox;
    QProcessEnvironment env = sandbox.environment();
    env.insert(QStringLiteral("QT_LOGGING_RULES"), QStringLiteral("qt.test.debug=true"));
    QCOMPARE(run(QStringList{QStringLiteral("filtering")} + sandbox.userArgs(), env).exitCode, 0);
    const QByteArray content = readFile(sandbox.userLog());
    for (const QByteArray dropped : {"qt-category-debug", "qt-category-info", "fake-qt debug", "fake-qt info", "dropped-category-debug", "QRhiBufferData debug"}) {
        QVERIFY2(!content.contains(dropped), dropped.constData());
    }
    for (const QByteArray kept : {"qt-category-warning", "fake-qt warning", "dropped-category-warning", "QRhiBufferData warning", "plain-debug-kept"}) {
        QVERIFY2(content.contains(kept), kept.constData());
    }
}

void LoggingTest::buildTypeDefault()
{
    Sandbox sandbox;
    QCOMPARE(run(sandbox, QStringLiteral("buildtype")).exitCode, 0);
    QByteArray content = readFile(sandbox.userLog());
    QVERIFY(content.contains("[org.sonicde.test.debug] debug-lib debug"));
    QVERIFY(content.contains("[org.sonicde.test.debug] debug-lib info"));
    QVERIFY(!content.contains("release-lib debug"));
    QVERIFY(content.contains("[org.sonicde.test.release] release-lib info"));

    Sandbox enabled;
    QProcessEnvironment env = enabled.environment();
    env.insert(QStringLiteral("QT_LOGGING_RULES"), QStringLiteral("org.sonicde.test.release.debug=true"));
    QCOMPARE(run(QStringList{QStringLiteral("buildtype")} + enabled.userArgs(), env).exitCode, 0);
    content = readFile(enabled.userLog());
    QVERIFY(content.contains("[org.sonicde.test.release] release-lib debug"));
}

void LoggingTest::journald()
{
    Sandbox sandbox;
    const int journal = bindJournal(sandbox.journal());
    QVERIFY(journal >= 0);
    const Result result = run(QStringList{QStringLiteral("journal"), QStringLiteral("--state-dir"), sandbox.stateDir(), QStringLiteral("--journal"), sandbox.journal()},
        sandbox.environment());
    QCOMPARE(result.exitCode, 0);
    const QList<QByteArray> datagrams = receiveAll(journal);
    close(journal);

    QByteArray single;
    QByteArray multi;
    for (const QByteArray& datagram : datagrams) {
        if (datagram.contains("MESSAGE=journal-single\n")) {
            single = datagram;
        } else if (datagram.contains("journal-first\njournal-second")) {
            multi = datagram;
        }
    }
    QVERIFY(!single.isEmpty());
    QVERIFY(single.contains("SYSLOG_IDENTIFIER=" + helperName.toLatin1() + '\n'));
    QVERIFY(single.contains("PRIORITY=4\n"));
    // The executable's own messages carry its declared component.
    QVERIFY(single.contains("SONIC_COMPONENT=org.sonicde.test.app\n"));
    const QByteArray text = "journal-first\njournal-second";
    QVERIFY(multi.contains(QByteArray("MESSAGE\n") + char(text.size()) + QByteArray(7, '\0') + text + '\n'));
    QVERIFY(readFile(sandbox.userLog()).contains("journal-single"));
}

void LoggingTest::capture_data()
{
    QTest::addColumn<QString>("stdoutKind"); // pipe, file, xsession, xsession-old
    QTest::addColumn<QByteArray>("parentComm");
    QTest::addColumn<qint64>("parentPid");
    QTest::addColumn<QByteArray>("pid1Comm");
    QTest::addColumn<QString>("journalStream"); // none, match, other, garbage
    QTest::addColumn<QString>("expected"); // stdout, journal, stderr, file

    QTest::newRow("runsv pipe") << "pipe" << QByteArray("runsv") << qint64(4242) << QByteArray("init") << "none" << "stdout";
    QTest::newRow("s6-supervise pipe") << "pipe" << QByteArray("s6-supervise") << qint64(4242) << QByteArray("init") << "none" << "stdout";
    QTest::newRow("supervise-daemon pipe") << "pipe" << QByteArray("supervise-daemon") << qint64(4242) << QByteArray("init") << "none" << "stdout";
    QTest::newRow("supervise-daemon file") << "file" << QByteArray("supervise-daemon") << qint64(4242) << QByteArray("init") << "none" << "stdout";
    QTest::newRow("dinit pid 1 file") << "file" << QByteArray("dinit") << qint64(1) << QByteArray("dinit") << "none" << "stdout";
    QTest::newRow("xsession-errors") << "xsession" << QByteArray("bash") << qint64(4242) << QByteArray("init") << "none" << "stdout";
    QTest::newRow("journal stream") << "pipe" << QByteArray("systemd") << qint64(4242) << QByteArray("init") << "match" << "journal";
    QTest::newRow("journal stream without socket") << "pipe" << QByteArray("systemd") << qint64(4242) << QByteArray("init") << "match-nosocket" << "stderr";

    QTest::newRow("shell pipe") << "pipe" << QByteArray("bash") << qint64(4242) << QByteArray("init") << "none" << "file";
    QTest::newRow("tee pipe") << "pipe" << QByteArray("tee") << qint64(4242) << QByteArray("init") << "none" << "file";
    QTest::newRow("shell redirect") << "file" << QByteArray("bash") << qint64(4242) << QByteArray("init") << "none" << "file";
    QTest::newRow("runsv with a file") << "file" << QByteArray("runsv") << qint64(4242) << QByteArray("init") << "none" << "file";
    QTest::newRow("dinit not pid 1") << "file" << QByteArray("dinit") << qint64(4242) << QByteArray("dinit") << "none" << "file";
    QTest::newRow("xsession-errors.old") << "xsession-old" << QByteArray("bash") << qint64(4242) << QByteArray("init") << "none" << "file";
    QTest::newRow("journal stream mismatch") << "pipe" << QByteArray("bash") << qint64(4242) << QByteArray("init") << "other" << "file";
    QTest::newRow("journal stream garbage") << "pipe" << QByteArray("bash") << qint64(4242) << QByteArray("init") << "garbage" << "file";
    QTest::newRow("unreadable parent comm") << "pipe" << QByteArray() << qint64(4242) << QByteArray("init") << "none" << "file";
}

void LoggingTest::capture()
{
    QFETCH(QString, stdoutKind);
    QFETCH(QByteArray, parentComm);
    QFETCH(qint64, parentPid);
    QFETCH(QByteArray, pid1Comm);
    QFETCH(QString, journalStream);
    QFETCH(QString, expected);

    Sandbox sandbox;
    const QString procRoot = sandbox.path(QStringLiteral("proc"));
    QDir().mkpath(procRoot);
    if (!parentComm.isEmpty()) {
        writeComm(procRoot, parentPid, parentComm);
    }
    if (parentPid != 1) {
        writeComm(procRoot, 1, pid1Comm);
    }

    int journal = -1;
    if (journalStream == QLatin1String("match")) {
        journal = bindJournal(sandbox.journal());
        QVERIFY(journal >= 0);
    }

    QString stdoutPath;
    if (stdoutKind == QLatin1String("file")) {
        stdoutPath = sandbox.path(QStringLiteral("out.log"));
    } else if (stdoutKind == QLatin1String("xsession")) {
        stdoutPath = sandbox.path(QStringLiteral(".xsession-errors"));
    } else if (stdoutKind == QLatin1String("xsession-old")) {
        stdoutPath = sandbox.path(QStringLiteral("xsession-errors.old"));
    }

    // stderr goes to a file whose device and inode the test knows in advance.
    const QString stderrPath = sandbox.path(QStringLiteral("stderr.txt"));
    QVERIFY(QFile(stderrPath).open(QIODevice::WriteOnly));
    struct stat stderrStat;
    QCOMPARE(stat(QFile::encodeName(stderrPath).constData(), &stderrStat), 0);

    QProcessEnvironment env = sandbox.environment();
    if (journalStream.startsWith(QLatin1String("match"))) {
        env.insert(QStringLiteral("JOURNAL_STREAM"), QStringLiteral("%1:%2").arg(stderrStat.st_dev).arg(stderrStat.st_ino));
    } else if (journalStream == QLatin1String("other")) {
        env.insert(QStringLiteral("JOURNAL_STREAM"), QStringLiteral("%1:%2").arg(stderrStat.st_dev).arg(stderrStat.st_ino + 1));
    } else if (journalStream == QLatin1String("garbage")) {
        env.insert(QStringLiteral("JOURNAL_STREAM"), QStringLiteral("not-a-stream"));
    }

    const QStringList arguments{QStringLiteral("log"),
        QStringLiteral("3"),
        QStringLiteral("captured-line"),
        QStringLiteral("--state-dir"),
        sandbox.stateDir(),
        QStringLiteral("--journal"),
        journal >= 0 ? sandbox.journal() : sandbox.path(QStringLiteral("no-journal")),
        QStringLiteral("--proc-root"),
        procRoot,
        QStringLiteral("--parent-pid"),
        QString::number(parentPid)};
    const Result result = run(arguments, env, [&](QProcess& process) {
        if (!stdoutPath.isEmpty()) {
            process.setStandardOutputFile(stdoutPath);
        }
        process.setStandardErrorFile(stderrPath);
    });
    QCOMPARE(result.exitCode, 0);

    const QByteArray out = stdoutPath.isEmpty() ? result.out : readFile(stdoutPath);
    const QByteArray err = readFile(stderrPath);
    const QByteArray ownFile = readFile(sandbox.userLog());
    const QByteArray reportedPath = Result{0, QProcess::NormalExit, {}, err}.value("PATH");

    if (expected == QLatin1String("file")) {
        QVERIFY(ownFile.contains("captured-line 0"));
        QCOMPARE(QString::fromLocal8Bit(reportedPath), sandbox.userLog());
        QVERIFY(!out.contains("captured-line"));
    } else {
        QVERIFY(!QFile::exists(sandbox.userLog()));
        QVERIFY(reportedPath.isEmpty());
        if (expected == QLatin1String("stdout")) {
            QVERIFY(lineWith(out, "captured-line 2").contains("(WW) [" + helperName.toLatin1() + "] captured-line 2"));
            QVERIFY(allLinesFormatted(out));
            QVERIFY(out.contains("output captured by"));
        } else if (expected == QLatin1String("journal")) {
            const QList<QByteArray> datagrams = receiveAll(journal);
            int captured = 0;
            for (const QByteArray& datagram : datagrams) {
                captured += datagram.contains("MESSAGE=captured-line") ? 1 : 0;
            }
            QCOMPARE(captured, 3);
            QVERIFY(!err.contains("captured-line"));
        } else {
            QVERIFY(lineWith(err, "captured-line 2").contains("(WW) [" + helperName.toLatin1() + "] captured-line 2"));
        }
    }
    if (journal >= 0) {
        close(journal);
    }
}

void LoggingTest::journalFailsPartway()
{
    Sandbox sandbox;
    const int journal = bindJournal(sandbox.journal());
    QVERIFY(journal >= 0);
    const QString stderrPath = sandbox.path(QStringLiteral("stderr.txt"));
    QVERIFY(QFile(stderrPath).open(QIODevice::WriteOnly));
    struct stat stderrStat;
    QCOMPARE(stat(QFile::encodeName(stderrPath).constData(), &stderrStat), 0);
    QProcessEnvironment env = sandbox.environment();
    env.insert(QStringLiteral("JOURNAL_STREAM"), QStringLiteral("%1:%2").arg(stderrStat.st_dev).arg(stderrStat.st_ino));

    // The start-up line and the first message reach journald; the rest fall back to stderr.
    const QStringList arguments{QStringLiteral("log"),
        QStringLiteral("3"),
        QStringLiteral("partway"),
        QStringLiteral("--state-dir"),
        sandbox.stateDir(),
        QStringLiteral("--journal"),
        sandbox.journal(),
        QStringLiteral("--journal-fail-after"),
        QStringLiteral("2")};
    const Result result = run(arguments, env, [&](QProcess& process) {
        process.setStandardErrorFile(stderrPath);
    });
    QCOMPARE(result.exitCode, 0);
    int inJournal = 0;
    for (const QByteArray& datagram : receiveAll(journal)) {
        inJournal += datagram.contains("MESSAGE=partway") ? 1 : 0;
    }
    close(journal);
    const QByteArray err = readFile(stderrPath);
    QCOMPARE(inJournal, 1);
    QVERIFY(err.contains("[" + helperName.toLatin1() + "] partway 1"));
    QVERIFY(err.contains("[" + helperName.toLatin1() + "] partway 2"));
    QVERIFY(!QFile::exists(sandbox.userLog()));
}

void LoggingTest::stalledPipe()
{
    Sandbox sandbox;
    const QString procRoot = sandbox.path(QStringLiteral("proc"));
    writeComm(procRoot, 4242, "runsv");

    int outPipe[2];
    int errPipe[2];
    int inPipe[2];
    QCOMPARE(pipe2(outPipe, O_CLOEXEC), 0);
    QCOMPARE(pipe2(errPipe, O_CLOEXEC), 0);
    QCOMPARE(pipe2(inPipe, O_CLOEXEC), 0);
    const QStringList arguments = QStringList{QStringLiteral("stall"), QStringLiteral("--proc-root"), procRoot, QStringLiteral("--parent-pid"), QStringLiteral("4242")}
        + sandbox.userArgs();
    QElapsedTimer timer;
    timer.start();
    const pid_t pid = spawnRaw(QFile::encodeName(helperPath), arguments, sandbox.environment(), inPipe[0], outPipe[1], errPipe[1]);
    QVERIFY(pid > 0);
    close(outPipe[1]);
    close(errPipe[1]);
    close(inPipe[0]);

    // Nobody reads stdout until the child has logged all 2000 lines.
    const QByteArray err = readUntil(errPipe[0], "STALL-DONE", 2000 * 100 + 30000);
    QVERIFY(err.contains("STALL-DONE"));
    // Only the first dropped line waits; the rest are dropped without waiting.
    QVERIFY2(timer.elapsed() < 10000, QByteArray::number(timer.elapsed()).constData());
    QCOMPARE(err.count("output pipe is full"), 1);

    QByteArray out = drain(outPipe[0], 200);
    QVERIFY(write(inPipe[1], "x", 1) == 1);
    close(inPipe[1]);
    int status = 0;
    out += drain(outPipe[0], 5000);
    QVERIFY(waitForChild(pid, 10000, &status));
    out += drain(outPipe[0], 100);
    close(outPipe[0]);
    close(errPipe[0]);

    QVERIFY(allLinesFormatted(out));
    int delivered = 0;
    for (const QByteArray& line : linesOf(out)) {
        delivered += line.contains("] stall-") && !line.contains("stall-final") ? 1 : 0;
    }
    const QByteArray notice = lineWith(out, "log lines dropped: output pipe full");
    QVERIFY(!notice.isEmpty());
    const QRegularExpressionMatch match = QRegularExpression(QStringLiteral("\\] (\\d+) log lines dropped")).match(QString::fromUtf8(notice));
    QVERIFY(match.hasMatch());
    QCOMPARE(match.captured(1).toInt(), 2000 - delivered);
    QVERIFY(delivered < 2000);
    QVERIFY(out.contains("stall-final"));
}

void LoggingTest::longLineThroughSlowPipe()
{
    Sandbox sandbox;
    const QString procRoot = sandbox.path(QStringLiteral("proc"));
    writeComm(procRoot, 4242, "runsv");
    int outPipe[2];
    QCOMPARE(pipe2(outPipe, O_CLOEXEC), 0);
    fcntl(outPipe[0], F_SETPIPE_SZ, 4096);
    const int devNull = open("/dev/null", O_RDWR | O_CLOEXEC);
    const QStringList arguments = QStringList{QStringLiteral("bigline"), QStringLiteral("--proc-root"), procRoot, QStringLiteral("--parent-pid"), QStringLiteral("4242")}
        + sandbox.userArgs();
    const pid_t pid = spawnRaw(QFile::encodeName(helperPath), arguments, sandbox.environment(), devNull, outPipe[1], devNull);
    QVERIFY(pid > 0);
    close(outPipe[1]);
    close(devNull);

    QByteArray out;
    char buffer[2048];
    for (;;) {
        QThread::msleep(20);
        pollfd fds{outPipe[0], POLLIN, 0};
        if (poll(&fds, 1, 5000) <= 0) {
            break;
        }
        const ssize_t n = read(outPipe[0], buffer, sizeof buffer);
        if (n <= 0) {
            break;
        }
        out.append(buffer, n);
    }
    close(outPipe[0]);
    int status = 0;
    QVERIFY(waitForChild(pid, 10000, &status));
    QVERIFY(lineWith(out, "bigline-").endsWith(QByteArray(10000, 'x')));
    QVERIFY(allLinesFormatted(out));
}

void LoggingTest::installAndRestore()
{
    Sandbox sandbox;
    const Result result = run(sandbox, QStringLiteral("restore"));
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.value("FIRST"), QByteArray("default"));
    QCOMPARE(result.value("SECOND"), QByteArray("null"));
    QCOMPARE(result.value("RESTORED"), QByteArray("default"));
    QCOMPARE(result.value("REINSTALLED"), QByteArray("sonic"));
    QVERIFY(readFile(sandbox.userLog()).contains("after-reinstall"));
}

void LoggingTest::autoThenExplicit()
{
    Sandbox sandbox;
    for (int i = 0; i < 2; ++i) {
        const Result result = run(sandbox, QStringLiteral("auto-then-explicit"), {QStringLiteral("--rotate-on-startup")});
        QCOMPARE(result.exitCode, 0);
        QCOMPARE(result.value("PREVIOUS"), QByteArray("default"));
        QCOMPARE(result.value("PLUGIN_LOADS"), QByteArray("1"));
    }
    QCOMPARE(countOf(readFile(sandbox.userLog()), "auto-then-explicit-run"), 1);
    QCOMPARE(countOf(readFile(sandbox.userLog() + QStringLiteral(".last")), "auto-then-explicit-run"), 1);
}

void LoggingTest::autoThenExplicitWithCustomHandler()
{
    Sandbox sandbox;
    const Result result = run(sandbox, QStringLiteral("auto-then-explicit"), {QStringLiteral("custom-first")});
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.value("PREVIOUS"), QByteArray("custom"));
}

void LoggingTest::explicitThenAuto()
{
    Sandbox sandbox;
    for (int i = 0; i < 2; ++i) {
        const Result result = run(sandbox, QStringLiteral("explicit-then-auto"), {QStringLiteral("--rotate-on-startup")});
        QCOMPARE(result.exitCode, 0);
        QCOMPARE(result.value("CURRENT"), QByteArray("sonic"));
        QCOMPARE(result.value("PLUGIN_LOADS"), QByteArray("1"));
    }
    QCOMPARE(countOf(readFile(sandbox.userLog()), "explicit-then-auto-run"), 1);
    QCOMPARE(countOf(readFile(sandbox.userLog() + QStringLiteral(".last")), "explicit-then-auto-run"), 1);
}

void LoggingTest::customHandlerKeptByAuto()
{
    Sandbox sandbox;
    const Result result = run(sandbox, QStringLiteral("custom-then-auto"));
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.value("CURRENT"), QByteArray("custom"));
    QVERIFY(result.value("CUSTOM_MESSAGES").toInt() >= 1);
    QVERIFY(!readFile(sandbox.userLog()).contains("to-the-custom-handler"));
}

void LoggingTest::defaultHandlerReplacedByAuto()
{
    Sandbox sandbox;
    const Result result = run(sandbox, QStringLiteral("default-then-auto"));
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.value("CURRENT"), QByteArray("sonic"));
    QCOMPARE(result.value("PLUGIN_LOADS"), QByteArray("1"));
    QVERIFY(readFile(sandbox.userLog()).contains("from-the-auto-install"));
}

void LoggingTest::beforeApplication()
{
    Sandbox sandbox;
    QCOMPARE(run(sandbox, QStringLiteral("before-app")).exitCode, 0);
    const QByteArray content = readFile(sandbox.userLog());
    const qsizetype before = content.indexOf("before-the-application");
    const qsizetype after = content.indexOf("after-the-application");
    QVERIFY(before >= 0);
    QVERIFY(after > before);
}

void LoggingTest::latePluginLoad()
{
    Sandbox sandbox;
    const Result result = run(sandbox, QStringLiteral("late-plugin"));
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.value("PLUGIN_LOADS"), QByteArray("1"));
    QCOMPARE(result.value("CURRENT"), QByteArray("sonic"));
}

void LoggingTest::afterStaticDestruction()
{
    Sandbox sandbox;
    const Result result = run(sandbox, QStringLiteral("static-dtor"));
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 0);
    QVERIFY(readFile(sandbox.userLog()).contains("from-static-destructor"));
}

void LoggingTest::threads()
{
    Sandbox sandbox;
    const Result result = run(sandbox, QStringLiteral("threads"));
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.value("THREAD_LINES"), QByteArray("80000"));
    QCOMPARE(result.value("THREAD_MALFORMED"), QByteArray("0"));
    QCOMPARE(result.value("THREAD_DUPLICATES"), QByteArray("0"));
}

void LoggingTest::concurrentProcesses()
{
    Sandbox sandbox;
    QProcess first;
    QProcess second;
    for (QProcess* process : {&first, &second}) {
        process->setProcessEnvironment(sandbox.environment());
        process->start(helperPath, QStringList{QStringLiteral("log"), QStringLiteral("5000"), process == &first ? QStringLiteral("procA") : QStringLiteral("procB")} + sandbox.userArgs());
    }
    QVERIFY(first.waitForFinished(60000));
    QVERIFY(second.waitForFinished(60000));
    const QByteArray content = readFile(sandbox.userLog());
    QVERIFY(allLinesFormatted(content));
    QCOMPARE(countOf(content, "] procA "), 5000);
    QCOMPARE(countOf(content, "] procB "), 5000);
}

void LoggingTest::dlopenWhileLogging()
{
    Sandbox sandbox;
    QCOMPARE(run(sandbox, QStringLiteral("dlopen-threads")).exitCode, 0);
    const QByteArray content = readFile(sandbox.userLog());
    QVERIFY(content.contains("[org.sonicde.test.dlopen] from-dlopen-library"));
    QVERIFY(allLinesFormatted(content));
}

void LoggingTest::forkedChild()
{
    Sandbox sandbox;
    const Result result = run(sandbox, QStringLiteral("fork"));
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.value("CHILD_EXITED"), QByteArray("1"));
    QVERIFY(readFile(sandbox.userLog()).contains("from-forked-child"));
}

void LoggingTest::setuidEmulation()
{
    Sandbox sandbox;
    QProcessEnvironment env = sandbox.environment();
    env.insert(QStringLiteral("QT_LOGGING_RULES"), QStringLiteral("*.debug=true"));
    const Result result = run(QStringList{QStringLiteral("setuid"), QStringLiteral("--setuid")} + sandbox.rootArgs(), env);
    QCOMPARE(result.exitCode, 0);
    const QByteArray content = readFile(sandbox.rootLog());
    QVERIFY(!content.contains("setuid-sonic-debug"));
    QVERIFY(!content.contains("setuid-qdebug"));
    QVERIFY(content.contains("setuid-sonic-info"));
}

void LoggingTest::injection()
{
    Sandbox sandbox;
    QCOMPARE(run(sandbox, QStringLiteral("injection")).exitCode, 0);
    const QList<QByteArray> lines = linesOf(readFile(sandbox.userLog()));
    qsizetype index = -1;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        if (lines.at(i).endsWith("] a")) {
            index = i;
        }
    }
    QVERIFY(index >= 0);
    QVERIFY(index + 1 < lines.size());
    QCOMPARE(lines.at(index + 1), QByteArray("    2026-01-01T00:00:00.000 (EE) [fake] b\\x01"));
}

void LoggingTest::noHome()
{
    Sandbox sandbox;
    QProcessEnvironment env = sandbox.environment();
    env.remove(QStringLiteral("HOME"));
    const Result result = run(QStringList{QStringLiteral("log"), QStringLiteral("1"), QStringLiteral("homeless"), QStringLiteral("--journal"), sandbox.path(QStringLiteral("no-journal"))}, env);
    QCOMPARE(result.exitCode, 0);
    QVERIFY(result.value("PATH").isEmpty());
    QVERIFY(result.err.contains("no usable home directory"));
    QVERIFY(result.err.contains("homeless 0"));
}

void LoggingTest::writeFailure()
{
    Sandbox sandbox;
    const Result result = run(sandbox, QStringLiteral("writefail"));
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(result.err.count("writing the log failed"), 1);
    const QByteArray content = readFile(sandbox.userLog());
    QVERIFY(!content.contains("lost-first"));
    QVERIFY(!content.contains("lost-second"));
    QVERIFY(content.contains("kept-after-pause"));
}

QTEST_GUILESS_MAIN(LoggingTest)

#include "loggingtest.moc"
