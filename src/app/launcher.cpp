#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTextStream>
#ifdef Q_OS_MACOS
#include <crt_externs.h>
#include <fcntl.h>
#include <spawn.h>
#include <cstring>
#include <vector>
#endif

int main(int argc, char** argv) {
#ifdef Q_OS_MACOS
    QStringList arguments;
    for (int i = 1; i < argc; ++i)
        arguments.append(QString::fromUtf8(argv[i]));
#endif
    QCoreApplication application(argc, argv);
#ifndef Q_OS_MACOS
    const auto arguments = application.arguments().mid(1);
#endif
    if (arguments == QStringList{"--version"} || arguments == QStringList{"-v"}) {
        QTextStream(stdout) << "TextScope " TEXTSCOPE_VERSION "\n";
        return 0;
    }
    if (arguments == QStringList{"--help"} || arguments == QStringList{"-h"}) {
        QTextStream(stdout)
            << "Usage: tscope [file] [--filters file.tat] [--config preferences.xml]\n"
               "              [--line N] [--clipboard] [--encoding NAME] [--direct]\n"
               "Launch TextScope and return immediately. Relative paths use the current directory.\n"
               "  -h, --help       Print this help.\n"
               "  -v, --version    Print the application version.\n"
               "Legacy /Filters: /Config: /Line: /Clipboard aliases are forwarded unchanged.\n"
               "Each invocation starts a new process; --wait and window reuse are not supported.\n";
        return 0;
    }
    const QDir directory(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
    const auto program = directory.filePath("textscope.exe");
#elif defined(Q_OS_MACOS)
    const auto adjacent = directory.filePath("textscope");
    const auto program = QFileInfo::exists(adjacent) ? adjacent
        : directory.filePath("textscope.app/Contents/MacOS/textscope");
#else
    const auto program = directory.filePath("textscope");
#endif
    if (!QFileInfo(program).isExecutable()) {
        QTextStream(stderr) << "tscope: application is missing or not executable: "
                            << QDir::toNativeSeparators(program) << '\n';
        return 1;
    }
#ifdef Q_OS_MACOS
    // QProcess normalizes arguments as filenames on macOS; preserve original bytes.
    auto executable = QFile::encodeName(program);
    std::vector<char*> forwarded{executable.data()};
    for (int i = 1; i < argc; ++i)
        forwarded.push_back(argv[i]);
    forwarded.push_back(nullptr);
    posix_spawnattr_t attributes;
    int error = posix_spawnattr_init(&attributes);
    if (error) {
        QTextStream(stderr) << "tscope: cannot initialize launch: " << std::strerror(error) << '\n';
        return 1;
    }
    posix_spawn_file_actions_t actions;
    error = posix_spawn_file_actions_init(&actions);
    if (!error) {
        error = posix_spawnattr_setflags(&attributes,
            POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT);
        for (int descriptor = 0; descriptor < 3 && !error; ++descriptor)
            error = posix_spawn_file_actions_addopen(&actions, descriptor, "/dev/null",
                descriptor == 0 ? O_RDONLY : O_WRONLY, 0);
        if (!error) {
            pid_t child;
            error = posix_spawn(&child, executable.constData(), &actions, &attributes,
                forwarded.data(), *_NSGetEnviron());
        }
        posix_spawn_file_actions_destroy(&actions);
    }
    posix_spawnattr_destroy(&attributes);
    if (error) {
        QTextStream(stderr) << "tscope: cannot start TextScope: " << std::strerror(error) << '\n';
        return 1;
    }
#else
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setWorkingDirectory(QDir::currentPath());
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    if (!process.startDetached()) {
        QTextStream(stderr) << "tscope: cannot start TextScope: " << process.errorString() << '\n';
        return 1;
    }
#endif
    return 0;
}
