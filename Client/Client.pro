#-------------------------------------------------
#
# Project created by QtCreator 2025-11-30T14:05:05
#
#-------------------------------------------------

QT       += core gui multimedia

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = Client
TEMPLATE = app

# Windows 可执行文件图标（qmake 会自动生成 Client_resource.rc 并用 windres 编译）
# 注意：RESOURCES 里的 images.qrc 只负责程序运行时的图标，不生成 exe 的文件图标
win32: RC_ICONS = $$PWD/resource/ico/app.ico

# DPAPI（CryptProtectData）：config.ini 里的登录口令用它加密，不再明文落盘
win32: LIBS += -lcrypt32

# The following define makes your compiler emit warnings if you use
# any feature of Qt which has been marked as deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if you use deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

CONFIG += c++11

SOURCES += \
        main.cpp \
        mainwindow.cpp \
    loginwidget.cpp \

HEADERS += \
        mainwindow.h \
    loginwidget.h

FORMS += \
        mainwindow.ui \
    loginwidget.ui \

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

include(comapi/comapi.pri)
include(basewidget/basewidget.pri)
include(netdb/netdb.pri)
include(uipage/uipage.pri)

# OpenSSL 4.1.0 (Win32) —— 消息端到端加密
include($$PWD/../3rdparty/openssl32/openssl.pri)

RESOURCES += \
    images.qrc
