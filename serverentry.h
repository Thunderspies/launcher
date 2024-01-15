#ifndef SERVERENTRY_H
#define SERVERENTRY_H

#include <QObject>
#include <QUrl>
#include <QFile>

class Manifest;

class ServerEntry : public QObject
{
    Q_OBJECT
public:
    explicit ServerEntry (
            QString name,
            QUrl motd,
            QUrl icon,
            QString client,
            QString args,
            Manifest *manifest,
            QObject *parent = nullptr );

    QString name;
    QUrl motd;
    QUrl icon;
    QString client;
    QString args;
    Manifest *manifest;

};

#endif // SERVERENTRY_H
