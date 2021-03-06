// Copyright 2009 The Archiveopteryx Developers <info@aox.org>

#include "recipient.h"

#include "date.h"
#include "address.h"
#include "mailbox.h"
#include "estringlist.h"
#include "configuration.h"


class RecipientData
    : public Garbage
{
public:
    RecipientData()
        : originalRecipient( 0 ),
          finalRecipient( 0 ),
          action( Recipient::Unknown ),
          lastAttemptDate( 0 ),
          mailbox( 0 )
    {}

    Address * originalRecipient;
    Address * finalRecipient;
    Recipient::Action action;
    EString status;
    EString remoteMta;
    EString diagnosticCode;
    Date * lastAttemptDate;
    EString finalLogId;
    Mailbox * mailbox;
};


/*! \class Recipient recipient.h

    The Recipient class holds information about a particular
    recipient, collected during a delivery attempt and optionally used
    for sending DSNs.
*/


/*!  Constructs a Recipient containing no data. The object must
     be completed using e.g. setFinalRecipient().
*/

Recipient::Recipient()
    : d( new RecipientData )
{
    // what a pity that zero-line functions may have more than zero bugs
}


/*! Constructs a Recipient object whose mailbox() is set to \a m. This
    function is provided for the convenience of code that creates an
    Injector.
*/

Recipient::Recipient( Mailbox * m )
    : d( new RecipientData )
{
    // Fortunately, one-line functions are guaranteed bug-free.
    d->mailbox = m;
}


/*! Constructs a Recipient object whose finalRecipient() is set to \a a
    and whose mailbox() is set to \a m. This function is provided only
    as a convenience.
*/

Recipient::Recipient( Address * a, Mailbox * m )
    : d( new RecipientData )
{
    d->finalRecipient = a;
    d->mailbox = m;
}


/*! Records that the message was originally sent to \a a. */

void Recipient::setOriginalRecipient( class Address * a )
{
    d->originalRecipient = a;
}


/*! Returns a pointer to the original recipient's address, or a null
    pointer if none is recorded.
*/

class Address * Recipient::originalRecipient() const
{
    return d->originalRecipient;
}


/*! Records that the message was finally sent to \a a.

    Calling both setFinalRecipient() and setOriginalRecipient() with
    the same address is discouraged.
*/

void Recipient::setFinalRecipient( class Address * a )
{
    d->finalRecipient = a;
}


/*! Returns a pointer to the final recipient's address, or a the
    originalRecipient() if none is recorded. If neither
    setFinalRecipient() nor setOriginalRecipient() has been called,
    finalRecipient() returns null.
*/

class Address * Recipient::finalRecipient() const
{
    if ( d->finalRecipient )
        return d->finalRecipient;
    return d->originalRecipient;
}


/*! Records that \a a is the action taken wrt. this recipient, and the
    resulting status \a s . The initial action() is Unknown and the
    initial status() an empty string.

    \a s must be a string containing three numbers separated by dots,
    e.g. "1.2.3" or "1000.2000.3000". The meaning of the numbers is as
    defined in RFC 3463.
*/

void Recipient::setAction( Action a, const EString & s )
{
    d->action = a;
    d->status = s;
}


/*! Returns the action recorded by setAction(). */

Recipient::Action Recipient::action() const
{
    return d->action;
}


/*! Returns the status recorded by setAction(). */

EString Recipient::status() const
{
    return d->status;
}


/*! Records that \a mta is the MTA to which we attempted to deliver
    this message the last time. The initial value is empty, which
    means that we didn't try to deliver the message to any remote MTA.
*/

void Recipient::setRemoteMTA( const EString & mta )
{
    d->remoteMta = mta;
}


/*! Returns the MTA recorded by setRemoteMTA(). */

EString Recipient::remoteMTA() const
{
    return d->remoteMta;
}


/*! Records that \a code is the diagnostic code resulting from the
    last delivery attempt. This must be an SMTP code (ie. the RFC 3464
    diagnostic-type is always smtp), and if empty, it means that there
    is no such code. The initial value is empty.
*/

void Recipient::setDiagnosticCode( const EString & code )
{
    d->diagnosticCode = code;
}


/*! Records the diagnostic code recorded by setDiagnosticCode(). */

EString Recipient::diagnosticCode() const
{
    return d->diagnosticCode;
}


/*! Records that the last delivery attempt for this recipient happened
    at \a date. The initial value, null, means that no deliveries have
    been attempted.
*/

void Recipient::setLastAttempt( class Date * date )
{
    d->lastAttemptDate = date;
}


/*! Returns the last attempt date for this recipient, or a null
    pointer if no deliveries have been attempted.
*/

Date * Recipient::lastAttempt() const
{
    return d->lastAttemptDate;
}


/*! Records that during the last delivery attempt, the remote server
    issued \a id as its final log ID. If \a id is empty, no ID was
    reported and none will be reported by Recipient.
*/

void Recipient::setFinalLogId( const EString & id )
{
    d->finalLogId = id;
}


/*! Returns whatever was set by setFinalLogId(), or an empty string if
    setFinalLogId() has not been called.

*/

EString Recipient::finalLogId() const
{
    return d->finalLogId;
}


/*! Returns a pararaph (as single line) describing the fate of this
    Recipient.
*/

EString Recipient::plainTextParagraph() const
{
    if ( !valid() )
        return "";

    EString s;
    EString a;

    if ( finalRecipient() && originalRecipient() &&
         finalRecipient()->toString(false) !=
         originalRecipient()->toString(false) ) {
        a.append( finalRecipient()->lpdomain() );
        a.append( " (forwarded from " );
        a.append( originalRecipient()->lpdomain() );
        a.append( ")" );
    }
    else if ( finalRecipient() ) {
        a.append( finalRecipient()->lpdomain() );
    }
    else if ( originalRecipient() ) {
        a.append( originalRecipient()->lpdomain() );
    }
    else {
        return "";
    }

    switch( action() ) {
    case Unknown:
        // we do not report on this recipient.
        return "";
        break;
    case Failed:
        s = "Your message could not be delivered to ";
        s.append( a );
        s.append( "." );
        if ( !status().isEmpty() &&
             !remoteMTA().isEmpty() ) {
            s.append( " " );
            if ( lastAttempt() ) {
                s.append( "At " );
                s.append( lastAttempt()->isoDate() );
                s.append( ", " );
                s.append( lastAttempt()->isoTime() );
                s.append( ", the " );
            }
            else {
                s.append( "The " );
            }
            s.append( "next-hop server (" );
            s.append( remoteMTA() );
            s.append( ") returned the following error code: " );
            s.append( status() );
            s.append( ". This is a fatal error. Sorry." );
        }
        break;
    case Delayed:
        s = "Delivery to ";
        s.append( a );
        s.append( " is unexpectedly delayed. Delivery attempts continue." );
        // here, we want to say "the next attempt is in 25 minutes" or
        // words to that effect. Maybe we need setNextAttempt()?
        break;
    case Delivered:
        s = "Your message was delivered to ";
        s.append( a );
        s.append( "." );
        break;
    case Relayed:
        s = "While delivering to ";
            s.append( a );
        s.append( ", your message was forwarded to " );
        if ( !remoteMTA().isEmpty() ) {
            s.append( remoteMTA() );
            s.append( "," );
        }
        else {
            s.append( "a host" );
        }
        s.append( " which cannot send reports such as this one."
                  " Unless you receive an error report, you can assume"
                  " that your message arrived safely." );
        break;
    case Expanded:
        s = "Your message was delivered to ";
        s.append( a );
        s.append( ", and resent to several other addresses from there." );
        break;
    }

    return s;
}


/*! Returns a paragraph containin the DSN for this Recipient. The
    returned string contains a series of LF-separated lines, but no
    trailing LF.
*/

EString Recipient::dsnParagraph() const
{
    if ( !valid() )
        return "";

    EStringList l;
    EString s;

    // [ original-recipient-field CRLF ]
    if ( originalRecipient() && originalRecipient() != finalRecipient() )
        l.append( "Original-Recipient: rfc822;" +
                  originalRecipient()->lpdomain() );

    // final-recipient-field CRLF
    if ( finalRecipient() )
        l.append( "Final-Recipient: rfc822;" +
                  finalRecipient()->lpdomain() );

    // action-field CRLF
    switch ( action() ) {
    case Unknown:
        l.append( "Action: unknown" );
        break;
    case Failed:
        l.append( "Action: failed" );
        break;
    case Delayed:
        l.append( "Action: delayed" );
        break;
    case Delivered:
        l.append( "Action: delivered" );
        break;
    case Relayed:
        l.append( "Action: relayed" );
        break;
    case Expanded:
        l.append( "Action: expanded" );
        break;
    }

    // status-field CRLF
    if ( !status().isEmpty() )
        l.append( "Status: " + status() );
    // [ remote-mta-field CRLF ]
    if ( !remoteMTA().isEmpty() )
        l.append( "Remote-Mta: dns;" + remoteMTA() );


    // [ diagnostic-code-field CRLF ]
    if ( !diagnosticCode().isEmpty() )
        l.append( "Diagnostic-Code: smtp;" + diagnosticCode() );

    // [ last-attempt-date-field CRLF ]
    if ( lastAttempt() )
        l.append( "Last-Attempt-Date: " + lastAttempt()->rfc822() );

    // [ final-log-id-field CRLF ]
    if ( !finalLogId().isEmpty() )
        l.append( "Final-Log-Id: smtp;" + finalLogId() );

    // we don't set will-retry-until. it only applies to delay dsns,
    // which we don't send.

    return l.join( "\n" );
}


/*! Sets this recipient's mailbox to \a m. */

void Recipient::setMailbox( Mailbox * m )
{
    d->mailbox = m;
}


/*! Returns a pointer to this Recipient's mailbox, or 0 if one hasn't
    been set with setMailbox().
*/

Mailbox * Recipient::mailbox() const
{
    return d->mailbox;
}


/*! Returns true if this Recipient has enough data to return a
    dsnParagraph() and a plainTextParagraph(), and false if not.
*/

bool Recipient::valid() const
{
    if ( action() == Unknown )
        return false;

    if ( status().isEmpty() )
        return false;

    if ( !finalRecipient() )
        return false;

    return true;
}


/*! This function is defined so that SMTP and the Injector may create a
    SortedList of Recipients. It compares this Recipient to \a b based
    on the mailbox() id. If either Recipient has no mailbox() defined,
    the results are meaningless.
*/

bool Recipient::operator <=( const Recipient &b )
{
    if ( mailbox() && b.mailbox() )
        return mailbox()->id() <= b.mailbox()->id();
    return false;
}
