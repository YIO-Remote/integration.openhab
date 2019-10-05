TEMPLATE        = lib
CONFIG         += plugin
QT             += core quick network
HEADERS         = openhab.h \
                  ../remote-software/sources/integrations/integration.h \
                  ../remote-software/sources/integrations/integrationinterface.h
SOURCES         = openhab.cpp
TARGET          = openhab
DESTDIR         = ../remote-software/plugins

# install
unix {
    target.path = /usr/lib
    INSTALLS += target
}
