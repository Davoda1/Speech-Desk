#include "mainwindow.h"
#include <QApplication>
#include <QTimer>
#include <QFile>
#include <QTextStream>
#include <QCommandLineParser>
#include <QScreen>
#include <QFontDatabase>
#include <QFileInfo>
#include <QDir>
int main(int argc,char **argv)
{
    QCoreApplication::setOrganizationName("Speech-Desk");QCoreApplication::setApplicationName("SpeechDesk");QCoreApplication::setApplicationVersion("1.1.0");
    qputenv("DO_NOT_TRACK","1");qputenv("NEEDLE_TELEMETRY","0");qputenv("HF_HUB_OFFLINE","1");qputenv("HF_HUB_DISABLE_TELEMETRY","1");
    if(qEnvironmentVariableIsEmpty("QT_MEDIA_BACKEND"))qputenv("QT_MEDIA_BACKEND","ffmpeg");
#ifndef Q_OS_WIN
    const auto executableDir=QFileInfo(QFile::symLinkTarget("/proc/self/exe")).absoluteDir();
    const QString portablePlugins=executableDir.filePath("plugins");
    const QString installedPlugins="/usr/lib/speechdesk/plugins";
    if(QDir(portablePlugins).exists())qputenv("QT_PLUGIN_PATH",portablePlugins.toUtf8());
    else if(executableDir.absolutePath()=="/usr/bin"&&QDir(installedPlugins).exists())qputenv("QT_PLUGIN_PATH",installedPlugins.toUtf8());
#endif
    QApplication app(argc,argv);QApplication::setStyle("Fusion");
    QFont font=app.font();font.setFamily(
#ifdef Q_OS_WIN
        "Segoe UI"
#else
        "DejaVu Sans"
#endif
    );if(!QFontDatabase::families().contains(font.family()))font.setFamily("DejaVu Sans");font.setPointSize(10);app.setFont(font);
    QCommandLineParser parser;parser.setApplicationDescription("SpeechDesk — write, dictate, then listen");parser.addHelpOption();parser.addVersionOption();parser.addOption({"smoke-test","Open the real application and exit after startup."});parser.addOption({"screenshot","Save a screenshot of the actual application, then exit.","path"});parser.addOption({"theme","Override theme for this session: light, dark or system.","theme"});parser.addOption({"demo","Show explicitly labeled sample text for a preview."});parser.addOption({"compact","Use the minimum supported window size."});parser.addOption({"dictation","Show integrated dictation controls."});parser.addOption({"stt","Alias for --dictation."});parser.process(app);
    MainWindow window;if(parser.isSet("theme"))window.setThemeOverride(parser.value("theme"));if(parser.isSet("compact"))window.resize(680,540);if(parser.isSet("demo"))window.showDemo(parser.isSet("dictation")||parser.isSet("stt"));else if(parser.isSet("dictation")||parser.isSet("stt"))window.showDictation();window.show();
    if(parser.isSet("screenshot")){QString path=parser.value("screenshot");QTimer::singleShot(700,&app,[&app,&window,path]{bool ok=window.grab().save(path);app.exit(ok?0:2);});}
    else if(parser.isSet("smoke-test"))QTimer::singleShot(1200,&app,&QApplication::quit);
    return app.exec();
}
