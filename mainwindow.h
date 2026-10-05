#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "manifest.h"
#include "manifestitem.h"

#include <QMainWindow>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProgressDialog>
#include <QPointer>

#ifndef VERSION
#define VERSION x.y.z
#endif
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    QNetworkAccessManager netMan;
    Ui::MainWindow *ui;
    Manifest* manifest = nullptr;
    ServerEntry* pendingLaunch = nullptr;
    bool busy = false;
    bool optionsOpen = false;
    quint64 loadGeneration = 0;
    quint64 operationGeneration = 0;
    QString operationDirectory;
    QList<QPointer<QNetworkReply>> operationReplies;
    long currentFiles;
    long errorFiles;
    long maxFiles;

    void setup();
    void addServerEntry(ServerEntry* server);
    void setManifest(Manifest* manifest);
    void validateManifest(Manifest* manifest);
    void downloadManifest(QUrl url);
    void openManifest(QString fname);
    void downloadItem(ManifestItem* item, int attemptsRemaining);
    void beginOperation();
    void cancelOperation();
    void finishOperation(bool success, const QString &error = QString());
    void finishItem(bool success);
    void startValidation();
    void refreshForLaunch();
    void acceptLaunchManifest(const QByteArray &content, ServerEntry *selected);
    Manifest *parseManifest(const QByteArray &content, const QUrl &source);
    bool isValidated(Manifest *manifest) const;
    void deleteItem(QString *item);
    void loadManifests();

};
#endif // MAINWINDOW_H
