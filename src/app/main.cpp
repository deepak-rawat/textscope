#include "session.hpp"

#include <QApplication>
#include <QFileOpenEvent>
#include <QIcon>
#include <QTextStream>
#include <QThreadPool>
#include <QTimer>
#include <stdexcept>

namespace {
textscope::Startup parse(const QStringList& arguments)
{
    textscope::Startup startup;
    bool sourceChosen = false;
    bool positional = false;
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        const auto argument = arguments[i];
        if (!positional && argument == "--") { positional = true; continue; }
        QString option, value;
        if (!positional && argument.startsWith("--")) {
            const auto equals = argument.indexOf('=');
            option = argument.mid(2, equals < 0 ? -1 : equals - 2).toLower();
            if (equals >= 0) value = argument.mid(equals + 1);
            else if (option != "clipboard" && option != "direct" && option != "help" && option != "version") {
                if (++i >= arguments.size()) throw std::runtime_error("Missing option value");
                value = arguments[i];
            }
        } else if (!positional && (argument.startsWith("/Filters:", Qt::CaseInsensitive) ||
            argument.startsWith("/Config:", Qt::CaseInsensitive) ||
            argument.startsWith("/Line:", Qt::CaseInsensitive) ||
            argument.compare("/Clipboard", Qt::CaseInsensitive) == 0)) {
            const auto colon = argument.indexOf(':');
            option = argument.mid(1, colon < 0 ? -1 : colon - 1).toLower();
            if (colon >= 0) value = argument.mid(colon + 1);
        } else {
            if (startup.clipboard) {
                qInfo().noquote() << "Ignoring file input because /Clipboard was supplied first:" << argument;
                continue;
            }
            if (sourceChosen) throw std::runtime_error("Only one input file may be supplied per window");
            startup.input = argument;
            sourceChosen = true;
            continue;
        }
        if (option == "filters") startup.filters.append(value);
        else if (option == "config") {
            if (!startup.configuration.isEmpty()) throw std::runtime_error("Only one configuration option is allowed");
            startup.configuration = value;
        } else if (option == "encoding") startup.encoding = value.toLatin1();
        else if (option == "line") {
            bool ok = false;
            startup.line = value.toULongLong(&ok);
            if (!ok || startup.line == 0) throw std::runtime_error("Line must be a positive integer");
        } else if (option == "clipboard") {
            if (!sourceChosen) { startup.clipboard = true; sourceChosen = true; }
        } else if (option == "direct") startup.direct = true;
        else throw std::runtime_error(("Unrecognized option: " + argument).toStdString());
    }
    return startup;
}

class Application : public QApplication {
public:
    using QApplication::QApplication;
    textscope::Session* create(const textscope::Startup& startup = {})
    {
        auto* session = new textscope::Session(startup, this);
        QObject::connect(session, &textscope::Session::newWindowRequested, this, [this] { create(); });
        return session;
    }
    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::FileOpen) {
            auto* open = static_cast<QFileOpenEvent*>(event);
            textscope::Startup startup;
            if (open->file().endsWith(".tat", Qt::CaseInsensitive)) startup.filters.append(open->file());
            else startup.input = open->file();
            create(startup);
            return true;
        }
        return QApplication::event(event);
    }
};
}

int main(int argc, char** argv)
{
    bool smokeTest = false;
    for (int i = 1; i < argc; ++i) {
        const QString argument = QString::fromLocal8Bit(argv[i]);
        if (argument == "--smoke-test") smokeTest = true;
        if (argument == "--help" || argument == "--version") {
            QTextStream out(stdout);
            if (argument == "--version") out << "TextScope 0.1.0\n";
            else out << "Usage: textscope [file] [--filters file.tat] [--config preferences.xml]\n"
                "           [--line N] [--clipboard] [--encoding NAME] [--direct]\n"
                "Legacy /Filters: /Config: /Line: /Clipboard aliases are accepted.\n"
                "Default: immutable snapshot. --direct explicitly reads a mutable original file.\n";
            return 0;
        }
    }
    Application app(argc, argv);
    app.setApplicationName("TextScope");
    app.setOrganizationName("TextScope");
    app.setApplicationVersion("0.1.0");
    app.setDesktopFileName("textscope");
    try {
        const QIcon icon(":/icons/textscope.png");
        if (icon.isNull()) throw std::runtime_error("Bundled application icon cannot be loaded");
        app.setWindowIcon(icon);
        QSettings settings;
        QSettings legacy("TextAnalysisToolQt", "TextAnalysisToolQt");
        textscope::migrateLegacySettings(settings, legacy);
        auto arguments = app.arguments();
        arguments.removeAll("--smoke-test");
        app.create(parse(arguments));
        if (smokeTest) QTimer::singleShot(250, &app, &QApplication::quit);
        const auto result = app.exec();
        QThreadPool::globalInstance()->waitForDone();
        return result;
    } catch (const std::exception& error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
}
