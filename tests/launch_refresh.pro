QT += core gui widgets xml network concurrent testlib
CONFIG += testcase console c++11
TEMPLATE = app
TARGET = tst_launchrefresh
INCLUDEPATH += ..
SOURCES += tst_launchrefresh.cpp \
           ../mainwindow.cpp ../manifest.cpp ../manifestitem.cpp \
           ../serverentry.cpp ../optionswindow.cpp ../errorwindow.cpp \
           ../launchprofileitemdelegate.cpp
HEADERS += ../mainwindow.h ../manifest.h ../manifestitem.h \
           ../serverentry.h ../optionswindow.h ../errorwindow.h \
           ../launchprofileitemdelegate.h
FORMS += ../mainwindow.ui ../optionswindow.ui ../errorwindow.ui
