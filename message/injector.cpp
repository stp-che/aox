#include "injector.h"

#include "arena.h"
#include "scope.h"
#include "dict.h"
#include "query.h"
#include "address.h"
#include "message.h"
#include "mailbox.h"
#include "fieldcache.h"
#include "addresscache.h"
#include "transaction.h"
#include "md5.h"
#include "utf.h"


static Dict< int > * bodyHashes;


// These structs represent one part of each entry in the header_fields
// and address_fields tables. (The other part being mailbox and UID.)

struct FieldLink {
    HeaderField *hf;
    String part;
};

struct AddressLink {
    Address * address;
    HeaderField::Type type;
};


class InjectorData {
public:
    InjectorData()
        : step( 0 ), failed( false ),
          owner( 0 ), message( 0 ), mailboxes( 0 ), transaction( 0 ),
          totalUids( 0 ), uids( 0 ), totalBodyparts( 0 ), bodypartIds( 0 ),
          bodyparts( 0 ), addressLinks( 0 ), fieldLinks( 0 ), otherFields( 0 ),
          fieldLookup( 0 ), addressLookup( 0 )
    {}

    int step;
    bool failed;

    EventHandler * owner;
    const Message * message;
    SortedList< Mailbox > * mailboxes;

    Transaction * transaction;

    uint totalUids;
    List< int > * uids;
    uint totalBodyparts;
    List< int > * bodypartIds;
    List< BodyPart > * bodyparts;
    List< AddressLink > * addressLinks;
    List< FieldLink > * fieldLinks;
    List< String > * otherFields;

    CacheLookup * fieldLookup;
    CacheLookup * addressLookup;
};


class IdHelper : public EventHandler {
protected:
    List< int > * list;
    List< Query > * queries;
    EventHandler * owner;

public:
    IdHelper( List< int > *l, List< Query > *q, EventHandler *ev )
        : list( l ), queries( q ), owner( ev )
    {}

    virtual void processResults( Query *q ) {
        list->append( new int( q->nextRow()->getInt( 0 ) ) );
    }

    void execute() {
        Query *q = queries->first();
        if ( !q->done() )
            return;

        if ( q->hasResults() )
            processResults( q );

        queries->take( queries->first() );
        if ( queries->isEmpty() )
            owner->notify();
    }
};


/*! \class Injector injector.h
    This class delivers a Message to a List of Mailboxes.

    The Injector takes a Message object, and performs all the database
    operations necessary to inject it into each of a List of Mailboxes.
    The message is assumed to be valid. The list of mailboxes must be
    sorted.
*/

/*! This setup function expects to be called by ::main() to perform what
    little initialisation is required by the Injector.
*/

void Injector::setup()
{
    bodyHashes = new Dict< int >;
}


/*! Creates a new Injector object to deliver the \a message into each of
    the \a mailboxes on behalf of the \a owner, which is notified when
    the delivery attempt is completed. Message delivery commences when
    the execute() function is called.

    The caller must not change \a mailboxes after this call.
*/

Injector::Injector( const Message * message,
                    SortedList< Mailbox > * mailboxes,
                    EventHandler * owner )
    : d( new InjectorData )
{
    d->owner = owner;
    d->message = message;
    d->mailboxes = mailboxes;
}


/*! Cleans up after injection. (We're already pretty clean.) */

Injector::~Injector()
{
}


/*! Returns true if this injector has finished its work, and false if it
    hasn't started or is currently working.
*/

bool Injector::done() const
{
    return d->step >= 4;
}


/*! Returns true if this injection failed, and false if it has succeeded
    or is in progress.
*/

bool Injector::failed() const
{
    return d->failed;
}


/*! This function creates and executes the series of database queries
    needed to perform message delivery.
*/

void Injector::execute()
{
    if ( d->step == 0 ) {
        // We begin by obtaining a UID for each mailbox we are injecting
        // a message into, and simultaneously inserting entries into the
        // bodyparts table. At the same time, we can begin to lookup and
        // insert the addresses and field names used in the message.

        d->transaction = new Transaction( this );

        selectUids();
        insertBodyparts();
        buildAddressLinks();
        buildFieldLinks();

        d->transaction->execute();
        d->step = 1;
    }

    if ( d->step == 1 ) {
        // Once we have UIDs for each Mailbox, we can insert rows into
        // messages and recent_messages.

        if ( d->uids->count() != d->totalUids )
            return;

        insertMessages();

        d->transaction->execute();
        d->step = 2;
    }

    if ( d->step == 2 ) {
        // We expect buildFieldLinks() to have completed immediately.
        // Once insertBodyparts() is completed, we can start adding to
        // the header_fields and part_numbers tables.

        if ( !d->fieldLookup->done() ||
             d->bodypartIds->count() != d->totalBodyparts )
            return;

        linkHeaderFields();
        linkBodyparts();

        d->transaction->execute();
        d->step = 3;
    }

    if ( d->step == 3 ) {
        // Fill in address_fields once the address lookup is complete.
        // (We could have done this without waiting for the bodyparts
        // to be inserted, but it didn't seem worthwhile.)

        if ( !d->addressLookup->done() )
            return;

        linkAddresses();

        // Now we just wait for everything to finish.

        d->transaction->commit();
        d->step = 4;
    }

    if ( d->step == 4 ) {
        if ( !d->transaction->done() )
            return;
        d->failed = d->transaction->failed();
        d->owner->notify();
    }
}


/*! This private function issues queries to retrieve a UID for each of
    the Mailboxes we are delivering the message into, adds each UID to
    d->uids, and informs execute() when it's done.
*/

void Injector::selectUids()
{
    Query *q;
    d->uids = new List< int >;
    List< Query > * queries = new List< Query >;
    IdHelper * helper = new IdHelper( d->uids, queries, this );

    List< Mailbox >::Iterator it( d->mailboxes->first() );
    while ( it ) {
        d->totalUids++;

        // We acquire a write lock on our mailbox, and hold it until the
        // entire transaction has committed successfully. We use uidnext
        // in lieu of a UID sequence to serialise Injectors, so that UID
        // announcements are correctly ordered.
        //
        // The mailbox list must be sorted, so that Injectors always try
        // to acquire locks in the same order, thus avoiding deadlocks.

        q = new Query( "select uidnext from mailboxes "
                       "where id=$1 for update", helper );
        q->bind( 1, it->id() );
        d->transaction->enqueue( q );
        queries->append( q );

        q = new Query( "update mailboxes set uidnext=uidnext+1 where id=$1",
                       helper );
        q->bind( 1, it->id() );
        d->transaction->enqueue( q );
        queries->append( q );

        it++;
    }
}


/*! This private function builds a list of AddressLinks containing every
    address used in the message, and initiates an AddressCache::lookup()
    after excluding any duplicate addresses. It causes execute() to be
    called when every address in d->addressLinks has been resolved.
*/

void Injector::buildAddressLinks()
{
    d->addressLinks = new List< AddressLink >;
    List< Address > * addresses = new List< Address >;
    Dict< Address > unique;

    HeaderField::Type types[] = {
        HeaderField::ReturnPath, HeaderField::Sender, HeaderField::ResentSender,
        HeaderField::From, HeaderField::To, HeaderField::Cc, HeaderField::Bcc,
        HeaderField::ResentFrom, HeaderField::ResentTo, HeaderField::ResentCc,
        HeaderField::ResentBcc, HeaderField::ReplyTo
    };
    int n = sizeof (types) / sizeof( types[0] );

    int i = 0;
    while ( i < n ) {
        HeaderField::Type t = types[ i++ ];
        List< Address > * a = d->message->header()->addresses( t );
        if ( a && !a->isEmpty() ) {
            List< Address >::Iterator it( a->first() );
            while ( it ) {
                Address *a = it++;
                String k = a->toString();

                if ( unique.contains( k ) ) {
                    a = unique.find( k );
                }
                else {
                    unique.insert( k, a );
                    addresses->append( a );
                }

                AddressLink *link = new AddressLink;
                d->addressLinks->append( link );
                link->address = a;
                link->type = t;
            }
        }
    }

    d->addressLookup = AddressCache::lookup( addresses, this );
}


/*! This private function builds a list of FieldLinks containing every
    header field used in the message, and uses
    FieldNameCache::lookup() to associate each unknown HeaderField
    with an ID. It causes execute() to be called when every field name
    in d->fieldLinks has been resolved.
*/

void Injector::buildFieldLinks()
{
    d->fieldLinks = new List< FieldLink >;
    d->otherFields = new List< String >;

    buildLinksForHeader( d->message->header(), "" );

    List< BodyPart >::Iterator it( d->message->bodyParts()->first() );
    while ( it ) {
        buildLinksForHeader( it->header(),
                             d->message->partNumber( it ) );
        it++;
    }

    d->fieldLookup = FieldNameCache::lookup( d->otherFields, this );
}


/*! This private function makes links in d->fieldLinks for each of the
    fields in \a hdr (from the bodypart numbered \a part). It is used
    by buildFieldLinks().
*/

void Injector::buildLinksForHeader( Header *hdr, const String &part )
{
    List< HeaderField >::Iterator it( hdr->fields()->first() );
    while ( it ) {
        HeaderField *hf = it++;

        FieldLink *link = new FieldLink;
        link->hf = hf;
        link->part = part;

        if ( hf->type() == HeaderField::Other )
            d->otherFields->append( new String ( hf->name() ) );

        d->fieldLinks->append( link );
    }
}


/*! This private function inserts an entry into bodyparts for every MIME
    bodypart in the message. The IDs are then stored in d->bodypartIds.
*/

void Injector::insertBodyparts()
{
    class BodypartHelper : public IdHelper {
    public:
        BodypartHelper( List< int > *l, List< Query > *q, EventHandler *ev )
            : IdHelper( l, q, ev )
        {}

        void processResults( Query *q ) {
            Row *r = q->nextRow();
            int *id = new int( r->getInt( "id" ) );
            String hash = r->getString( "hash" );

            list->append( id );
            bodyHashes->insert( hash, id );
        }
    };

    Query *i, *s;
    d->bodypartIds = new List< int >;
    List< Query > * queries = new List< Query >;
    IdHelper * helper = new BodypartHelper( d->bodypartIds, queries, this );

    d->bodyparts = d->message->bodyParts();
    List< BodyPart >::Iterator it( d->bodyparts->first() );
    while ( it ) {
        d->totalBodyparts++;
        BodyPart *b = it++;

        // If we've seen the text of this body part before, we already
        // know its bodypart id. If not, we need to insert a new entry
        // into bodyparts.

        String hash = MD5::hash( b->data() ).hex();
        int *id = bodyHashes->find( hash );
        if ( id ) {
            // We'll construct a fake result for this query, and pretend
            // that it completed successfully, giving us an id and hash.
            //
            // s = new Query( "select " + fn( *id ) + ", "
            //                "'" + hash.hex() + "' as hash", helper );
            // d->transaction->enqueue( s );

            Row::Column *c = new Row::Column[2];
            c[0].name = "id";
            c[0].type = Database::Integer;
            c[0].length = 4;
            c[0].value.append( (char)((*id >> 24) & 0xff) );
            c[0].value.append( (char)((*id >> 16) & 0xff) );
            c[0].value.append( (char)((*id >> 8) & 0xff) );
            c[0].value.append( (char)(*id & 0xff) );
            c[1].name = "hash";
            c[1].type = Database::Bytes;
            c[1].value = hash;
            c[1].length = 32;

            s = new Query( "select id,hash from cache", helper );
            s->addRow( new Row( 2, c ) );
            s->setState( Query::Completed );
            queries->append( s );
            continue;
        }

        bool text = true;
        if ( b->contentType() &&
             b->contentType()->type() != "text" )
            text = false;

        i = new Query( "insert into bodyparts (text,bytes,lines) "
                       "values ($1,$2,$3)", helper );
        if ( text ) {
            Codec *c = new Utf8Codec;
            i->bind( 1, c->fromUnicode( b->text() ), Query::Binary );
        }
        else {
            i->bindNull( 1 );
        }
        i->bind( 2, b->numBytes() );
        i->bind( 3, b->numLines() );
        d->transaction->enqueue( i );

        if ( !text ) {
            Query *i;

            i = new Query( "insert into binary_parts (id, data) "
                           "values (select currval('bodypart_ids'),$1)",
                           helper );
            i->bind( 1, b->data(), Query::Binary );
            d->transaction->enqueue( i );
        }

        s = new Query( "select currval('bodypart_ids')::integer as id, "
                       "'" + hash + "' as hash", helper );
        d->transaction->enqueue( s );
        queries->append( s );
    }
}


/*! This private function inserts one row per mailbox into the messages
    table.
*/

void Injector::insertMessages()
{
    Query *q;

    List< int >::Iterator uids( d->uids->first() );
    List< Mailbox >::Iterator mb( d->mailboxes->first() );
    while ( uids ) {
        Mailbox *m = mb++;
        int uid = *uids++;

        q = new Query( "insert into messages (mailbox,uid) values "
                       "($1,$2)", 0 );
        q->bind( 1, m->id() );
        q->bind( 2, uid );
        d->transaction->enqueue( q );

        q = new Query( "insert into recent_messages (mailbox,uid) "
                        "values ($1,$2)", 0 );
        q->bind( 1, m->id() );
        q->bind( 2, uid );
        d->transaction->enqueue( q );
    }
}


/*! This private function inserts entries into the header_fields table
    for each new message.
*/

void Injector::linkHeaderFields()
{
    Query *q;

    List< int >::Iterator uids( d->uids->first() );
    List< Mailbox >::Iterator mb( d->mailboxes->first() );
    while ( uids ) {
        Mailbox *m = mb++;
        int uid = *uids++;

        List< FieldLink >::Iterator it( d->fieldLinks->first() );
        while ( it ) {
            FieldLink *link = it++;

            HeaderField::Type t = link->hf->type();
            if ( t == HeaderField::Other )
                t = FieldNameCache::translate( link->hf->name() );

            q = new Query( "insert into header_fields "
                           "(mailbox,uid,part,field,value) values "
                           "($1,$2,$3,$4,$5)", 0 );
            q->bind( 1, m->id() );
            q->bind( 2, uid );
            q->bind( 3, link->part );
            q->bind( 4, t );
            q->bind( 5, link->hf->value() );

            d->transaction->enqueue( q );
        }
    }
}


/*! This private function inserts rows into the part_numbers table for
    each new message.
*/

void Injector::linkBodyparts()
{
    Query *q;

    List< int >::Iterator uids( d->uids->first() );
    List< Mailbox >::Iterator mb( d->mailboxes->first() );
    while ( uids ) {
        Mailbox *m = mb++;
        int uid = *uids++;

        List< int >::Iterator bids( d->bodypartIds->first() );
        List< BodyPart >::Iterator it( d->bodyparts->first() );
        while ( it ) {
            int bid = *bids++;
            BodyPart *b = it++;

            q = new Query( "insert into part_numbers "
                           "(mailbox,uid,bodypart,partno) values "
                           "($1,$2,$3,$4)", 0 );
            q->bind( 1, m->id() );
            q->bind( 2, uid );
            q->bind( 3, bid );
            q->bind( 4, d->message->partNumber( b ) );

            d->transaction->enqueue( q );
        }
    }
}


/*! This private function inserts one entry per AddressLink into the
    address_fields table for each new message.
*/

void Injector::linkAddresses()
{
    Query *q;

    List< int >::Iterator uids( d->uids->first() );
    List< Mailbox >::Iterator mb( d->mailboxes->first() );
    while ( uids ) {
        Mailbox *m = mb++;
        int uid = *uids++;

        List< AddressLink >::Iterator it( d->addressLinks->first() );
        while ( it ) {
            AddressLink *link = it++;

            q = new Query( "insert into address_fields "
                           "(mailbox,uid,field,address) values "
                           "($1,$2,$3,$4)", 0 );
            q->bind( 1, m->id() );
            q->bind( 2, uid );
            q->bind( 3, link->type );
            q->bind( 4, link->address->id() );

            d->transaction->enqueue( q );
        }
    }
}
