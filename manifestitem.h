#ifndef MANIFESTITEM_H
#define MANIFESTITEM_H

#include <QObject>

class ManifestItem : public QObject
{
    Q_OBJECT
public:
    explicit ManifestItem (
            QString &fname,
            QByteArray &md5,
            long size,
            QList<QUrl*> &urls,
            QObject *parent = nullptr );
    bool validate();

    QString fname;
    QByteArray md5;
    long size;
    QList<QUrl*> urls;

};

#endif // MANIFESTITEM_H
