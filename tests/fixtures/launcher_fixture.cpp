#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

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
    const auto output = qEnvironmentVariable("TEXTSCOPE_LAUNCHER_OUTPUT");
    if (output.isEmpty())
        return 2;
    QThread::msleep(1000);
    QFile file(output);
    if (!file.open(QIODevice::WriteOnly))
        return 3;
    const auto data = QJsonDocument(QJsonObject{
        {"cwd", QDir::currentPath()}, {"arguments", QJsonArray::fromStringList(arguments)}
    }).toJson();
    if (file.write(data) != data.size() || !file.flush())
        return 4;
    file.close();
    QFile done(output + ".done");
    return done.open(QIODevice::WriteOnly) ? 0 : 5;
}
