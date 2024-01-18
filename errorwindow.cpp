#include <QClipboard>

#include "errorwindow.h"
#include "ui_errorwindow.h"

ErrorWindow::ErrorWindow(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::ErrorWindow)
{
    ui->setupUi(this);

    QFile logFile("log.txt");
    logFile.open(QIODevice::ReadOnly);
    ui->errorText->setPlainText(logFile.readAll());
    adjustSize();
    logFile.close();

    connect(ui->copyButton, &QPushButton::pressed, [this]{
        QApplication::clipboard()->setText(ui->errorText->toPlainText());
    });
}

ErrorWindow::~ErrorWindow()
{
    delete ui;
}
