QT +=  network sql

HEADERS += \
    $$PWD/clientsocket.h \
    $$PWD/myserver.h \
    $$PWD/databasemag.h \
    $$PWD/passwordhash.h \
    $$PWD/filesocket.h \
    $$PWD/handshake.h

SOURCES += \
    $$PWD/clientsocket.cpp \
    $$PWD/myserver.cpp \
    $$PWD/databasemag.cpp \
    $$PWD/passwordhash.cpp \
    $$PWD/filesocket.cpp \
    $$PWD/handshake.cpp
