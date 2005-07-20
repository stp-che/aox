// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#ifndef LOGPANE_H
#define LOGPANE_H

#include <qwidget.h>
#include <qlistview.h>


class LogPane
    : public QWidget
{
    Q_OBJECT
public:
    LogPane( QWidget * );
    ~LogPane();

    QListView * listView() const;
    uint maxLines() const;

private:
    class LogPaneData * d;
};


class LogView
    : public QListView
{
    Q_OBJECT
public:
    LogView( LogPane * parent );
    ~LogView();

    void insertItem( QListViewItem * );

private:
    LogPane * parent;
};


#endif
