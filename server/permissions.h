// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#ifndef PERMISSIONS_H
#define PERMISSIONS_H

#include "event.h"
#include "string.h"


class Permissions
    : public EventHandler
{
public:
    Permissions( class Mailbox *, const String &, const String & );

    Permissions( class Mailbox *, class User *,
                 class EventHandler * );

    enum Right {
        Lookup, // l
        Read, // r
        KeepSeen, // s
        Write, // w
        Insert, // i
        Post, // p
        CreateMailboxes, // k
        DeleteMailbox, // x
        DeleteMessages, // t
        Expunge, // e
        Admin, // a
        WriteSharedAnnotation, // n
        // New rights go above this line.
        NumRights
    };

    bool ready();
    void execute();

    void set( const String & );
    void allow( const String & );
    void disallow( const String & );
    bool allowed( Right );

    String string() const;

    Mailbox * mailbox() const;
    User * user() const;

    static char rightChar( Permissions::Right );
    static String describe( char );

    static bool validRight( char );
    static bool validRights( const String & );

    static String all();

    static const char * rights;

private:
    class PermissionData * d;
};


class PermissionsChecker
    : public Garbage
{
public:
    PermissionsChecker();

    void require( Permissions *, Permissions::Right );

    Permissions * permissions( class Mailbox *, class User * ) const;

    bool allowed() const;
    bool ready() const;

    String error() const;

private:
    class PermissionsCheckerData * d;
};


#endif
