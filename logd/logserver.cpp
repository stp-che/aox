// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include "logserver.h"

#include "allocator.h"
#include "buffer.h"
#include "dict.h"
#include "list.h"
#include "file.h"
#include "loop.h"
#include "log.h"

// fprintf, stderr
#include <stdio.h>


static uint id;
static File *logFile;
static Log::Severity logLevel;

static Log::Facility facility( const String & );
static Log::Severity severity( const String & );


/*! \class LogServer logserver.h
    The LogServer listens for log items on a TCP socket and commits
    them to file intelligently.

    Each logged item belongs to a transaction (a base-36 number), has a
    level of seriousness (debug, info, error or disaster) and a text. If
    the transaction ID is 0, the item is logged immediately, else it's
    held in memory until the transaction is committed.

    When a log transaction is committed, the client can decide what to
    commit. For example, debugging can be discarded and the rest logged.

    If the client crashes or unexpectedly closes the TCP connection,
    everything belonging to pending transactions is immediately written
    to disk.
*/

class LogServerData {
public:
    LogServerData(): id( ::id++ ), name( "(Anonymous)" ) {}

    uint id;

    class Line {
    public:
        Line( Log::Facility f, Log::Severity s, const String &l )
            : facility( f ), severity( s ), line( l )
        {}

        Log::Facility facility;
        Log::Severity severity;
        String line;
    };

    typedef List<Line> Queue;

    Dict<Queue> pending;

    String name;
};


/*! Constructs an empty LogServer, listening on socket \a s. */

LogServer::LogServer( int s )
    : Connection( s, Connection::LogServer ), d( new LogServerData )
{
    Loop::addConnection( this );
}


/*! Constructs a LogServer which listens nowhere. This can effectively
    only be used by SelfLogger.
*/

LogServer::LogServer()
    : Connection(), d( new LogServerData )
{
}


void LogServer::react( Event e )
{
    switch ( e ) {
    case Read:
        parse();
        break;
    case Timeout:
        // Timeout never should happen
    case Shutdown:
        log( 0, Log::Immediate, Log::Debug, "log server shutdown" );
        commitAll();
        break;
    case Connect:
    case Error:
    case Close:
        commitAll();
        break;
    };
}


/*! Parses log messages from the input buffer. */

void LogServer::parse()
{
    String *s;
    while ( ( s = readBuffer()->removeLine() ) != 0 )
        processLine( *s );
}


/*! Adds a single \a line to the log output.

    The line must consist of a client identifier (numbers and slashes)
    followed by a space, the message facility, a slash and a severity,
    followed by a space and the log message.
*/

void LogServer::processLine( const String &line )
{
    if ( line.startsWith( "name " ) ) {
        d->name = line.mid( 5 ).simplified();
        return;
    }

    uint cmd = 0;
    uint msg = 0;

    cmd = line.find( ' ' );
    if ( cmd > 0 )
        msg = line.find( ' ', cmd+1 );
    if ( msg <= cmd+1 )
        return;

    String transaction = line.mid( 0, cmd );
    String priority = line.mid( cmd+1, msg-cmd-1 );
    String parameters = line.mid( msg+1 ).simplified();

    bool c = false;
    if ( priority == "commit" ) {
        priority = parameters;
        parameters = "";
        c = true;
    }

    int n = priority.find( '/' );
    if ( n < 0 )
        return;

    Log::Facility f = facility( priority.mid( 0, n ) );
    Log::Severity s = severity( priority.mid( n+1 ) );

    if ( c ) {
        commit( transaction, f, s );
    }
    else if ( s >= logLevel || f == Log::Immediate ) {
        if ( s >= Log::Error )
            s = Log::Debug;
        commit( transaction, f, s );
        output( transaction, f, s, parameters );
    }
    else {
        log( transaction, f, s, parameters );
    }
}


/*! Saves \a line with tag \a t, facility \a f, and severity \a s in the
    list of pending output lines. If \a f is Immediate, however, \a line
    is logged immediately.
*/

void LogServer::log( String t, Log::Facility f, Log::Severity s,
                     const String &line )
{
    LogServerData::Queue * q = d->pending.find( t );
    if ( !q ) {
        q = new LogServerData::Queue;
        d->pending.insert( t, q );
    }
    q->append( new LogServerData::Line( f, s, line ) );
}


/*! Commits all log lines of \a severity or higher from transaction \a
    tag to the log file, and discards lines of lower severity. It does
    nothing with the \a facility yet.
*/

void LogServer::commit( String tag,
                        Log::Facility facility, Log::Severity severity )
{
    LogServerData::Queue * q = d->pending.find( tag );
    if ( !q || q->isEmpty() )
        return;

    List< LogServerData::Line >::Iterator i( q->first() );
    while ( i ) {
        if ( i->severity >= severity )
            output( tag, i->facility, i->severity, i->line );
        ++i;
    }
    q->clear();
}



/*! Commits all messages made to all transactions. */

void LogServer::commitAll()
{
    StringList keys( d->pending.keys() );
    StringList::Iterator i( keys.first() );
    while ( i && d->pending.find( *i )->isEmpty() )
        ++i;
    if ( !i )
        return;

    output( 0, Log::Immediate, Log::Error,
            d->name + " unexpectedly died. "
            "All messages in unfinished transactions follow." );
    i = keys.first();
    while ( i ) {
        commit( *i, Log::General, Log::Debug );
        ++i;
    }
}


/*! This private function actually writes \a line to the log file with
    the \a tag, facility \a f, and severity \a s converted into their
    textual representations.
*/

void LogServer::output( String tag, Log::Facility f, Log::Severity s,
                        const String &line )
{
    String msg;
    msg.reserve( line.length() );

    msg.append( Log::facility( f ) );
    msg.append( "/" );
    msg.append( Log::severity( s ) );
    msg.append( ": " );
    msg.append( fn( d->id, 36 ) );
    msg.append( "/" );
    msg.append( tag );
    msg.append( ": " );
    msg.append( line );
    msg.append( "\n" );

    if ( logFile )
        logFile->write( msg );
    else
        fprintf( stderr, "%s", msg.cstr() );
}


/*! Tells all LogServer object to write log information to \a name
    from now on. (If the file has to be created, \a mode is used.)
*/

void LogServer::setLogFile( const String &name, const String &mode )
{
    uint m;
    String s = mode;
    bool ok = false;

    if ( s.length() == 4 && s[0] == '0' )
        s = s.mid( 1 );

    if ( s.length() == 3 ) {
        if ( s[0] >= '0' && s[0] <= '9' &&
             s[1] >= '0' && s[1] <= '9' &&
             s[2] >= '0' && s[2] <= '9' )
        {
            m = ( s[0] - '0' ) * 0100 +
                ( s[1] - '0' ) * 010 +
                ( s[2] - '0' );
            ok = true;
        }

    }

    if ( !ok ) {
        ::log( "Invalid logfile-mode " + mode, Log::Disaster );
        return;
    }

    File * l = new File( name, File::Append, m );
    if ( !l->valid() ) {
        ::log( "Could not open log file " + name, Log::Disaster );
        return;
    }
    logFile = l;
    Allocator::addEternal( logFile, "logfile name" );
}


/*! Sets the log level to the Severity corresponding to \a l. */

void LogServer::setLogLevel( const String &l )
{
    logLevel = severity( l );
}


static Log::Facility facility( const String &l )
{
    Log::Facility f = Log::General;

    String p = l.lower();
    switch ( p[0] ) {
    case 'i':
    if ( p == "immediate" )
        f = Log::Immediate;
    else if ( p == "imap" )
        f = Log::IMAP;
    break;
    case 'c':
        //if ( p == "configuration" )
        f = Log::Configuration;
        break;
    case 'd':
        //if ( p == "database" )
        f = Log::Database;
        break;
    case 'a':
        //if ( p == "authentication" )
        f = Log::Authentication;
        break;
    case 's':
        if ( p == "smtp" )
            f = Log::SMTP;
        else if ( p == "server" )
            f = Log::Server;
    default:
        f = Log::Immediate;
        break;
    }
    return f;
}

static Log::Severity severity( const String &l )
{
    Log::Severity s = Log::Info;

    switch ( l[1] ) {
    case 'e':
    case 'E':
        s = Log::Debug;
        break;
    case 'n':
    case 'N':
        s = Log::Info;
        break;
    case 'r':
    case 'R':
        s = Log::Error;
        break;
    case 'i':
        s = Log::Disaster;
        break;
    }
    return s;
}


/*! Logs a final line in the logfile and reopens it. */

void LogServer::reopen( int )
{
    if ( !logFile )
        return;

    File * l = new File( logFile->name(), File::Append );
    if ( !l->valid() ) {
        ::log( "SIGHUP handler was unable to open new log file" +
               l->name(),
               Log::Disaster );
        ::commit();
        Loop::shutdown(); // XXX: perhaps better to switch to syslog
    }
    ::log( "SIGHUP caught. Closing and reopening log file " + logFile->name(),
           Log::Info );
    ::commit();
    delete logFile;
    logFile = l;
    ::log( "SIGHUP caught. Reopened log file " + logFile->name(),
           Log::Info );
    ::commit();
}
