// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include "copy.h"

#include "imapsession.h"
#include "transaction.h"
#include "messageset.h"
#include "occlient.h"
#include "mailbox.h"
#include "query.h"
#include "user.h"


class CopyData
    : public Garbage
{
public:
    CopyData() :
        uid( false ), firstUid( 0 ), modseq( 0 ),
        mailbox( 0 ), transaction( 0 ),
        findUid( 0 ),
        completedQueries( 0 )
    {}
    bool uid;
    MessageSet set;
    uint firstUid;
    int64 modseq;
    Mailbox * mailbox;
    Transaction * transaction;
    List<Query> queries;
    Query * findUid;
    uint completedQueries;
};


/*! \class Copy copy.h

    The Copy class implements the IMAP COPY command (RFC 3501 section
    6.4.7), as extended by RFC 2359.

    Copy copies all elements of a message, including such things as
    flags.
*/


/*! Constructs a Copy object parsing uids if \a uid is true, and msns
    if \a uid is false.
*/

Copy::Copy( bool uid )
    : Command(), d( new CopyData )
{
    d->uid = uid;
}


void Copy::parse()
{
    space();
    d->set = set( !d->uid );
    shrink( &d->set );
    space();
    d->mailbox = mailbox();
    end();
    requireRight( d->mailbox, Permissions::Insert );
    requireRight( d->mailbox, Permissions::Write );
    if ( ok() )
        log( "Will copy " + fn( d->set.count() ) +
             " messages to " + d->mailbox->name().ascii() );
}


void Copy::execute()
{
    if ( state() != Executing )
        return;

    if ( d->set.isEmpty() ) {
        finish();
        return;
    }

    if ( !permitted() )
        return;

    if ( !d->findUid ) {
        d->transaction = new Transaction( this );
        d->findUid = new Query( "select uidnext,nextmodseq from mailboxes "
                                "where id=$1 for update",
                                this );
        d->findUid->bind( 1, d->mailbox->id() );
        d->transaction->enqueue( d->findUid );
        d->transaction->execute();
    }
    if ( !d->findUid->done() )
        return;

    if ( !d->firstUid ) {
        Row * r = d->findUid->nextRow();
        if ( r ) {
            d->firstUid = r->getInt( "uidnext" );
            d->modseq = r->getBigint( "nextmodseq" );
        }
        else {
            error( No, "Could not allocate UID and modseq in target mailbox" );
        }

        if ( !ok() ) {
            d->transaction->rollback();
            return;
        }

        Mailbox * current = imap()->session()->mailbox();
        String where = "where m.mailbox=$3 and m.uid>=$4 and m.uid<$5";
        MessageSet set = d->set;
        if ( current->view() )
            where = "join view_messages vm on "
                    " (m.mailbox=vm.source and m.uid=vm.suid) "
                    "where vm.view=$3 and vm.uid>=$4 and vm.uid<$5";

        Query * q;

        uint cmailbox = current->id();
        uint tmailbox = d->mailbox->id();
        uint tuid = d->firstUid;
        uint i = 1;
        while ( i <= d->set.count() ) {
            uint cuid = set.value( i );
            uint j = i + 1;
            while ( j-i == set.value( j ) - cuid && j < i+1024 )
                j++;

            String diff = "m.uid+$2";
            uint delta = tuid-cuid;
            if ( tuid < cuid ) {
                diff = "m.uid-$2";
                delta = cuid-tuid;
            }

            q = new Query( "insert into messages "
                           "(mailbox, uid, idate, rfc822size) "
                           "select $1, " + diff +
                           ", m.idate, m.rfc822size from messages m " +
                           where,
                           this );
            q->bind( 1, tmailbox );
            q->bind( 2, delta );
            q->bind( 3, cmailbox );
            q->bind( 4, cuid );
            q->bind( 5, cuid + j - i );
            enqueue( q );

            q = new Query( "insert into part_numbers "
                           "(mailbox, uid, part, bodypart, bytes, lines) "
                           "select $1, " + diff +
                           ", m.part, m.bodypart, m.bytes, m.lines "
                           "from part_numbers m " + where,
                           this );
            q->bind( 1, tmailbox );
            q->bind( 2, delta );
            q->bind( 3, cmailbox );
            q->bind( 4, cuid );
            q->bind( 5, cuid + j - i );
            enqueue( q );

            q = new Query( "insert into header_fields "
                           "(mailbox, uid, part, position, field, value) "
                           "select $1, " + diff +
                           ", m.part, m.position, m.field, m.value "
                           "from header_fields m " + where,
                           this );
            q->bind( 1, tmailbox );
            q->bind( 2, delta );
            q->bind( 3, cmailbox );
            q->bind( 4, cuid );
            q->bind( 5, cuid + j - i );
            enqueue( q );

            q = new Query( "insert into address_fields "
                           "(mailbox, uid, part, position, field,"
                           " address, number) "
                           "select $1, " + diff + ", m.part, m.position, "
                           "m.field, m.address, m.number "
                           "from address_fields m " + where,
                           this );
            q->bind( 1, tmailbox );
            q->bind( 2, delta );
            q->bind( 3, cmailbox );
            q->bind( 4, cuid );
            q->bind( 5, cuid + j - i );
            enqueue( q );

            q = new Query( "insert into flags "
                           "(mailbox, uid, flag) "
                           "select $1, " + diff + ", m.flag "
                           "from flags m " + where,
                           this );
            q->bind( 1, tmailbox );
            q->bind( 2, delta );
            q->bind( 3, cmailbox );
            q->bind( 4, cuid );
            q->bind( 5, cuid + j - i );
            enqueue( q );

            q = new Query( "insert into annotations "
                           "(mailbox, uid, owner, name, value) "
                           "select $1, " + diff + ", $6, m.name, m.value "
                           "from annotations m " + where + " and "
                           "(owner is null or owner=$6)",
                           this );
            q->bind( 1, tmailbox );
            q->bind( 2, delta );
            q->bind( 3, cmailbox );
            q->bind( 4, cuid );
            q->bind( 5, cuid + j - i );
            q->bind( 6, imap()->user()->id() );
            enqueue( q );

            tuid = tuid + j - i;
            i = j;
        }

        // could this be done faster?
        q = new Query( "insert into modsequences (mailbox, uid, modseq) "
                       "select $1, uid, $2 from messages "
                       "where mailbox=$1 and uid>=$3 and uid<$4",
                       this );
        q->bind( 1, tmailbox );
        q->bind( 2, d->modseq );
        q->bind( 3, d->firstUid );
        q->bind( 4, tuid );
        enqueue( q );

        q = new Query( "update mailboxes set uidnext=$1, nextmodseq=$2 "
                       "where id=$3",
                       this );
        q->bind( 1, tuid );
        q->bind( 2, d->modseq+1 );
        q->bind( 2, tmailbox );
        d->transaction->enqueue( q );

        d->transaction->commit();
    }

    if ( d->set.count() > 256 ) {
        uint completed = 0;
        List<Query>::Iterator i( d->queries );
        while ( i ) {
            if ( i->state() == Query::Completed )
                completed++;
            ++i;
        }
        while ( d->completedQueries < completed ) {
            imap()->enqueue( "* OK [PROGRESS " +
                             tag() + " " +
                             fn( d->completedQueries ) + " " +
                             fn( d->queries.count() ) +
                             "] working\r\n" );
            d->completedQueries++;
        }
    }

    if ( !d->transaction->done() )
        return;

    if ( d->transaction->failed() ) {
        error( No, "Database failure: " + d->transaction->error() );
        return;
    }

    uint next = d->firstUid + d->set.count();
    if ( d->mailbox->uidnext() <= next ) {
        d->mailbox->setUidnextAndNextModSeq( next, d->modseq+1 );
        OCClient::send( "mailbox " + d->mailbox->name().utf8().quoted() + " "
                        "uidnext=" + fn( next ) + " "
                        "nextmodseq=" + fn( d->modseq+1 ) );
    }


    MessageSet target;
    target.add( d->firstUid, next - 1 );
    setRespTextCode( "COPYUID " +
                     fn( d->mailbox->uidvalidity() ) +
                     " " +
                     d->set.set() +
                     " " +
                     target.set() );
    finish();
}


/*! Wrapper for Transaction::enqueue() which also stores \a q in a
    list for later access.
*/

void Copy::enqueue( class Query * q )
{
    d->transaction->enqueue( q );
    d->queries.append( q );
}
