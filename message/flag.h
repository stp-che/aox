// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#ifndef FLAG_H
#define FLAG_H

#include "estringlist.h"
#include "event.h"


class Query;


class Flag
    : public EventHandler
{
private:
    Flag();

public:
    static void setup();

    static EString name( uint );
    static uint id( const EString & );

    static uint largestId();
    static EStringList allFlags();
    static void addWatcher( class Session * );
    static void removeWatcher( class Session * );

    void execute();


private:
    friend class FlagObliterator;
    class FlagData * d;
};


#endif
