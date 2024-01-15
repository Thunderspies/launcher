#include "launchprofileitemdelegate.h"

#include <QDebug>

LaunchProfileItemDelegate::LaunchProfileItemDelegate(QObject *parent)
    : QAbstractItemDelegate(parent) { }

void LaunchProfileItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {

    painter->save();

    QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
    QString name = index.data(Qt::DisplayRole).toString();
    QString motd = index.data(Qt::UserRole).toString();

    QRect rect = option.rect;

    if(option.state & QStyle::State_Selected)
        painter->fillRect(rect, option.palette.highlight());

    rect = option.rect.adjusted(0, 0, 0, 0);
    if(!icon.isNull())
        icon.paint(painter, rect, Qt::AlignVCenter|Qt::AlignLeft);

    rect = option.rect.adjusted(option.fontMetrics.height() * 4, 0, 0, 0);
    painter->setFont(QFont(option.font.family(), 11, QFont::Bold));
    painter->setBrush(QColor(0, 0, 0));
    painter->drawText (
        rect.left(),
        rect.top(),
        rect.width(),
        rect.height(),
          Qt::AlignTop
        | Qt::AlignLeft
        | Qt::TextWordWrap
        | Qt::TextSingleLine,
        name,
        &rect );

    rect = option.rect.adjusted(option.fontMetrics.height() * 4, 0, 0, 0);
    painter->setFont(QFont(option.font.family(), 8, QFont::Normal));
    painter->setBrush(QColor(0, 0, 0));
    if(!motd.isNull())
        painter->drawText (
            rect.left(),
            rect.top(),
            rect.width(),
            rect.height(),
              Qt::AlignBottom
            | Qt::AlignLeft
            | Qt::TextWordWrap
            | Qt::TextSingleLine,
            motd,
            &rect );

    painter->restore();

}

QSize LaunchProfileItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const {
    QString motd = index.data(Qt::UserRole).toString();
    int width = 30 * option.fontMetrics.maxWidth();
    return QSize(width, option.fontMetrics.height() * 3);
}
