#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "manifest.h"
#include "optionswindow.h"
#include "errorwindow.h"
#include "launchprofileitemdelegate.h"

#include <QtConcurrent>
#include <QMessageBox>
#include <QProgressDialog>
#include <QDesktopServices>
#include <QJsonDocument>

// FIXME: Don't put so much in the main window.
MainWindow::MainWindow (
        QWidget *parent )
    : QMainWindow(parent)
    , ui(new Ui::MainWindow) {

    setup();

}

void MainWindow::setup() {

    ui->setupUi(this);
    ui->listWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    ui->listWidget->setItemDelegate(new LaunchProfileItemDelegate);

    loadManifests();

    /*
     * Configure the screenshot button to open the screenshot folder.
     */
    connect (
        ui->ScreenshotButton,
        &QPushButton::released,
        [] {
            QString path = QDir::cleanPath(QDir::currentPath() + QDir::separator() + "screenshots");
            QDir(path).mkpath(".");
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        });

    /*
     * Configure the options button to open the options menu.
     */
    connect (
        ui->OptionsButton,
        &QPushButton::released,
        [this] {
            ui->OptionsButton->setEnabled(false);
            OptionsWindow *w = new OptionsWindow(this);
            w->show();
            connect(w, &QDialog::finished, [this] {
                ui->OptionsButton->setEnabled(true);
                loadManifests();
            });
        });

    /*
     * Configure the launch profile list to set the
     * current manifest to the selected list item.
     */
    connect (
        ui->listWidget,
        &QListWidget::itemClicked,
        [this] {
            QListWidgetItem *item = ui->listWidget->currentItem();
            ServerEntry *entry = item->data(Qt::UserRole + 1).value<ServerEntry*>();
            setManifest(entry->manifest);
        });

    /*
     * Configure the launch button to run with the given launch profile.
     */
    connect (
        ui->LaunchButton,
        &QPushButton::released,
        [this] {
            QSettings *settings = new QSettings();
            QProcess *proc = new QProcess(this);
            QListWidgetItem *item = ui->listWidget->currentItem();
            ServerEntry *server = item->data(Qt::UserRole + 1).value<ServerEntry*>();
            QString args = settings->value("launchParams", "").toString();
            proc->startDetached(server->client, server->args.split(" ") + args.split(" "));
        });

    /*
     * Configure the validate button to validate the selected manifest.
     */
    connect (
        ui->ValidateButton,
        &QPushButton::released,
        [this] {
            validateManifest(manifest);
        });

    QSettings *settings = new QSettings();
    if(!settings->value("checkUpdates", true).toBool())
        return;

    /*
     * Check for an update.
     */
    QNetworkRequest req(QUrl("https://api.github.com/repos/Thunderspies/launcher/releases/latest"));
    QNetworkReply *res = netMan.get(req);
    connect (
        res,
        &QNetworkReply::finished,
        [res, settings] {
            QString tag = QJsonDocument::fromJson(res->readAll()).object().value("tag_name").toString();
            if(tag == TOSTRING(VERSION))
                return;
            QMessageBox msgBox;
            msgBox.setWindowTitle("Update Available");
            msgBox.setText("A new update is available");
            msgBox.setInformativeText("Would you like to view the latest release?");
            msgBox.addButton(QMessageBox::Ignore);
            QAbstractButton *viewButton = msgBox.addButton("View Release", QMessageBox::YesRole);
            QAbstractButton *disableButton = msgBox.addButton("Never Check", QMessageBox::ActionRole);
            connect(disableButton, &QAbstractButton::pressed, [settings] {
                settings->setValue("checkUpdates", false);
            });
            msgBox.exec();
            if(msgBox.clickedButton() == viewButton)
                QDesktopServices::openUrl(QUrl("https://github.com/Thunderspies/launcher/releases/latest"));
        });

}

/*
 * Ask the user for permission before deleting a file.
 */
void MainWindow::deleteItem(QString *item) {

    QFile file(*item);
    if(!file.exists()) {
        qInfo() << *item << " does not exist, so not deleting";
        return;
    }

    QMessageBox::StandardButton choice = QMessageBox::question(this, "Delete File", "Delete " + *item);
    if(choice == QMessageBox::Yes)
        if(!file.remove())
            QMessageBox::warning(this, "File Delete Error", "Unable to delete " + *item);

}

/*
 * Download and/or validate a file in the given manifest.
 * FIXME: A LOT of code dupe.
 */
void MainWindow::downloadItem(ManifestItem *item) {

    QFutureWatcher<bool> *watcher = new QFutureWatcher<bool>(this);
    QFuture<bool> future = QtConcurrent::run([item]{
        return item->validate();
    });

    connect(watcher, &QFutureWatcher<bool>::finished, [=] {

        QFileInfo(item->fname).dir().mkpath(".");
        QSaveFile *file = new QSaveFile(item->fname);
        if(!file->open(QIODevice::WriteOnly)) {
            qCritical() << "failed to write to " << item->fname;
            errorFiles++;
            if(currentFiles + errorFiles >= maxFiles) {
                ui->ValidateButton->setEnabled(true);
                ui->listWidget->setEnabled(true);
                ErrorWindow *w = new ErrorWindow(this);
                w->show();
            }
            return;
        }

        if(future.result()) {
            qInfo() << item->fname + " validated";
            currentFiles++;
            ui->UpdateProgress->setValue(currentFiles);
            if(currentFiles + errorFiles >= maxFiles) {
                qInfo() << "last file";
                if(errorFiles <= 0) {
                    QSettings settings;
                    settings.setValue("manifestChecksum", manifest->checksum);
                    settings.setValue("oldDir", QDir::currentPath());
                    qInfo() << QDir::currentPath();
                    qInfo() << settings.value("oldDir").toString();
                    ui->LaunchButton->setEnabled(true);
                } else {
                    ErrorWindow *w = new ErrorWindow(this);
                    w->show();
                }
                ui->ValidateButton->setEnabled(true);
                ui->listWidget->setEnabled(true);
            }
            return;
        }

        if(item->urls.isEmpty()) {
            file->cancelWriting();
            qCritical() << "failed to download " << item->fname;
            errorFiles++;
            if(currentFiles + errorFiles >= maxFiles) {
                ui->ValidateButton->setEnabled(true);
                ui->listWidget->setEnabled(true);
                ErrorWindow *w = new ErrorWindow(this);
                w->show();
            }
            return;
        }

        QNetworkRequest req(*item->urls.takeLast());
        req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
        QNetworkReply *res = netMan.get(req);
        connect (
            res,
            &QNetworkReply::readyRead,
            [=] {
               file->write(res->read(res->bytesAvailable()));
            });
        connect (
            res,
            &QNetworkReply::finished,
            [=] {

               if(res->error() != QNetworkReply::NoError)
                   qWarning() << res->request().url().toString() << res->errorString();

               res->deleteLater();
               file->commit();
               this->downloadItem(item);

            });

        watcher->deleteLater();

    });

    watcher->setFuture(future);

}

/*
 * Add a server entry (launch profile) to the UI list.
 */
void MainWindow::addServerEntry(ServerEntry *server) {

    /*
     * Create a data model object for the list widget from the launch profile.
     */
    QListWidgetItem *item = new QListWidgetItem(server->name, ui->listWidget);
    item->setData(Qt::UserRole + 1, QVariant::fromValue(server));

    // Download the launch profile icon if it's there is one available.
    if(!server->icon.isEmpty()) {
        QNetworkRequest req(server->icon);
        req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
        QNetworkReply *res = netMan.get(req);
        connect (
            res,
            &QNetworkReply::finished,
            [=] {

               if(res->error() != QNetworkReply::NoError) {
                   qWarning() << "icon: " << res->errorString();
                   return;
               }

               QPixmap pixels;
               if(!pixels.loadFromData(res->readAll()))
                   qWarning() << "unable to read icon: " << server->icon;
               else
                   item->setIcon(QIcon(pixels));

               res->deleteLater();

            });
    }

    // Download the message of the day (MoTD) if one is available.
    if(!server->motd.isEmpty()) {
        QNetworkRequest req(server->motd);
        req.setHeader(QNetworkRequest::UserAgentHeader, "Sweet Tea / " + QString(TOSTRING(VERSION)));
        req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
        QNetworkReply *res = netMan.get(req);
        item->setData(Qt::UserRole, "Retrieving MoTD");
        connect (
            res,
            &QNetworkReply::finished,
            [=] {

               if(res->error() != QNetworkReply::NoError) {
                   qWarning() << "motd: " << res->errorString();
                   item->setData(Qt::UserRole, "Failed to retrieve MoTD");
                   return;
               }

               QString motd(res->read(140));
               item->setData(Qt::UserRole, motd);

               res->deleteLater();

            });
    }

}

/*
 * Read a manifest file from the local file system.
 */
void MainWindow::openManifest(QString fname) {

    /*
     * Disable the validate button so it's not
     * pressed while a manifest is being read
     * still.
     */
    ui->ValidateButton->setEnabled(false);
    ui->UpdateProgress->setValue(false);

    // Parse the XML of the manifest.
    QDomDocument doc;
    QFile file(fname);

    if(file.open(QIODevice::ReadOnly) && doc.setContent(&file)) {

        // Hash the manifest to easily compare to other manifests.
        QCryptographicHash md5(QCryptographicHash::Md5);
        md5.addData(doc.toByteArray());

        // Create a manifest object from the XML file.
        // Add its server entries (launch profiles) to the list.
        Manifest *manifest = new Manifest(doc, md5.result());
        for(ServerEntry *server : manifest->servers)
            addServerEntry(server);

    }

    else
        // Log a critical error if the manifest can't be read.
        // FIXME: Add to error list.
        // FIXME: Make error critical.
        qWarning() << "unable to read manifest: " + fname;

    file.close();

}

/*
 * Download a manifest.
 */
void MainWindow::downloadManifest(QUrl url) {

    /*
     * Disable the validate button so it's not
     * pressed while a manifest is being downloaded
     * still.
     */
    ui->ValidateButton->setEnabled(false);
    ui->UpdateProgress->setValue(false);

    /*
     * Send an HTTP request to download the manifest,
     * and add its launch profiles (server entries) to the
     * list for display on success response.
     */
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    QNetworkReply *res = netMan.get(req);
    connect (
        res,
        &QNetworkReply::finished,
        [=] {

           // Log a critical error if the manifest can't be downloaded.
           // FIXME: Add to error list.
           if(res->error() != QNetworkReply::NoError)
               qCritical() << "manifest: " << res->errorString();

           // Parse the XML of the manifest.
           QByteArray content = res->readAll();
           QDomDocument doc = QDomDocument();
           doc.setContent(content);

           // Hash the manifest to easily compare to other manifests.
           QCryptographicHash md5(QCryptographicHash::Md5);
           md5.addData(content);

           // Create a manifest object from the XML file.
           // Add its server entries (launch profiles) to the list.
           Manifest *manifest = new Manifest(doc, md5.result());
           for(ServerEntry *server : manifest->servers)
               addServerEntry(server);

           // Delete the response object to avoid memory leaks.
           res->deleteLater();

        });

}

/*
 * Set the currently selected manifest.
 */
void MainWindow::setManifest(Manifest *manifest) {

    /*
     * Reference the selected manifest so it can
     * be used when the validate button is pressed.
     */
    this->manifest = manifest;

    /*
     * Since a manifest is needed for validation,
     * enable the validation button once a manifest
     * is selected. Disable the launch button until
     * that manifest has been validated, and set
     * validation progress to 0.
     */
    ui->LaunchButton->setEnabled(false);
    ui->ValidateButton->setEnabled(true);
    ui->UpdateProgress->setValue(0);

    /*
     * The checksum from the last valid manifest, and
     * the last directory used to download files can
     * be compared to the current manifest and directory
     * to determine if validation is necessary.
     */
    QSettings settings;
    QByteArray oldChecksum = settings.value("manifestChecksum").toByteArray();
    QString oldDir = settings.value("oldDir").toString();
    qInfo() << "old manifest: " + oldChecksum.toHex();
    qInfo() << "new manifest: " + manifest->checksum.toHex();
    qInfo() << "old dir: " + oldDir;
    qInfo() << "new dir: " + QDir::currentPath();

    /*
     * Enable launching if this manifest is the last valid one.
     */
    if(oldChecksum == manifest->checksum && oldDir == QDir::currentPath()) {
        currentFiles = manifest->items.size();
        ui->UpdateProgress->setValue(currentFiles);
        ui->UpdateProgress->setMaximum(currentFiles);
        ui->LaunchButton->setEnabled(true);
    }

}

/*
 * Validate a manifest by validating each file in
 * the manifest.
 */
void MainWindow::validateManifest(Manifest *manifest) {

    /*
     * Clear the last valid manifest and download
     * directory, so that validation is not skipped
     * again until another manifest is validated.
     */
    QSettings settings;
    settings.remove("manifestChecksum");
    settings.remove("oldDir");

    /*
     * Delete any files that are designated for
     * deletion in the manifest.
     */
    for(QString *item : manifest->deletions)
        deleteItem(item);

    /*
     * FIXME: The current file count was used for
     * other things, but not it's only here for the
     * progress bar. This can be removed later.
     */
    currentFiles = 0;
    errorFiles = 0;
    maxFiles = manifest->items.size();

    /*
     * Disable the UI elements, so they aren't pressed
     * during validation.
     */
    ui->ValidateButton->setEnabled(false);
    ui->LaunchButton->setEnabled(false);
    ui->listWidget->setEnabled(false);
    ui->UpdateProgress->setMaximum(maxFiles);

    // Download and/or validate each file in the manifest.
    for(ManifestItem *item : manifest->items)
        downloadItem(item);

}

/*
 * Fetch the list of manifests, and either download
 * or read from the local file system.
 */
void MainWindow::loadManifests() {

    ui->listWidget->clear();
    QSettings settings;
    QStringList manifests = settings.value("manifests").toString().split(" ");

    for(QString manifest : manifests) {
        QUrl url = QUrl::fromUserInput(manifest);

        // Download the manifest if it's not a local file.
        if(!url.isLocalFile())
            downloadManifest(url);

        // Read the manifest from the local file system.
        else
            openManifest(manifest);

    }

}

MainWindow::~MainWindow() {
    delete ui;
}
