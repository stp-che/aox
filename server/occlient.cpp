// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include "occlient.h"

#include "scope.h"
#include "string.h"
#include "configuration.h"
#include "endpoint.h"
#include "mailbox.h"
#include "buffer.h"
#include "loop.h"
#include "log.h"


class OCCData {
public:
};

static class OCClient *client;


/*! \class OCClient occlient.h
    This class is responsible for interacting with the OCServer.

    Every IMAP server initiates a connection to the cluster coordination
    server by calling the static setup() function at startup. This class
    assumes responsibility for interacting with the rest of the cluster.
*/


/*! Creates an OCClient object for the fd \a s. */

OCClient::OCClient( int s )
    : Connection( s, Connection::OryxClient ), d( new OCCData )
{
    Loop::addConnection( this );
}


OCClient::~OCClient()
{
    Loop::removeConnection( this );
}


/*! Connects to the configured OCD server on ocdhost.
    Expects to be called from ::main().
*/

void OCClient::setup()
{
    Configuration::Text ocdHost( "ocd-address", "127.0.0.1" );
    Configuration::Scalar ocdPort( "ocd-port", 2050 );
    Endpoint e( ocdHost, ocdPort );

    if ( !e.valid() ) {
        ::log( "Invalid ocdhost address <" + ocdHost + "> port <" +
               fn( ocdPort ) + ">\n", Log::Disaster );
        return;
    }

    client = new OCClient( Connection::socket( e.protocol() ) );
    client->setBlocking( true );

    if ( client->connect( e ) < 0 ) {
        ::log( "Unable to connect to ocdhost " + e.string() + "\n",
               Log::Disaster );
        return;
    }

    client->setBlocking( false );
}


void OCClient::react( Event e )
{
    switch ( e ) {
    case Connect:
    case Timeout:
    case Shutdown:
        break;

    case Read:
        parse();
        break;

    case Close:
    case Error:
        Loop::shutdown();
        break;
    }
}


/*! Parses messages from the OCServer. */

void OCClient::parse()
{
    String *s = readBuffer()->removeLine();

    if ( !s )
        return;

    int i = s->find( ' ' );
    String tag = s->mid( 0, i );
    int j = s->find( ' ', i+1 );
    String msg = s->mid( i, j-1 ).lower().stripCRLF();
    String arg = s->mid( j+1 ).stripCRLF();

    log( "OCClient received tag " + tag + " message " + msg +
         " arguments <<" + arg + ">>", Log::Debug );

    if ( msg == "shutdown" )
        Loop::shutdown();
    else if ( msg == "mailbox" )
        updateMailbox( arg );
}


/*! This static function sends the message \a s to the OCServer. */

void OCClient::send( const String &s )
{
    client->enqueue( "* " + s + "\n" );
    client->write();
}


/*! Parse and acts on a single mailbox update. A Mailbox update
    changes a single aspect of a mailbox, which may be whether it is
    deleted or what its UIDNEXT value is.

    The format is: Mailbox name quoted using String::quoted(),
    followed by a space, followed by the attribute name (deleted or
    uidnext) followed by '=', followed by the value (t or f for
    deleted, a decimal integer for uidnext).
*/

void OCClient::updateMailbox( const String & arg )
{
    uint i = arg.length();
    while ( i > 0 && arg[i] != '"' )
        i--;
    i++;
    String mailboxName = arg.mid( 0, i );
    if ( !mailboxName.isQuoted() ) {
        log( "Mailbox name not quoted: " + mailboxName, Log::Error );
        return;
    }
    Mailbox * m = Mailbox::obtain( mailboxName.unquoted() );
    if ( !m ) {
        log( "Mailbox name syntactically invalid: " + mailboxName.unquoted(),
             Log::Error );
        return;
    }
    String rest = arg.mid( i );
    if ( rest == " deleted=t" ) {
        if ( !m->deleted() )
            log( "OCClient deleted mailbox " + m->name(), Log::Debug );
        m->setDeleted( true );
    }
    else if ( rest == " deleted=f" ) {
        if ( m->deleted() )
            log( "OCClient undeleted mailbox " + m->name(), Log::Debug );
        m->setDeleted( false );
    }
    if ( rest.startsWith( " uidnext=" ) ) {
        bool ok;
        uint n = rest.mid( 9 ).number( &ok );
        if ( !ok ) {
            log( "Unable to parse UIDNEXT value: " + rest.mid( 9 ),
                 Log::Error );
        }
        else {
            if ( m->uidnext() != n )
                log( "OCClient set mailbox " + m->name() +
                     " to uidnext " + fn( n ), Log::Debug );
            m->setUidnext( n );
        }
    }
    else {
        log( "Unable to parse mailbox changes: " + rest, Log::Error );
    }
}
