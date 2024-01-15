#include "manifest.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QtDebug>

Manifest::Manifest(QDomDocument &doc, QByteArray checksum, QObject *parent)
    : QObject(parent),
      checksum(checksum) {

    QDomNodeList filelists = doc.elementsByTagName("file");
    for(int i = 0; i < filelists.size(); i++) {
        QDomNode node = filelists.item(i);
        QString name = node
                .attributes()
                .namedItem("name")
                .nodeValue()
                .trimmed();
        long size = node
                .attributes()
                .namedItem("size")
                .nodeValue()
                .trimmed()
                .toLong();
        QByteArray md5 = QByteArray::fromHex(node
                                             .attributes()
                                             .namedItem("md5")
                                             .nodeValue()
                                             .trimmed()
                                             .toLatin1());
        QList<QUrl*> urls;
        QDomNodeList children = node.childNodes();
        for(int j = 0; j < children.size(); j++) {
            QDomNode child = children.item(j);
            urls.append(new QUrl(child.toElement().text().trimmed()));
        }
        if(!QDir(name).isAbsolute() && !name.contains(".."))
            if(size == 0)
                deletions.append(new QString(name));
            else
                items.append(new ManifestItem(name, md5, size, urls, this));
        else
            qWarning() << "insecure path not allowed for file: " << name;
    }

    QDomNodeList deleteList = doc.elementsByTagName("deletefile");
    for(int i = 0; i < deleteList.size(); i++) {
        QDomNode node = deleteList.item(i);
        QString name = node.toElement().text().trimmed();
        if(!QDir(name).isAbsolute() && !name.contains(".."))
            deletions.append(new QString(name));
        else
            qWarning() << "insecure path not allowed for file: " << name;
    }

    QDomNodeList profiles = doc.elementsByTagName("launch");
    for(int i = 0; i < profiles.size(); i++) {
        QDomNode node = profiles.item(i);
        QString name = node.toElement().text().trimmed();
        QUrl icon(node.attributes()
                  .namedItem("icon")
                  .nodeValue()
                  .trimmed());
        QString client = node.attributes()
                .namedItem("exec")
                .nodeValue()
                .trimmed();
        QUrl motd(node.attributes()
                  .namedItem("motd")
                  .nodeValue()
                  .trimmed());
        QString args = node.attributes()
                .namedItem("params")
                .nodeValue()
                .trimmed();
        if(!QDir(client).isAbsolute() && !client.contains(".."))
            servers.append(new ServerEntry(name, motd, icon, client, args, this, this));
        else
            qWarning() << "insecure path not allowed for client: " << client;
    }

}

bool Manifest::validate() {
    for(ManifestItem *item : items)
        item->validate();
    return true;
}
