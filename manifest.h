#ifndef MANIFEST_H
#define MANIFEST_H

#include "manifestitem.h"
#include "serverentry.h"

#include <QObject>
#include <QtXml>

class Manifest : public QObject
{
    Q_OBJECT
public:
    explicit Manifest(QDomDocument &doc, QByteArray checksum, QObject *parent = nullptr);
    bool validate();

    QByteArray checksum;
    QList<ManifestItem*> items;
    QList<QString*> deletions;
    QList<ServerEntry*> servers;

};

#endif // MANIFEST_H
