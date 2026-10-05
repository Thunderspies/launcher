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
#include <QSignalBlocker>
#include <QPersistentModelIndex>
#include <QTimer>

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
            if(busy || optionsOpen)
                return;
            optionsOpen = true;
            ui->OptionsButton->setEnabled(false);
            ui->LaunchButton->setEnabled(false);
            ui->ValidateButton->setEnabled(false);
            ui->listWidget->setEnabled(false);
            OptionsWindow *w = new OptionsWindow(this);
            w->show();
            connect(w, &QDialog::finished, this, [this] {
                optionsOpen = false;
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
        &QListWidget::currentItemChanged,
        this,
        [this](QListWidgetItem *item) {
            cancelOperation();
            ServerEntry *entry = item ? item->data(Qt::UserRole + 1).value<ServerEntry*>() : nullptr;
            setManifest(entry ? entry->manifest : nullptr);
        });

    /*
     * Configure the launch button to run with the given launch profile.
     */
    connect(ui->LaunchButton, &QPushButton::released, this, &MainWindow::refreshForLaunch);

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
 */
void MainWindow::downloadItem(ManifestItem *item, int attemptsRemaining) {

    const quint64 generation = operationGeneration;
    const QString filename = QDir(operationDirectory).absoluteFilePath(item->fname);
    const QByteArray checksum = item->md5;
    const long size = item->size;
    QFutureWatcher<bool> *watcher = new QFutureWatcher<bool>(this);
    // The worker owns copies so closing/reloading the window cannot destroy its input.
    QFuture<bool> future = QtConcurrent::run([filename, checksum, size]() mutable {
        QList<QUrl*> urls;
        QString path = filename;
        QByteArray md5 = checksum;
        ManifestItem copy(path, md5, size, urls);
        return copy.validate();
    });
    connect(watcher, &QFutureWatcher<bool>::finished, this, [=] {
        watcher->deleteLater();
        if(!busy || generation != operationGeneration)
            return;
        if(future.result()) {
            finishItem(true);
            return;
        }
        if(attemptsRemaining <= 0) {
            qCritical() << "failed to download or validate " << item->fname;
            finishItem(false);
            return;
        }

        QFileInfo(filename).dir().mkpath(".");
        QSharedPointer<QSaveFile> file(new QSaveFile(filename));
        if(!file->open(QIODevice::WriteOnly)) {
            qCritical() << "failed to write to " << filename;
            finishItem(false);
            return;
        }

        // Try each mirror at most once per validation, retaining them for future runs.
        QNetworkRequest req(*item->urls.at(attemptsRemaining - 1));
        req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
        QNetworkReply *res = netMan.get(req);
        operationReplies.append(res);
        QTimer *timeout = new QTimer(res);
        timeout->setSingleShot(true);
        connect(timeout, &QTimer::timeout, res, &QNetworkReply::abort);
        timeout->start(60000);
        connect(res, &QNetworkReply::readyRead, this, [=] {
            if(!busy || generation != operationGeneration)
                return;
            timeout->start(60000);
            QByteArray data = res->readAll();
            if(file->write(data) != data.size())
                res->abort();
        });
        connect(res, &QNetworkReply::finished, this, [=] {
            timeout->stop();
            res->deleteLater();
            if(!busy || generation != operationGeneration) {
                file->cancelWriting();
                return;
            }
            if(res->error() != QNetworkReply::NoError) {
                qWarning() << res->request().url().toString() << res->errorString();
                file->cancelWriting();
            } else if(!file->commit()) {
                qCritical() << "failed to save " << filename;
                finishItem(false);
                return;
            }
            downloadItem(item, attemptsRemaining - 1);
        });
    });
    watcher->setFuture(future);
}

/*
 * Add a server entry (launch profile) to the UI list.
 */
void MainWindow::addServerEntry(ServerEntry *server) {
    const quint64 generation = loadGeneration;

    /*
     * Create a data model object for the list widget from the launch profile.
     */
    QListWidgetItem *item = new QListWidgetItem(server->name, ui->listWidget);
    item->setData(Qt::UserRole + 1, QVariant::fromValue(server));
    const QPersistentModelIndex index(ui->listWidget->model()->index(ui->listWidget->row(item), 0));

    // Download the launch profile icon if it's there is one available.
    if(!server->icon.isEmpty()) {
        QNetworkRequest req(server->icon);
        req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
        QNetworkReply *res = netMan.get(req);
        connect (
            res,
            &QNetworkReply::finished,
            this,
            [=] {
               res->deleteLater();
               if(generation != loadGeneration || !index.isValid())
                   return;

               if(res->error() != QNetworkReply::NoError) {
                   qWarning() << "icon: " << res->errorString();
                   return;
               }

               QPixmap pixels;
               if(!pixels.loadFromData(res->readAll()))
                   qWarning() << "unable to read icon: " << server->icon;
               else
                   item->setIcon(QIcon(pixels));

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
            this,
            [=] {
               res->deleteLater();
               if(generation != loadGeneration || !index.isValid())
                   return;

               if(res->error() != QNetworkReply::NoError) {
                   qWarning() << "motd: " << res->errorString();
                   item->setData(Qt::UserRole, "Failed to retrieve MoTD");
                   return;
               }

               QString motd(res->read(140));
               item->setData(Qt::UserRole, motd);

            });
    }

}

/*
 * Read a manifest file from the local file system.
 */
void MainWindow::openManifest(QString fname) {
    QFile file(fname);
    if(!file.open(QIODevice::ReadOnly)) {
        qWarning() << "unable to read manifest: " << fname;
        return;
    }
    Manifest *loaded = parseManifest(file.readAll(), QUrl::fromLocalFile(QFileInfo(fname).absoluteFilePath()));
    if(loaded)
        for(ServerEntry *server : loaded->servers)
            addServerEntry(server);
}

/*
 * Download a manifest.
 */
void MainWindow::downloadManifest(QUrl url) {
    const quint64 generation = loadGeneration;
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    QNetworkReply *res = netMan.get(req);
    QTimer::singleShot(30000, res, [res] { res->abort(); });
    connect(res, &QNetworkReply::finished, this, [=] {
        res->deleteLater();
        if(generation != loadGeneration)
            return;
        if(res->error() != QNetworkReply::NoError) {
            qCritical() << "manifest: " << res->errorString();
            return;
        }
        Manifest *loaded = parseManifest(res->readAll(), url);
        if(loaded)
            for(ServerEntry *server : loaded->servers)
                addServerEntry(server);
    });
}

Manifest *MainWindow::parseManifest(const QByteArray &content, const QUrl &source) {
    QDomDocument doc;
    if(!doc.setContent(content)) {
        qCritical() << "unable to parse manifest: " << source;
        return nullptr;
    }
    Manifest *loaded = new Manifest(doc, QCryptographicHash::hash(content, QCryptographicHash::Md5), this);
    loaded->source = source;
    return loaded;
}

bool MainWindow::isValidated(Manifest *manifest) const {
    QSettings settings;
    return manifest && settings.value("manifestChecksum").toByteArray() == manifest->checksum
        && settings.value("oldDir").toString() == QDir::currentPath();
}

void MainWindow::beginOperation() {
    busy = true;
    ++operationGeneration;
    operationDirectory = QDir::currentPath();
    ui->LaunchButton->setEnabled(false);
    ui->ValidateButton->setEnabled(false);
    ui->OptionsButton->setEnabled(false);
    ui->listWidget->setEnabled(false);
}

void MainWindow::cancelOperation() {
    ++operationGeneration;
    busy = false;
    pendingLaunch = nullptr;
    const auto replies = operationReplies;
    operationReplies.clear();
    for(const auto &reply : replies)
        if(reply && !reply->isFinished())
            reply->abort();
    ui->OptionsButton->setEnabled(!optionsOpen);
    ui->listWidget->setEnabled(!optionsOpen);
}

void MainWindow::finishOperation(bool success, const QString &error) {
    ServerEntry *server = success ? pendingLaunch : nullptr;
    pendingLaunch = nullptr;
    busy = false;
    operationReplies.clear();
    ui->OptionsButton->setEnabled(!optionsOpen);
    ui->listWidget->setEnabled(!optionsOpen);
    ui->ValidateButton->setEnabled(manifest && !optionsOpen);
    ui->LaunchButton->setEnabled(isValidated(manifest) && !optionsOpen);
    if(!error.isEmpty()) {
        QMessageBox::warning(this, "Launch Error", error);
        return;
    }
    if(server) {
        if(operationDirectory != QDir::currentPath()) {
            QMessageBox::warning(this, "Launch Error", "The game directory changed. Please validate again.");
            return;
        }
        QSettings settings;
        const QString args = settings.value("launchParams", "").toString();
        if(!QProcess::startDetached(server->client, args.split(" ") + server->args.split(" "), operationDirectory))
            QMessageBox::warning(this, "Launch Error", "Unable to start " + server->client);
    }
}

void MainWindow::refreshForLaunch() {
    if(busy || optionsOpen || !ui->LaunchButton->isEnabled())
        return;
    QListWidgetItem *item = ui->listWidget->currentItem();
    ServerEntry *selected = item ? item->data(Qt::UserRole + 1).value<ServerEntry*>() : nullptr;
    if(!selected)
        return;
    manifest = selected->manifest;
    beginOperation();
    const quint64 generation = operationGeneration;
    const QUrl source = manifest->source;
    if(source.isLocalFile()) {
        QFile file(source.toLocalFile());
        if(!file.open(QIODevice::ReadOnly)) {
            finishOperation(false, "Unable to read the manifest. Please try again.");
            return;
        }
        acceptLaunchManifest(file.readAll(), selected);
        return;
    }
    QNetworkRequest req(source);
    req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    req.setRawHeader("Cache-Control", "no-cache");
    QNetworkReply *res = netMan.get(req);
    operationReplies.append(res);
    QTimer::singleShot(30000, res, [res] { res->abort(); });
    connect(res, &QNetworkReply::finished, this, [=] {
        res->deleteLater();
        if(!busy || generation != operationGeneration)
            return;
        if(res->error() != QNetworkReply::NoError) {
            finishOperation(false, "Unable to refresh the manifest: " + res->errorString());
            return;
        }
        acceptLaunchManifest(res->readAll(), selected);
    });
}

void MainWindow::acceptLaunchManifest(const QByteArray &content, ServerEntry *selected) {
    Manifest *fresh = parseManifest(content, selected->manifest->source);
    if(!fresh) {
        finishOperation(false, "Unable to parse the refreshed manifest. Please try again.");
        return;
    }
    ServerEntry *replacement = nullptr;
    int matches = 0;
    for(ServerEntry *server : fresh->servers) {
        if(server->name == selected->name) {
            replacement = server;
            ++matches;
        }
    }
    if(matches != 1 || replacement->client.isEmpty()) {
        fresh->deleteLater();
        finishOperation(false, "The selected launch profile is missing or ambiguous in the refreshed manifest. Reopen Options to reload the server list.");
        return;
    }
    if(fresh->checksum == manifest->checksum) {
        fresh->deleteLater();
        pendingLaunch = selected;
    } else {
        Manifest *previous = manifest;
        // Suppress selection changes while replacing exactly this manifest's profiles.
        QSignalBlocker blocker(ui->listWidget);
        for(int row = ui->listWidget->count() - 1; row >= 0; --row) {
            QListWidgetItem *item = ui->listWidget->item(row);
            if(item->data(Qt::UserRole + 1).value<ServerEntry*>()->manifest == previous)
                delete ui->listWidget->takeItem(row);
        }
        manifest = fresh;
        for(ServerEntry *server : fresh->servers) {
            addServerEntry(server);
            if(server == replacement)
                ui->listWidget->setCurrentRow(ui->listWidget->count() - 1);
        }
        pendingLaunch = replacement;
        previous->deleteLater();
    }
    if(isValidated(manifest))
        finishOperation(true);
    else
        startValidation();
}

void MainWindow::finishItem(bool success) {
    if(success)
        ++currentFiles;
    else
        ++errorFiles;
    ui->UpdateProgress->setValue(currentFiles);
    if(currentFiles + errorFiles < maxFiles)
        return;
    if(errorFiles == 0 && operationDirectory == QDir::currentPath()) {
        QSettings settings;
        settings.setValue("manifestChecksum", manifest->checksum);
        settings.setValue("oldDir", operationDirectory);
        finishOperation(true);
    } else {
        finishOperation(false);
        ErrorWindow *w = new ErrorWindow(this);
        w->show();
    }
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
    ui->ValidateButton->setEnabled(manifest && !optionsOpen);
    ui->UpdateProgress->setValue(0);
    if(!manifest || optionsOpen)
        return;

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
        ui->UpdateProgress->setMaximum(qMax(1L, currentFiles));
        ui->UpdateProgress->setValue(qMax(1L, currentFiles));
        ui->LaunchButton->setEnabled(true);
    }

}

/*
 * Validate a manifest by validating each file in
 * the manifest.
 */
void MainWindow::validateManifest(Manifest *manifest) {
    if(busy || optionsOpen || !manifest)
        return;
    this->manifest = manifest;
    beginOperation();
    startValidation();
}

void MainWindow::startValidation() {

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
    ui->UpdateProgress->setMaximum(qMax(1L, maxFiles));
    ui->UpdateProgress->setValue(0);

    // Download and/or validate each file in the manifest.
    if(maxFiles == 0) {
        ui->UpdateProgress->setValue(1);
        QSettings settings;
        settings.setValue("manifestChecksum", manifest->checksum);
        settings.setValue("oldDir", operationDirectory);
        finishOperation(true);
        return;
    }
    for(ManifestItem *item : manifest->items)
        downloadItem(item, item->urls.size());

}

/*
 * Fetch the list of manifests, and either download
 * or read from the local file system.
 */
void MainWindow::loadManifests() {
    cancelOperation();
    ++loadGeneration;
    ui->listWidget->clear();
    setManifest(nullptr);
    for(Manifest *loaded : findChildren<Manifest*>(QString(), Qt::FindDirectChildrenOnly))
        loaded->deleteLater();
    QSettings settings;
    QStringList manifests = settings.value("manifests").toString().split(" ");

    for(QString manifest : manifests) {
        QUrl url = QUrl::fromUserInput(manifest);

        // Download the manifest if it's not a local file.
        if(!url.isLocalFile())
            downloadManifest(url);

        // Read the manifest from the local file system.
        else
            openManifest(url.toLocalFile());

    }

}

MainWindow::~MainWindow() {
    ++loadGeneration;
    cancelOperation();
    delete ui;
}
