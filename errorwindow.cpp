#include "errorwindow.h"
#include "ui_errorwindow.h"

ErrorWindow::ErrorWindow(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::ErrorWindow)
{
    ui->setupUi(this);
}

ErrorWindow::~ErrorWindow()
{
    delete ui;
}

void ErrorWindow::addErrors(QStringList errors) {
    ui->errors->addItems(errors);
}
