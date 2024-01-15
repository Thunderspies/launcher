#include "serverentry.h"

ServerEntry::ServerEntry (
        QString name,
        QUrl motd,
        QUrl icon,
        QString client,
        QString args,
        Manifest *manifest,
        QObject *parent )
    : QObject(parent),
      name(name),
      motd(motd),
      icon(icon),
      client(client),
      args(args),
      manifest(manifest) {}
