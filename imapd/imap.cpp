/*! \class IMAP imap.h
    This class implements the IMAP server as seen by clients.

    This class is responsible for interacting with IMAP clients, and for
    overseeing the operation of individual command handlers. It looks at
    client input to decide which Command to defer the real work to, and
    ensures that the handler is called at the appropriate times.

    The IMAP state (RFC 3501 section 3) and Idle state (RFC 2177) are
    both encoded here, exactly as in the specification.
*/

#include "imap.h"

#include "arena.h"
#include "scope.h"
#include "string.h"
#include "buffer.h"
#include "list.h"
#include "mailbox.h"
#include "command.h"
#include "handlers/capability.h"
#include "log.h"

#include <time.h>


static bool endsWithLiteral( const String *, uint *, bool * );


class IMAPData {
public:
    IMAPData()
        : state( IMAP::NotAuthenticated ),
          logger( new Log ), cmdArena( 0 ), args( 0 ),
          readingLiteral( false ), literalSize( 0 ),
          reader( 0 ), mailbox( 0 ),
          idle( false )
    {}
    ~IMAPData() {
        delete cmdArena;
    }

    IMAP::State state;

    Log * logger;
    Arena * cmdArena;
    List< String > * args;

    bool readingLiteral;
    uint literalSize;

    Command * reader;
    List< Command > commands;

    Mailbox *mailbox;
    String login;

    bool idle;
};


/*! Creates an IMAP server, and sends an initial CAPABILITY response to
    the client.
*/

IMAP::IMAP( int s )
    : Connection( s ), d( new IMAPData )
{
    if ( s < 0 )
        return;

    d->logger->log( "Accepted IMAP connection from " + peer() );

    writeBuffer()->append( String( "* OK [CAPABILITY " ) +
                           Capability::capabilities() + "]\r\n");
    setTimeout( time(0) + 1800 );
}


/*! Destroys the IMAP server. */

IMAP::~IMAP()
{
    delete d;
}


/*! Handles incoming data and timeouts. */

void IMAP::react( Event e )
{
    switch ( e ) {
    case Read:
        parse();
        break;

    case Timeout:
        writeBuffer()->append( "* BYE autologout\r\n" );
        d->logger->log( "autologout" );
        Connection::setState( Closing );
        break;

    case Connect:
    case Error:
    case Close:
        if ( state() != Logout )
            d->logger->log( "Unexpected close by client" );
        break;

    case Shutdown:
        writeBuffer()->append( "* BYE server shutdown\r\n" );
        break;
    }

    d->logger->commit();
    runCommands();
    d->logger->commit();

    if ( timeout() == 0 )
        setTimeout( time(0) + 1800 );
    if ( state() == Logout )
        Connection::setState( Closing );
}


/*! Reads input from the client, and feeds it to the appropriate Command
    handlers.
*/

void IMAP::parse()
{
    Scope s;
    Buffer * r = readBuffer();

    while ( true ) {
        // We allocate and donate a new arena to each command we create,
        // and use it to allocate anything command-related in this loop.

        if ( !d->cmdArena )
            s.setArena( d->cmdArena = new Arena );
        if ( !d->args )
            d->args = new List< String >;

        // We read a line of client input, possibly including literals,
        // and create a Command to deal with it.

        if ( !d->readingLiteral && !d->reader ) {
            uint n;
            bool plus;
            String * s;

            // Do we have a complete line yet?

            s = r->removeLine();
            if ( !s )
                return;

            d->args->append( s );

            if ( endsWithLiteral( s, &n, &plus ) ) {
                d->readingLiteral = true;
                d->literalSize = n;

                if ( !plus )
                    writeBuffer()->append( "+\r\n" );
            }

            // Have we finished reading the entire command?

            if ( !d->readingLiteral ) {
                addCommand();
                d->cmdArena = 0;
                d->args = 0;
            }
        }
        else if ( d->readingLiteral ) {
            // Have we finished reading a complete literal?
            if ( r->size() < d->literalSize )
                return;

            d->args->append( r->string( d->literalSize ) );
            r->remove( d->literalSize );
            d->readingLiteral = false;
        }
        else if ( d->reader ) {
            // If a Command has reserve()d input, we just feed it.
            d->reader->read();
        }
    }
}


/*! This function parses enough of the command line to create a Command,
    and then uses it to parse the rest of the input.
*/

void IMAP::addCommand()
{
    String * s = d->args->first();
    d->logger->log( Log::Debug, "Received " +
                    String::fromNumber( (d->args->count() + 1)/2 ) +
                    "-line command: " + *s );

    String tag, command;

    // Parse the tag: A nonzero sequence of any ASTRING-CHAR except '+'.

    char c;
    uint i = 0;

    while ( i < s->length() && ( c = (*s)[i] ) > ' ' && c < 127 &&
            c != '(' && c != ')' && c != '{' && c != '%' && c != '*' &&
            c != '"' && c != '\\' && c != '+' )
        i++;

    if ( i < 1 || c != ' ' ) {
        writeBuffer()->append( "* BAD tag\r\n" );
        d->logger->log( "Unable to parse tag. Line: " + *s );
parseError:
        delete d->cmdArena;
        return;
    }

    tag = s->mid( 0, i );


    // Parse the command name (a single atom).

    uint j = ++i;

    while ( i < s->length() && ( c = (*s)[i] ) > ' ' && c < 127 &&
            c != '(' && c != ')' && c != '{' && c != '%' && c != '*' &&
            c != '"' && c != '\\' && c != ']' )
        i++;

    if ( i == j ) {
        writeBuffer()->append( "* BAD no command\r\n" );
        d->logger->log( "Unable to parse command. Line: " + *s );
        goto parseError;
    }

    command = s->mid( j, i-j );


    // Try to create a command handler.

    Command *cmd = Command::create( this, command, tag, d->args, d->cmdArena );

    if ( !cmd ) {
        d->logger->log( "Unknown command '" + command + "' (tag '" +
                        tag + "')" );
        writeBuffer()->append( tag + " BAD unknown command: " + command +
                               "\r\n" );
        goto parseError;
    }


    // Use this Command to parse the command line.

    cmd->step( i );
    cmd->parse();


    // Then add it to our list of Commands.

    if ( cmd->ok() && cmd->state() == Command::Executing &&
         !d->commands.isEmpty() )
    {
        // We're already executing d->commands. Can we start cmd now?

        if ( cmd->group() == 0 ) {
            // No, we can't.
            cmd->setState( Command::Blocked );
            cmd->logger()->log( Log::Debug,
                                "Blocking execution of " + tag +
                                " (concurrency not allowed for " +
                                command + ")" );
        }
        else {
            // Do the other d->commands belong to the same group?
            List< Command >::Iterator it;

            it = d->commands.first();
            while ( it && it->group() == cmd->group() )
                it++;

            if ( it ) {
                cmd->setState( Command::Blocked );
                cmd->logger()->log( Log::Debug,
                                    "Blocking execution of " + tag +
                                    " until it can be exectuted" );
            }
        }
    }

    d->commands.append( cmd );
}


/*! Returns the current state of this IMAP session, which is one of
    NotAuthenticated, Authenticated, Selected and Logout.
*/

IMAP::State IMAP::state() const
{
    return d->state;
}


/*! Sets this IMAP connection to be in state \a s. The initial value
    is NotAuthenticated.
*/

void IMAP::setState( State s )
{
    if ( s == d->state )
        return;
    d->state = s;
    String name;
    switch ( s ) {
    case NotAuthenticated:
        name = "not authenticated";
        break;
    case Authenticated:
        name = "authenticated";
        break;
    case Selected:
        name = "selected";
        break;
    case Logout:
        name = "logout";
        break;
    };
    d->logger->log( "Changed to " + name + " state" );
}


/*! Notifies this IMAP connection that it is idle if \a i is true, and
    not idle if \a i is false. An idle connection (see RFC 2177) is one
    in which e.g. EXPUNGE/EXISTS responses may be sent at any time. If a
    connection is not idle, such responses must be delayed until the
    client can listen to them.
*/

void IMAP::setIdle( bool i )
{
    if ( i == d->idle )
        return;
    d->idle = i;
    if ( i )
        d->logger->log( "entered idle mode" );
    else
        d->logger->log( "left idle mode" );
}


/*! Returns true if this connection is idle, and false if it is
    not. The initial (and normal) state is false.
*/

bool IMAP::idle() const
{
    return d->idle;
}


/*! Notifies the IMAP object that it's logged in as \a name. */

void IMAP::setLogin( const String & name )
{
    if ( state() != NotAuthenticated ) {
        // not sure whether I like this... on one hand, it does
        // prevent change of login. on the other, how could that
        // possibly happen?
        d->logger->log( Log::Error,
                        "ignored setLogin("+name+") due to wrong state" );
        return;
    }
    d->login = name;
    d->logger->log( "logged in as " + name );
    setState( Authenticated );
}


/*! Returns the current login name. Initially, the login name is an
    empty string.

    The return value is meaningful only in Authenticated and Selected
    states.
*/

String IMAP::login()
{
    return d->login;
}


/*! Returns the currently-selected Mailbox. */

Mailbox *IMAP::mailbox()
{
    return d->mailbox;
}


/*! Sets the currently-selected Mailbox to \a m. Note that the new
    mailbox must not need expunges. If it needs expunges, IMAP SELECT
    might return Expunge responses, which would be very bad.
*/

void IMAP::setMailbox( Mailbox *m )
{
    if ( m == d->mailbox )
        return;

    d->mailbox = m;

    if ( m )
        d->logger->log( "now using mailbox " + m->name() );
}


/*! Reserves input from the connection for \a command.

    When more input is available, \a Command::read() is called, and as
    soon as the command has read enough, it must call reserve( 0 ) to
    hand the connection back to the general IMAP parser.

    Most commands should never need to call this; it is provided for
    commands that need to read more input after parsing has completed,
    such as IDLE and AUTHENTICATE.

    There is a nasty gotcha: If a command reserves the input stream and
    calls Command::error() while in Blocked state, the command is
    deleted, but there is no way to hand the input stream back to the
    IMAP object. Only the relevant Command knows when it can hand the
    input stream back.

    Therefore, Commands that call reserve() simply must hand it back properly
    before calling Command::error() or Command::setState().
*/

void IMAP::reserve( Command * command )
{
    d->reader = command;
}


/*! Calls execute() on all currently operating commands, and if
    possible calls emitResponses() and retires those which can be
    retired.
*/

void IMAP::runCommands()
{
    Command * c;
    bool more = true;

    while ( more ) {
        more = false;

        List< Command >::Iterator i;

        i = d->commands.first();
        while ( i ) {
            c = i++;
            Scope x( c->arena() );

            if ( c->ok() ) {
                if ( c->state() == Command::Executing )
                    c->execute();
            }
            if ( !c->ok() )
                c->setState( Command::Finished );
            if ( c->state() == Command::Finished )
                c->emitResponses();
        }

        i = d->commands.first();
        while ( i ) {
            if ( i->state() == Command::Finished )
                delete d->commands.take(i);
            else
                i++;
        }

        // XXX: Don't we need to check the group here?
        c = d->commands.first();
        if ( c && c->ok() && c->state() == Command::Blocked ) {
            c->setState( Command::Executing );
            more = true;
        }
    }
}


/*  This static helper function returns true if \a s ends with an IMAP
    literal specification. If so, it sets \a *n to the number of bytes
    in the literal, and \a *plus to true if the number had a trailing
    '+' (for LITERAL+). Returns false if it couldn't find a literal.
*/

static bool endsWithLiteral( const String *s, uint *n, bool *plus )
{
    if ( !s->endsWith( "}" ) )
        return false;

    uint i = s->length() - 2;
    if ( (*s)[i] == '+' ) {
        *plus = true;
        i--;
    }

    uint j = i;
    while ( i > 0 && (*s)[i] >= '0' && (*s)[i] <= '9' )
        i--;

    if ( (*s)[i] != '{' )
        return false;

    bool ok;
    *n = s->mid( i+1, j-i+1 ).number( &ok );

    return ok;
}
