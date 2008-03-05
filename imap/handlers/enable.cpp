// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include "enable.h"

#include "capability.h"


/*! \class Enable enable.h

    The Enable class implements the IMAP ENABLE command as defined by
    RFC 5161. Really simple command.
*/



Enable::Enable()
    : Command(), condstore( false ), annotate( false )
{
}


void Enable::parse()
{
    if ( !nextChar() == ' ' )
        error( Bad, "No capabilities enabled" );
    while ( ok() && nextChar() == ' ' ) {
        space();
        String capability = atom().upper();
        if ( capability == "CONDSTORE" ) {
            condstore = true;
        }
        else if ( capability == "ANNOTATE" ) {
            annotate = true;
        }
        else {
            String all = Capability::capabilities( imap(), true ).upper();
            StringList::Iterator s( StringList::split( ' ', all ) );
            while ( s && capability != *s )
                ++s;
            if ( s )
                error( Bad, "Capability " + *s + " is not subject to Enable" );
        }
    }
    end();
}


void Enable::execute()
{
    String r = "ENABLED";
    if ( condstore ) {
        imap()->setClientSupports( IMAP::Condstore );
        r.append( " CONDSTORE" );
    }
    if ( annotate ) {
        imap()->setClientSupports( IMAP::Annotate );
        r.append( " ANNOTATE" );
    }
    respond( r );
    finish();
}
