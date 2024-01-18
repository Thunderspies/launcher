#include "mainwindow.h"
#include "manifest.h"

#include <QtDebug>
#include <QApplication>

static QFile *g_logFile;

void logHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context)
    static QTextStream stream(g_logFile);
    if(!g_logFile->isOpen()) {
        g_logFile->open(QIODevice::WriteOnly);
    }
    QString typeString;
    switch(type) {
        case QtDebugMsg: typeString = "Debug"; break;
        case QtWarningMsg: typeString = "Warning"; break;
        case QtCriticalMsg: typeString = "Critical"; break;
        case QtFatalMsg: typeString = "Fatal"; break;
        case QtInfoMsg: typeString = "Info"; break;
    }
    // NOTE: Using deprecated endl because Qt::endl isn't available in latest Ubuntu
    stream << QString("%1: %2").arg(typeString, msg) << endl;
}

int main(int argc, char *argv[])
{

    QApplication a(argc, argv);

    a.setApplicationName("Sweet Tea");
    a.setOrganizationName("Thunderspy Gaming");

    /*
     * Attempt to change the working directory
     * to the one in the one in the configurations.
     */
    QSettings settings;
    QString datadir = settings.value (
                "datadir",
                QStandardPaths::writableLocation(QStandardPaths::DataLocation)
                ).toString();
    if(!QDir::setCurrent(datadir)) {
        qWarning() << "unable to access: " + datadir;
        if(QDir(datadir).mkpath(".") && QDir::setCurrent(datadir))
            settings.remove("oldDir");
        else
            qWarning() << "unable to create: " + datadir;
    }
    g_logFile = new QFile(datadir + "/log.txt");
    qInstallMessageHandler(logHandler);

    QObject::connect(&a, &QCoreApplication::aboutToQuit, g_logFile, &QFile::close);

    // Show the main window.
    MainWindow w;
    w.show();

    return a.exec();

}
