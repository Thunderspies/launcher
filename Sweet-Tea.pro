QT       += core gui xml network widgets concurrent

CONFIG += c++11

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

SOURCES += \
    errorwindow.cpp \
    launchprofileitemdelegate.cpp \
    main.cpp \
    mainwindow.cpp \
    manifest.cpp \
    manifestitem.cpp \
    optionswindow.cpp \
    serverentry.cpp

HEADERS += \
    errorwindow.h \
    launchprofileitemdelegate.h \
    mainwindow.h \
    manifest.h \
    manifestitem.h \
    optionswindow.h \
    serverentry.h

FORMS += \
    errorwindow.ui \
    mainwindow.ui \
    optionswindow.ui

RC_ICONS = icon.ico

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
