#include "optionswindow.h"
#include "ui_optionswindow.h"

#include <QSettings>
#include <QDir>
#include <QStandardPaths>
#include <QDebug>

OptionsWindow::OptionsWindow(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::OptionsWindow)
{

    ui->setupUi(this);

    QSettings *settings = new QSettings();
    ui->ManifestList->addItems(settings->value("manifests").toString().split(" "));
    ui->DownloadPathLine->setText(
                settings->value("datadir",
                               QStandardPaths::writableLocation(
                                   QStandardPaths::DataLocation))
                .toString());
    ui->ParamLine->setText(settings->value("launchParams", "").toString());

    connect (
        ui->NewManifestLine,
        &QLineEdit::returnPressed,
        [this] {
            ui->ManifestList->addItem(ui->NewManifestLine->text());
            ui->NewManifestLine->clear();
        });

    connect (
        ui->AddManifestButton,
        &QPushButton::released,
        [this] {
            ui->ManifestList->addItem(ui->NewManifestLine->text());
            ui->NewManifestLine->clear();
        });

    connect (
        ui->RemoveManifestButton,
        &QPushButton::released,
        [this] {
            qDeleteAll(ui->ManifestList->selectedItems());
        });

    connect (
        ui->ApplyButton,
        &QPushButton::released,
        [this] {
            accept();
        });

    connect (
        ui->CancelButon,
        &QPushButton::released,
        [this] {
            reject();
        });

    connect (
        this,
        &QDialog::accepted,
        [this, settings] {
            QStringList manifests;
            for(int i = 0; i < ui->ManifestList->count(); i++)
                manifests.append(ui->ManifestList->item(i)->text());
            settings->setValue("manifests", manifests.join(" "));
            QString datadir = ui->DownloadPathLine->text().isEmpty()
                    ? QDir::currentPath()
                    : ui->DownloadPathLine->text();
            settings->setValue("datadir", datadir);
            QDir::setCurrent(ui->DownloadPathLine->text());
            QString launchParams = ui->ParamLine->text();
            settings->setValue("launchParams", launchParams);
        });

}

OptionsWindow::~OptionsWindow()
{
    delete ui;
}
