// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include "ldaprelay.h"

#include "configuration.h"
#include "eventloop.h"
#include "mechanism.h"
#include "buffer.h"
#include "user.h"


class LdapRelayData
    : public Garbage
{
public:
    LdapRelayData()
        : mechanism( 0 ),
          state( LdapRelay::Working ),
          haveReadType( false ), responseLength( 0 )
        {}

    SaslMechanism * mechanism;

    LdapRelay::State state;
    bool haveReadType;
    uint responseLength;
};


/*! \class LdapRelay ldaprelay.h

    The LdapRelay class helps Mechanism relay SASL challenges and
    responses to and from an LDAP server. If the LDAP server accepts
    the authentication, then the user is accepted as an Archiveopteryx
    user.

    The LdapRelay state machine contains the following states:

    Working: The LDAP server still hasn't answered.

    BindFailed: We should reject this authentication.

    BindSucceeded: We should accept this authentication.
*/



/*! Constructs an LdapRelay to verify whatever \a mechanism needs. */

LdapRelay::LdapRelay( SaslMechanism * mechanism )
    : Connection( Connection::socket( server().protocol() ),
                  Connection::LdapRelay ),
      d ( new LdapRelayData )
{
    d->mechanism = mechanism;
    setTimeoutAfter( 30 );
    connect( server() );
    EventLoop::global()->addConnection( this );
}


/*! Reacts to incoming packets from the LDAP server, changes the
    object's state, and eventually notifies the Mechanism. \a e is as
    for Connection::react().
*/

void LdapRelay::react( Event e )
{
    if ( d->state != Working )
        return;

    switch( e ) {
    case Read:
        parse();
        break;

    case Connection::Timeout:
        fail( "LDAP server timeout" );
        break;

    case Connect:
        bind();
        break;

    case Error:
        fail( "Unexpected error" );
        break;

    case Close:
        fail( "Unexpected close by LDAP server" );
        break;

    case Shutdown:
        break;
    }

    if ( d->state == Working )
        return;

    setState( Closing );
    d->mechanism->execute();
}


/*! Returns the address of the LDAP server used. */

Endpoint LdapRelay::server()
{
    return Endpoint(
        Configuration::text( Configuration::LdapServerAddress ),
        Configuration::scalar( Configuration::LdapServerPort ) );

}


/*! Parses the response the server sends, which has to be a bind
    response.
*/

void LdapRelay::parse()
{
    Buffer * r = readBuffer();
    uint byte;
    if ( !d->haveReadType ) {
        if ( r->size() < 2 )
            return;
        // LDAPMessage magic bytes (30 xx)
        //     30 -> universal context-specific zero
        //     0c -> length 12
        byte = (*r)[0];
        if ( byte != 0x30 ) {
            fail( "Expected LDAP type byte 0x30, received 0x" +
                  String::fromNumber( byte, 16 ).lower() );
            return;
        }
        d->responseLength = (*r)[1];
        d->haveReadType = true;
        r->remove( 2 );
    }

    if ( r->size() < d->responseLength )
        return;

    //  message-id (02 01 01)
    //     02 -> integer
    //     01 -> length
    //     01 -> message-id
    if ( (*r)[0] != 2 || (*r)[1] != 1 || (*r)[2] != 1 ) {
        fail( "Expected LDAP message-id to have type 2 length 1 ID 1, "
              "received type " + fn( (*r)[0] ) + " length " + fn( (*r)[1] ) +
              " ID " + fn( (*r)[2] ) );
        return;
    }
    r->remove( 3 );

    //  bindresponse (61 07)
    //     61 -> APPLICATION 1, BindResponse
    //     07 -> length of remaining bytes
    if ( (*r)[0] != 0x61 ) {
        fail( "Expected LDAP response type 0x61, received type " +
              String::fromNumber( (*r)[0] ) );
        return;
    }
    //uint bindResponseLength = (*r)[1];
    r->remove( 2 );

    //   resultcode
    //     0a -> enum
    //     01 -> length?
    //     00 -> success
    if ( (*r)[0] != 10 || (*r)[1] != 1 ) {
        fail( "Expected LDAP result code to have type 10 length 1, "
              "received type " + fn( (*r)[0] ) + " length " + fn( (*r)[1] ) );
        return;
    }
    uint resultCode = (*r)[2];
    r->remove( 3 );
    if ( resultCode != 0 )
        fail( "LDAP server refused authentication with result code " +
              fn( resultCode ) );
    else
        succeed();

    // I think we don't care about the rest of the data.

    //   matchedDN
    //     04 -> octetstring
    //     00 -> length
    if ( (*r)[1] + 2 >= (int)r->size() )
        return;
    r->remove( (*r)[1] + 2 );

    //   errorMessage
    //     04 -> octetstring
    //     00 -> length

    if ( (*r)[0] != 4 || (*r)[1] >= r->size() )
        return;
    uint l = (*r)[1];
    r->remove( 2 );
    String e( r->string( l ) );
    if ( !e.isEmpty() )
        log( "Note: LDAP server returned error message: " + e );

    if ( d->state != BindFailed )
        unbind();
}


/*! Sends a single bind request. */

void LdapRelay::bind()
{
    // LDAP message
    //     30 -> LDAP message
    //     nn -> number of remaining bytes

    String m;
    m.append( 0x30 );

    // Message id
    //     02 -> integer
    //     01 -> length
    //     01 -> message-id

    String id;
    id.append( "\002\001\001" );

    // Bind request
    //     60 -> APPLICATION 0, i.e. bind request
    //     nn -> number of remaining bytes

    String h;
    h.append( 0x60 );

    //   version (03)
    //    02 -> integer
    //    01 -> length
    //    03 -> version

    String s;
    s.append( "\002\001\003" );

    //   name (?)
    //    04 -> octetstring
    //    nn -> length
    //    s* -> DN

    s.append( 0x04 );
    String dn;
    if ( d->mechanism->user() )
        dn = d->mechanism->user()->ldapdn().utf8();
    s.append( (char)dn.length() );
    s.append( dn );

    //   authentication
    //    80 -> type: context-specific universal zero, and zero is "password"
    //    nn -> length
    //    s* -> password

    s.append( 0x80 );
    String pw = d->mechanism->secret().utf8();
    s.append( (char)pw.length() );
    s.append( pw );

    // Fill in the length fields

    h.append( (char)s.length() );
    m.append( (char)( id.length() + h.length() + s.length() ) );

    enqueue( m );
    enqueue( id );
    enqueue( h );
    enqueue( s );
}


/*! Sends an unbind request. */

void LdapRelay::unbind()
{
    String m;
    m.append( 0x30 );
    m.append( 0x05 );
    m.append( 0x02 );
    m.append( 0x01 );
    m.append( 0x03 );
    m.append( 0x42 );
    m.append( "\000", 1 );
    enqueue( m );
}


/*! This private helper sets the state and logs \a error. */

void LdapRelay::fail( const String & error )
{
    if ( d->state != Working )
        return;

    d->state = BindFailed;
    log( error );
}


/*! This private helper sets the state and logs. */

void LdapRelay::succeed()
{
    if ( d->state != Working )
        return;

    d->state = BindSucceeded;
    log( "LDAP authentication succeeded" );
}


/*! Returns the relay object's current state. */

LdapRelay::State LdapRelay::state() const
{
    return d->state;
}