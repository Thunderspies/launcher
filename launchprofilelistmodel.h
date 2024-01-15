#ifndef LAUNCHPROFILELISTMODEL_H
#define LAUNCHPROFILELISTMODEL_H

#include <QAbstractListModel>

#include "serverentry.h"

class LaunchProfileListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    explicit LaunchProfileListModel(QObject *parent = nullptr);

    // Basic functionality:
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    // Add data:
    bool insertRows(int row, int count, const QModelIndex &parent = QModelIndex()) override;

    // Remove data:
    bool removeRows(int row, int count, const QModelIndex &parent = QModelIndex()) override;

private:
    QList<ServerEntry> launchProfiles;

};

#endif // LAUNCHPROFILELISTMODEL_H
