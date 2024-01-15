#ifndef ERRORWINDOW_H
#define ERRORWINDOW_H

#include <QDialog>

namespace Ui {
class ErrorWindow;
}

class ErrorWindow : public QDialog
{
    Q_OBJECT

public:
    explicit ErrorWindow(QWidget *parent = nullptr);
    ~ErrorWindow();
    void addErrors(QStringList errors);

private:
    Ui::ErrorWindow *ui;
};

#endif // ERRORWINDOW_H
