#ifndef LAUNCHPROFILEITEMDELEGATE_H
#define LAUNCHPROFILEITEMDELEGATE_H

#include <QPainter>
#include <QAbstractItemDelegate>

class LaunchProfileItemDelegate : public QAbstractItemDelegate
{

public:
    explicit LaunchProfileItemDelegate(QObject *parent = nullptr);
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

#endif // LAUNCHPROFILEITEMDELEGATE_H
