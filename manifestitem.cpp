#include "manifestitem.h"

#include <QCryptographicHash>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QDebug>
#include <QSaveFile>

ManifestItem::ManifestItem (
            QString &fname,
            QByteArray &md5,
            long size,
            QList<QUrl*> &urls,
            QObject *parent )
    : QObject(parent),
    fname(fname),
    md5(md5),
    size(size),
    urls(urls) {}

bool ManifestItem::validate() {

    QFile file (fname);
    QFileInfo info(file.fileName());
    QCryptographicHash hash(QCryptographicHash::Md5);

    bool valid = info.exists()
            && info.size() == size
            && file.open(QFile::ReadOnly)
            && hash.addData(&file)
            && hash.result() == md5;
    file.close();
    return valid;

}
