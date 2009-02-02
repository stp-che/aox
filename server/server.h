// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#ifndef SERVER_H
#define SERVER_H

#include "global.h"


class EString;


class Server
    : public Garbage
{
public:
    Server( const char *, int, char *[] );

    enum ChrootMode {
        JailDir, LogDir, MessageCopyDir, TlsProxyDir
    };

    void setChrootMode( ChrootMode );

    enum Stage {
        Configuration,
        NameResolution,
        Files,
        LogSetup,
        Loop,
        Report,
        Fork,
        PidFile,
        LogStartup,
        Secure,
        Finish // MUST BE LAST
    };

    void setup( Stage );
    void run();

    static EString name();
    static bool useCache();

    static void killChildren();

private:
    static class ServerData * d;

    void configuration();
    void nameResolution();
    void files();
    void loop();
    void logSetup();
    void fork();
    void pidFile();
    void logStartup();
    void secure();
};


#endif
