// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include "injector.h"

#include "map.h"
#include "dict.h"
#include "flag.h"
#include "query.h"
#include "address.h"
#include "message.h"
#include "ustring.h"
#include "mailbox.h"
#include "bodypart.h"
#include "datefield.h"
#include "fieldname.h"
#include "mimefields.h"
#include "messagecache.h"
#include "addressfield.h"
#include "transaction.h"
#include "annotation.h"
#include "allocator.h"
#include "session.h"
#include "scope.h"
#include "graph.h"
#include "html.h"
#include "md5.h"
#include "utf.h"
#include "log.h"
#include "dsn.h"


class MidFetcher;
class UidFetcher;
class BidFetcher;
class AddressCreator;


static PreparedStatement *lockUidnext;
static PreparedStatement *incrUidnext;
static PreparedStatement *incrUidnextWithRecent;
static PreparedStatement *idBodypart;
static PreparedStatement *intoBodyparts;

static GraphableCounter * successes;
static GraphableCounter * failures;


// This struct contains the id for a Bodypart, as well as the queries
// used to create and identify it.

struct Bid
    : public Garbage
{
    Bid( Bodypart * b )
        : bodypart( b ), insert( 0 ), select( 0 )
    {}

    Bodypart *bodypart;
    String hash;
    Query * insert;
    Query * select;
};


// These structs represent each entry in the header_fields and
// address_fields tables respectively.

struct FieldLink
    : public Garbage
{
    uint messageId;
    HeaderField *hf;
    String part;
    int position;
};

struct AddressLink
    : public Garbage
{
    uint messageId;
    Address * address;
    HeaderField::Type type;
    String part;
    int position;
    int number;
};


// The following is everything the Injector needs to do its work.

enum State {
    Inactive,
    CreatingFlags, CreatingAnnotationNames, CreatingFields,
    InsertingBodyparts, InsertingAddresses, SelectingUids,
    InsertingMessages,
    LinkingAddresses, LinkingFlags, LinkingAnnotations,
    AwaitingCompletion, Done
};

class InjectorData
    : public Garbage
{
public:
    InjectorData()
        : state( Inactive ), failed( false ),
          owner( 0 ), messages( 0 ), transaction( 0 ),
          midFetcher( 0 ), uidFetcher( 0 ), bidFetcher( 0 ),
          addressLinks( 0 ), fieldLinks( 0 ), dateLinks( 0 ),
          otherFields( 0 ), fieldCreation( 0 ), addressCreator( 0 ),
          flagCreation( 0 ), annotationCreation( 0 )
    {}

    State state;

    bool failed;

    EventHandler *owner;
    List<Message> * messages;

    Transaction *transaction;

    MidFetcher *midFetcher;
    UidFetcher *uidFetcher;
    BidFetcher *bidFetcher;

    List< AddressLink > * addressLinks;
    List< FieldLink > * fieldLinks;
    List< FieldLink > * dateLinks;
    StringList * otherFields;

    Query * fieldCreation;
    AddressCreator * addressCreator;
    Query * flagCreation;
    Query * annotationCreation;

    struct Delivery
        : public Garbage
    {
        Delivery( Message * m, Address * a, List<Address> * l )
            : message( m ), sender( a ), recipients( l )
        {}

        Message * message;
        Address * sender;
        List<Address> * recipients;
    };

    List<Delivery> deliveries;
};


class MidFetcher
    : public EventHandler
{
public:
    List<Message> * messages;
    List<Query> * queries;
    EventHandler *owner;
    List<Message>::Iterator * it;
    List<Query>::Iterator * qi;
    Query * insert;
    Query * select;
    bool failed;
    bool finished;
    String error;

    MidFetcher( List<Message> * ml, List<Query> * ql, EventHandler * ev )
        : messages( ml ), queries( ql ), owner( ev ),
          it( 0 ), qi( 0 ), insert( 0 ), select( 0 ),
          failed( false ), finished( false )
    {}

    void execute() {
        if ( finished )
            return;

        if ( !it ) {
            it = new List<Message>::Iterator( messages );
            qi = new List<Query>::Iterator( queries );
        }

        if ( !insert ) {
            insert = *qi;
            ++(*qi);
            select = *qi;
            ++(*qi);
        }

        if ( !insert->done() || !select->done() )
            return;

        Row * r = select->nextRow();
        if ( r ) {
            Message * m = *it;
            m->setDatabaseId( r->getInt( "id" ) );
        }
        else {
            failed = true;
            if ( insert->failed() )
                error = insert->error();
            else if ( select->failed() )
                error = select->error();
        }

        insert = 0;
        select = 0;
        ++(*it);

        if ( !*it ) {
            finished = true;
            owner->execute();
        }
    }

    bool done() const {
        return finished;
    }
};


class UidFetcher
    : public EventHandler
{
public:
    SortedList<Mailbox> * mailboxes;
    List<Query> * queries;
    List<Message> * messages;
    EventHandler *owner;
    bool failed;
    String error;

    UidFetcher( SortedList<Mailbox> * mbl, List<Query> *ql,
                List<Message> * ml, EventHandler * ev )
        : mailboxes( mbl ), queries( ql ), messages( ml ), owner( ev ),
          failed( false )
    {}

    void execute() {
        Query *q;

        while ( ( q = queries->firstElement() ) != 0 &&
                q->done() )
        {
            queries->shift();

            if ( q->hasResults() )
                process( q );
            else
                failed = true;
        }

        if ( failed || queries->isEmpty() )
            owner->execute();
    }

    void process( Query * q )
    {
        Mailbox * mb = mailboxes->shift();

        Row * r = q->nextRow();
        uint uidnext = r->getInt( "uidnext" );
        int64 nextms = r->getBigint( "nextmodseq" );

        if ( uidnext > 0x7ff00000 ) {
            Log::Severity level = Log::Error;
            if ( uidnext > 0x7fffff00 )
                level = Log::Disaster;
            log( "Note: Mailbox " + mb->name().ascii() +
                 " only has " + fn ( 0x7fffffff - uidnext ) +
                 " more usable UIDs. Please contact info@oryx.com"
                 " to resolve this problem.", level );
        }

        uint n = 0;
        List<Message>::Iterator it( messages );
        while ( it ) {
            Message * m = it;
            if ( m->inMailbox( mb ) ) {
                m->setUid( mb, uidnext+n );
                m->setModSeq( mb, nextms );
                n++;
            }
            ++it;
        }

        uint recentIn = 0;
        if ( r->getInt( "uidnext" ) == r->getInt( "first_recent" ) ) {
            List<Session>::Iterator si( mb->sessions() );
            if ( si ) {
                recentIn++;
                si->addRecent( uidnext, n );
            }
        }

        Query * u;
        if ( recentIn == 0 )
            u = new Query( *incrUidnext, 0 );
        else
            u = new Query( *incrUidnextWithRecent, 0 );
        u->bind( 1, mb->id() );
        u->bind( 2, n );
        q->transaction()->enqueue( u );
    }

    bool done() const {
        return queries->isEmpty();
    }
};


class BidFetcher
    : public EventHandler
{
public:
    Transaction * transaction;
    Query * look;
    List<Bid> * list;
    EventHandler * owner;
    List<Bid>::Iterator * li;
    uint state;
    uint savepoint;
    bool done;
    bool failed;
    String error;

    BidFetcher( Transaction * t, List<Bid> * l, EventHandler * ev )
        : transaction( t ), look( 0 ), list( l ), owner( ev ), li( 0 ),
          state( 0 ), savepoint( 0 ), done( false ), failed( false )
    {}

    void execute()
    {
        Query * q = 0;

        if ( !li )
            li = new List<Bid>::Iterator( list );

        if ( look ) {
            if ( look->state() == Query::Inactive ) {
                transaction->enqueue( look );
                transaction->execute();
                return;
            }
            if ( !look->done() )
                return;
            // get all the bodyparts rows
            Dict<Row> rows;
            Row * r = 0;
            while ( (r=look->nextRow()) != 0 )
                rows.insert( r->getString( "hash" ), r );
            // then tie each bodyparts row to all the Bodypart objects
            // that have the right hash.
            List<Bid>::Iterator bi( list );
            while ( bi ) {
                r = rows.find( bi->hash );
                if ( r )
                    bi->bodypart->setId( r->getInt( "id" ) );
                ++bi;
            }
        }

        while ( !done && *li ) {
            while ( *li && ( !(*li)->insert || (*li)->bodypart->id() ) )
                ++(*li);
            if ( !*li )
                break;

            struct Bid * b = *li;
            String s;

            switch ( state ) {
            case 0:
                s.append( "savepoint a" );
                s.append( fn( savepoint ) );
                q = new Query( s, this );
                transaction->enqueue( q );
                transaction->enqueue( b->insert );
                state = 1;
                transaction->execute();
                return;
                break;
            case 1:
                if ( !b->insert->done() )
                    return;
                if ( b->insert->failed() ) {
                    String e( b->insert->error() );
                    if ( !e.contains( "bodyparts_hash_key" ) ) {
                        error = e;
                        done = failed = true;
                        owner->execute();
                        return;
                    }
                    String s( "rollback to a" );
                    s.append( fn( savepoint ) );
                    q = new Query( s, this );
                    transaction->enqueue( q );
                }
                s = "release savepoint a";
                s.append( fn( savepoint ) );
                transaction->enqueue( new Query( s, 0 ) );
                transaction->enqueue( b->select );
                state = 2;
                transaction->execute();
                return;
                break;
            case 2:
                if ( !b->select->done() ) {
                    return;
                }
                else {
                    Row * r = b->select->nextRow();
                    if ( b->select->failed() || !r ) {
                        done = failed = true;
                        error = b->select->error();
                        if ( !r && error.isEmpty() )
                            error = "No matching bodypart found";
                        owner->execute();
                        return;
                    }
                    b->bodypart->setId( r->getInt( "id" ) );
                }
                ++(*li);
                state = 0;
                savepoint++;
                break;
            }
        }

        done = true;
        owner->execute();
    }
};


class AddressCreator
    : public EventHandler
{
public:
    int state;
    Query * q;
    Transaction * t;
    List<Address> * addresses;
    EventHandler * owner;
    Dict<Address> unided;
    int savepoint;
    bool failed;
    bool done;

    AddressCreator( Transaction * tr, List<Address> * a, EventHandler * ev )
        : state( 0 ), q( 0 ), t( tr ), addresses( a ), owner( ev ),
          savepoint( 0 ), failed( false ), done( false )
    {}

    void execute();
    void selectAddresses();
    void processAddresses();
    void insertAddresses();
    void processInsert();
};

void AddressCreator::execute()
{
    if ( state == 0 )
        selectAddresses();

    if ( state == 1 )
        processAddresses();

    if ( state == 2 )
        insertAddresses();

    if ( state == 3 )
        processInsert();

    if ( state == 4 ) {
        state = 42;
        done = true;
        owner->execute();
    }
}

static String addressKey( Address * a )
{
    String r;
    r.append( a->uname().utf8() );
    r.append( '\0' );
    r.append( a->localpart() );
    r.append( '\0' );
    r.append( a->domain().lower() );
    return r;
}

void AddressCreator::selectAddresses()
{
    q = new Query( "", this );

    String s( "select id, name, localpart, domain "
              "from addresses where " );

    unided.clear();

    uint i = 0;
    StringList sl;
    List<Address>::Iterator it( addresses );
    while ( it && i < 1024 ) {
        Address * a = it;
        if ( !a->id() ) {
            int n = 3*i+1;
            String p;
            unided.insert( addressKey( a ), a );
            q->bind( n, a->uname() );
            p.append( "(name=$" );
            p.append( fn( n++ ) );
            q->bind( n, a->localpart() );
            p.append( " and localpart=$" );
            p.append( fn( n++ ) );
            q->bind( n, a->domain().lower() );
            p.append( " and lower(domain)=$" );
            p.append( fn( n++ ) );
            p.append( ")" );
            sl.append( p );
            ++i;
        }
        ++it;
    }
    s.append( sl.join( " or " ) );
    q->setString( s );
    q->allowSlowness();

    if ( i == 0 ) {
        state = 4;
    }
    else {
        state = 1;
        t->enqueue( q );
        t->execute();
    }
}

void AddressCreator::processAddresses()
{
    while ( q->hasResults() ) {
        Row * r = q->nextRow();
        Address * a =
            new Address( r->getUString( "name" ),
                         r->getString( "localpart" ),
                         r->getString( "domain" ) );

        Address * orig =
            unided.take( addressKey( a ) );
        if ( orig )
            orig->setId( r->getInt( "id" ) );
    }

    if ( !q->done() )
        return;

    if ( unided.isEmpty() ) {
        state = 0;
        selectAddresses();
    }
    else {
        state = 2;
    }
}

void AddressCreator::insertAddresses()
{
    q = new Query( "savepoint b" + fn( savepoint ), this );
    t->enqueue( q );

    q = new Query( "copy addresses (name,localpart,domain) "
                   "from stdin with binary", this );
    StringList::Iterator it( unided.keys() );
    while ( it ) {
        Address * a = unided.take( *it );
        q->bind( 1, a->uname(), Query::Binary );
        q->bind( 2, a->localpart(), Query::Binary );
        q->bind( 3, a->domain(), Query::Binary );
        q->submitLine();
        ++it;
    }

    state = 3;
    t->enqueue( q );
    t->execute();
}

void AddressCreator::processInsert()
{
    if ( !q->done() )
        return;

    state = 0;
    if ( q->failed() ) {
        if ( q->error().contains( "addresses_nld_key" ) ) {
            q = new Query( "rollback to b" + fn( savepoint ), this );
            t->enqueue( q );
            savepoint++;
        }
        else {
            failed = true;
            state = 4;
        }
    }
    else {
        q = new Query( "release savepoint b" + fn( savepoint ), this );
        t->enqueue( q );
    }

    if ( state == 0 )
        selectAddresses();
}


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
    lockUidnext =
        new PreparedStatement(
            "select uidnext,nextmodseq,first_recent from mailboxes "
            "where id=$1 for update"
        );
    Allocator::addEternal( lockUidnext, "lockUidnext" );

    incrUidnext =
        new PreparedStatement(
            "update mailboxes "
            "set uidnext=uidnext+$2,nextmodseq=nextmodseq+1 "
            "where id=$1"
        );
    Allocator::addEternal( incrUidnext, "incrUidnext" );

    incrUidnextWithRecent =
        new PreparedStatement(
            "update mailboxes "
            "set uidnext=uidnext+$2,"
                 "nextmodseq=nextmodseq+1,"
                 "first_recent=first_recent+$2 "
            "where id=$1"
        );
    Allocator::addEternal( incrUidnextWithRecent, "incrUidnext w/recent" );

    idBodypart =
        new PreparedStatement(
            "select id from bodyparts where hash=$1"
        );
    Allocator::addEternal( idBodypart, "idBodypart" );

    intoBodyparts =
        new PreparedStatement(
            "insert into bodyparts (hash,bytes,text,data) "
            "values ($1,$2,$3,$4)"
        );
    Allocator::addEternal( intoBodyparts, "intoBodyparts" );
}


/*! Creates a new Injector to deliver the \a messages on behalf of
    the \a owner, which is notified when the injection is completed.
    Message delivery commences when the execute() function is called.
*/

Injector::Injector( List<Message> * messages, EventHandler * owner )
    : d( new InjectorData )
{
    if ( !lockUidnext )
        setup();

    d->owner = owner;
    d->messages = messages;
}


/*! \overload
    Creates a new Injector to deliver the \a message on behalf of the
    \a owner, which is notified when the injection is completed. This
    single-message variant is provided for convenience.
*/

Injector::Injector( Message * message, EventHandler * owner )
    : d( new InjectorData )
{
    if ( !lockUidnext )
        setup();

    d->owner = owner;
    d->messages = new List<Message>;
    d->messages->append( message );
}


/*! Notes that the given \a message must be delivered to the specified
    \a recipients from the given \a sender.
*/

void Injector::addDelivery( Message * message, Address * sender,
                            List<Address> * recipients )
{
    d->deliveries.append( new InjectorData::Delivery( message, sender,
                                                      recipients ) );
}


/*! \overload
    Notes that all messages must be delivered to the specified
    \a recipients from the given \a sender. This version is provided as
    a convenience to callers who only want to inject a single message
    and don't want to mix ordinary injections and deliveries.
*/

void Injector::addDelivery( Address * sender, List<Address> * recipients )
{
    List<Message>::Iterator it( d->messages );
    while ( it ) {
        addDelivery( it, sender, recipients );
        ++it;
    }
}


/*! Returns true if this injector has finished its work, and false if it
    hasn't started or is currently working.
*/

bool Injector::done() const
{
    return ( d->failed || d->state == Done );
}


/*! Returns true if this injection failed, and false if it has succeeded
    or is in progress.
*/

bool Injector::failed() const
{
    return d->failed;
}


/*! Returns an error message if injection failed, or an empty string
    if it succeeded or hasn't failed yet.
*/

String Injector::error() const
{
    if ( !d->failed )
        return "";

    List<Message>::Iterator it( d->messages );
    while ( it ) {
        Message * m = it;
        if ( !m->valid() )
            return m->error();
        ++it;
    }

    if ( d->bidFetcher->failed )
        return d->bidFetcher->error;
    if ( !d->transaction )
        return "";
    return d->transaction->error();
}


/*! This function creates and executes the series of database queries
    needed to perform message delivery.
*/

void Injector::execute()
{
    Scope x( log() );

    if ( d->state == Inactive ) {
        List<Message>::Iterator it( d->messages );
        while ( it ) {
            Message * m = it;
            if ( !m->valid() ) {
                d->failed = true;
                finish();
                return;
            }
            ++it;
        }

        logMessageDetails();

        d->transaction = new Transaction( this );
        d->state = CreatingFlags;
        createFlags();
    }

    if ( d->state == CreatingFlags ) {
        if ( d->flagCreation && !d->flagCreation->done() )
            return;

        if ( d->flagCreation && d->flagCreation->failed() ) {
            d->failed = true;
            d->transaction->rollback();
            d->state = AwaitingCompletion;
        }
        else {
            d->state = CreatingAnnotationNames;
            createAnnotationNames();
        }
    }

    if ( d->state == CreatingAnnotationNames ) {
        if ( d->annotationCreation && !d->annotationCreation->done() )
            return;

        if ( d->annotationCreation && d->annotationCreation->failed() ) {
            d->failed = true;
            d->transaction->rollback();
            d->state = AwaitingCompletion;
        }
        else {
            d->state = InsertingBodyparts;
            setupBodyparts();
            d->bidFetcher->execute();
        }
    }

    if ( d->state == InsertingBodyparts ) {
        if ( !d->bidFetcher->done )
            return;

        if ( d->bidFetcher->failed ) {
            d->failed = true;
            d->transaction->rollback();
            d->state = AwaitingCompletion;
        }
        else {
            selectMessageId();
            selectUids();
            d->transaction->execute();
            d->state = SelectingUids;
        }
    }

    if ( d->state == SelectingUids && !d->transaction->failed() ) {
        // Once we have UIDs for each Mailbox, we can insert rows into
        // messages.

        if ( !d->midFetcher->done() || !d->uidFetcher->done() )
            return;

        if ( d->midFetcher->failed || d->uidFetcher->failed ) {
            d->failed = true;
            d->transaction->rollback();
            d->state = AwaitingCompletion;
        }
        else {
            d->state = CreatingFields;
            buildFieldLinks();
            createFields();
        }
    }

    if ( d->state == CreatingFields ) {
        if ( d->fieldCreation && !d->fieldCreation->done() )
            return;

        if ( d->fieldCreation && d->fieldCreation->failed() ) {
            d->failed = true;
            d->transaction->rollback();
            d->state = AwaitingCompletion;
        }
        else {
            d->state = InsertingAddresses;
            resolveAddressLinks();
        }
    }

    if ( d->state == InsertingAddresses ) {
        if ( !d->addressCreator->done )
            return;

        if ( d->addressCreator->failed ) {
            d->failed = true;
            d->transaction->rollback();
            d->state = AwaitingCompletion;
        }
        else {
            d->state = InsertingMessages;
        }
    }

    if ( d->state == InsertingMessages && !d->transaction->failed() ) {
        insertMessages();
        linkBodyparts();
        linkHeaderFields();
        linkDates();
        insertDeliveries();
        linkAddresses();
        linkFlags();
        linkAnnotations();
        handleWrapping();

        d->state = LinkingAnnotations;
        d->transaction->execute();
    }

    if ( d->state == LinkingAnnotations || d->transaction->failed() ) {
        // Now we just wait for everything to finish.
        if ( d->state < AwaitingCompletion ) {
            d->transaction->enqueue(
                new Query( "notify mailboxes_updated", 0 ) );
            d->transaction->commit();
        }
        d->state = AwaitingCompletion;
    }

    if ( d->state == AwaitingCompletion ) {
        if ( !d->transaction->done() )
            return;
        if ( !::failures ) {
            ::failures = new GraphableCounter( "injection-errors" );
            ::successes = new GraphableCounter( "messages-injected" );
        }

        if ( !d->failed )
            d->failed = d->transaction->failed();

        if ( d->failed ) {
            ::failures->tick();
        }
        else {
            announce();
            ::successes->tick();
        }
        d->state = Done;
        finish();
    }
}


/*! This function notifies the owner of this Injector of its completion.
    It will do so only once.
*/

void Injector::finish()
{
    // XXX: If we fail early in the transaction, we'll continue to
    // be notified of individual query failures. We don't want to
    // pass them on, because d->owner would have killed itself.
    if ( !d->owner )
        return;

    if ( d->failed )
        log( "Injection failed: " + error() );
    else
        log( "Injection succeeded" );
    d->owner->execute();
    d->owner = 0;
}


/*! This private function inserts a new entry into the messages table
    and creates an MidFetcher to fetch the id of the new row.
*/

void Injector::selectMessageId()
{
    List<Query> * queries = new List<Query>;

    d->midFetcher =
        new MidFetcher( d->messages, queries, this );

    List<Message>::Iterator it( d->messages );
    while ( it ) {
        Message * m = it;

        Query * q =
            new Query( "insert into messages(id,rfc822size) "
                       "values (default,$1)", 0 );
        q->bind( 1, m->rfc822().length() );
        queries->append( q );
        d->transaction->enqueue( q );

        q = new Query( "select currval('messages_id_seq')::int "
                       "as id", d->midFetcher );
        queries->append( q );
        d->transaction->enqueue( q );

        ++it;
    }
}


/*! This private function is responsible for fetching a uid and modseq
    value for each message in each mailbox and incrementing uidnext and
    nextmodseq appropriately.
*/

void Injector::selectUids()
{
    // We are given a number of messages, each of which has its own list
    // of target mailboxes. There may be many messages, but chances are
    // that there are few mailboxes (the overwhelmingly common case is
    // just one mailbox).
    //
    // In principle, we could loop over d->messages/m->mailboxes() as we
    // do elsewhere, enqueue-ing a select/increment for each one. Things
    // would work so long as the increment for one message was executed
    // before the select for the next one. But we don't do that, because
    // then injecting ten thousand messages into one mailbox would need
    // ten thousand selects and, worse still, ten thousand updates too.
    //
    // So we turn the loop inside out, build a list of mailboxes, count
    // the messages to be injected into each one, and increment uidnext
    // and modseq by that number, once per mailbox instead of once per
    // message.
    //
    // To protect against concurrent injection into the same mailboxes,
    // we hold a write lock on the mailboxes during injection; thus, the
    // mailbox list must be sorted, so that the Injectors try to acquire
    // locks in the same order to avoid deadlock.

    Map<uint> uniq;
    SortedList<Mailbox> * mailboxes =
        new SortedList<Mailbox>;

    List<Message>::Iterator it( d->messages );
    while ( it ) {
        Message * m = it;
        List<Mailbox>::Iterator mi( m->mailboxes() );
        while ( mi ) {
            Mailbox * mb = mi;

            if ( !uniq.find( mb->id() ) ) {
                uniq.insert( mb->id(), (uint *)1 );
                mailboxes->insert( mb );
            }

            ++mi;
        }
        ++it;
    }

    List<Query> * queries = new List<Query>;
    d->uidFetcher =
        new UidFetcher( mailboxes, queries, d->messages, this );

    SortedList<Mailbox>::Iterator mi( mailboxes );
    while ( mi ) {
        Mailbox * mb = mi;

        Query * q = new Query( *lockUidnext, d->uidFetcher );
        q->bind( 1, mb->id() );
        d->transaction->enqueue( q );
        queries->append( q );

        ++mi;
    }
}


/*! This private function builds a list of AddressLinks containing every
    address used in the message, and initiates an AddressCache::lookup()
    after excluding any duplicate addresses. It causes execute() to be
    called when every address in d->addressLinks has been resolved (if
    any need resolving).
*/

void Injector::resolveAddressLinks()
{
    List< Address > * addresses = new List< Address >;
    Dict< Address > unique( 333 );
    Dict< Address > naked( 333 );

    List<AddressLink>::Iterator i( d->addressLinks );
    while ( i ) {
        String k = addressKey( i->address );

        if ( unique.contains( k ) ) {
            i->address = unique.find( k );
        }
        else {
            unique.insert( k, i->address );
            addresses->append( i->address );
            k = i->address->lpdomain();
            naked.insert( k, i->address );
        }

        ++i;
    }

    // if we're also going to insert deliveries rows, and one or more
    // of the addresses aren't in the to/cc fields, make sure we
    // create addresses rows and learn their ids.

    List<InjectorData::Delivery>::Iterator di( d->deliveries );
    while ( di ) {
        List<Address>::Iterator ai( di->recipients );
        while ( ai ) {
            Address * a = ai;
            String k( a->lpdomain() );

            if ( naked.contains( k ) ) {
                Address * same = naked.find( k );
                if ( a != same ) {
                    di->recipients->remove( a );
                    di->recipients->prepend( same );
                }
            }
            else {
                naked.insert( k, a );
                addresses->append( a );
            }
            ++ai;
        }

        String k( di->sender->lpdomain() );
        if ( naked.contains( k ) )
            di->sender = naked.find( k );
        else
            addresses->append( di->sender );

        ++di;
    }

    d->addressCreator =
        new AddressCreator( d->transaction, addresses, this );
    d->addressCreator->execute();
}


/*! This function creates a FieldCreator to create anything in
    d->otherFields that we do not already recognise.
*/

void Injector::createFields()
{
    StringList newFields;

    Dict<int> seen;
    StringList::Iterator it( d->otherFields );
    while ( it ) {
        String n( *it );
        if ( FieldName::id( n ) == 0 && !seen.contains( n ) ) {
            newFields.append( n );
            seen.insert( n, 0 );
        }
        ++it;
    }

    if ( !newFields.isEmpty() )
        d->fieldCreation =
            FieldName::create( newFields, d->transaction, this );
}


/*! This private function builds a list of FieldLinks containing every
    header field used in the message.
*/

void Injector::buildFieldLinks()
{
    d->fieldLinks = new List< FieldLink >;
    d->addressLinks = new List< AddressLink >;
    d->dateLinks = new List< FieldLink >;
    d->otherFields = new StringList;

    List<Message>::Iterator it( d->messages );
    while ( it ) {
        Message * m = it;

        buildLinksForHeader( m, m->header(), "" );

        // Since the MIME header fields belonging to the first-child of a
        // single-part Message are physically collocated with the RFC 822
        // header, we don't need to inject them into the database again.
        bool skip = false;
        ContentType *ct = m->header()->contentType();
        if ( !ct || ct->type() != "multipart" )
            skip = true;

        List<Bodypart>::Iterator bi( m->allBodyparts() );
        while ( bi ) {
            Bodypart *bp = bi;
            String pn = m->partNumber( bp );

            if ( !skip )
                buildLinksForHeader( m, bp->header(), pn );
            else
                skip = false;
            if ( bp->message() )
                buildLinksForHeader( m, bp->message()->header(),
                                     pn + ".rfc822" );
            ++bi;
        }

        ++it;
    }

}


/*! This private function makes links in d->fieldLinks for each of the
    fields in \a hdr (from the bodypart numbered \a part) of the given
    message \a m. It is used by buildFieldLinks().
*/

void Injector::buildLinksForHeader( Message * m, Header *hdr, const String &part )
{
    List< HeaderField >::Iterator it( hdr->fields() );
    while ( it ) {
        HeaderField *hf = it;

        FieldLink *link = new FieldLink;
        link->hf = hf;
        link->part = part;
        link->position = hf->position();
        link->messageId = m->databaseId();

        if ( hf->type() >= HeaderField::Other )
            d->otherFields->append( new String ( hf->name() ) );

        if ( hf->type() > HeaderField::LastAddressField )
            d->fieldLinks->append( link );

        if ( part.isEmpty() && hf->type() == HeaderField::Date )
            d->dateLinks->append( link );

        if ( hf->type() <= HeaderField::LastAddressField ) {
            List< Address > * al = ((AddressField *)hf)->addresses();
            List< Address >::Iterator ai( al );
            uint n = 0;
            while ( ai ) {
                AddressLink * link = new AddressLink;
                link->part = part;
                link->position = hf->position();
                link->type = hf->type();
                link->address = ai;
                link->number = n;
                link->messageId = m->databaseId();
                d->addressLinks->append( link );

                ++n;
                ++ai;
            }
        }

        ++it;
    }
}


/*! This private function looks through d->bodyparts, and fills in the
    INSERT needed to create, and the SELECT needed to identify, every
    storable bodypart in the message. The queries are executed by the
    BidFetcher.
*/

void Injector::setupBodyparts()
{
    List<Bid> * bodyparts = new List<Bid>;

    d->bidFetcher =
        new BidFetcher( d->transaction, bodyparts, this );

    List<Message>::Iterator it( d->messages );
    while ( it ) {
        Message * m = it;
        List<Bodypart>::Iterator bi( m->allBodyparts() );
        while ( bi ) {
            bodyparts->append( new Bid( bi ) );
            ++bi;
        }
        ++it;
    }

    StringList hashes;
    List< Bid >::Iterator bi( bodyparts );
    while ( bi ) {
        Bodypart *b = bi->bodypart;

        // These decisions should move into Bodypart member functions.

        bool storeText = false;
        bool storeData = false;

        ContentType *ct = b->contentType();
        if ( ct ) {
            if ( ct->type() == "text" ) {
                storeText = true;
                if ( ct->subtype() == "html" )
                    storeData = true;
            }
            else {
                storeData = true;
                if ( ct->type() == "multipart" && ct->subtype() != "signed" )
                    storeData = false;
                if ( ct->type() == "message" && ct->subtype() == "rfc822" )
                    storeData = false;
            }
        }
        else {
            storeText = true;
        }

        if ( storeText || storeData ) {
            PgUtf8Codec u;

            String data;
            if ( storeText )
                data = u.fromUnicode( b->text() );
            else if ( storeData )
                data = b->data();
            bi->hash = MD5::hash( data ).hex();

            Query * i = new Query( *intoBodyparts, d->bidFetcher );
            i->bind( 1, bi->hash );
            i->bind( 2, b->numBytes() );
            hashes.append( bi->hash );

            if ( storeText ) {
                String text( data );

                if ( storeData )
                    text = u.fromUnicode( HTML::asText( b->text() ) );

                i->bind( 3, text, Query::Binary );
            }
            else {
                i->bindNull( 3 );
            }

            if ( storeData )
                i->bind( 4, data, Query::Binary );
            else
                i->bindNull( 4 );

            i->allowFailure();

            bi->insert = i;
            bi->select = new Query( *idBodypart, d->bidFetcher );
            bi->select->bind( 1, bi->hash );
        }

        ++bi;
    }

    if ( hashes.isEmpty() )
        return;

    hashes.removeDuplicates();
    String r = "select id, hash from bodyparts where hash=$";
    uint n = 1;
    d->bidFetcher->look = new Query( "", d->bidFetcher );
    StringList::Iterator h( hashes );
    while ( h ) {
        if ( n > 1 )
            r.append( " or hash=$" );
        r.append( fn( n ) );
        d->bidFetcher->look->bind( n, *h );
        ++n;
        ++h;
    }
    d->bidFetcher->look->setString( r );
}


/*! This private function inserts one row per mailbox into the
    mailbox_messages table.
*/

void Injector::insertMessages()
{
    Query *qm =
        new Query( "copy mailbox_messages "
                   "(mailbox,uid,message,idate,modseq) "
                   "from stdin with binary", 0 );

    uint n = 0;
    List<Message>::Iterator it( d->messages );
    while ( it ) {
        Message * m = it;
        List<Mailbox>::Iterator mi( m->mailboxes() );
        while ( mi ) {
            n++;
            Mailbox *mb = mi;
            uint uid = m->uid( mb );
            int64 ms = m->modSeq( mb );

            qm->bind( 1, mb->id(), Query::Binary );
            qm->bind( 2, uid, Query::Binary );
            qm->bind( 3, m->databaseId(), Query::Binary );
            qm->bind( 4, internalDate( mb, m ), Query::Binary );
            qm->bind( 5, ms, Query::Binary );
            qm->submitLine();

            ++mi;
        }
        ++it;
    }

    if ( n )
        d->transaction->enqueue( qm );
}


/*! This private function inserts one row per remote recipient into
    the deliveries table.
*/

void Injector::insertDeliveries()
{
    if ( d->deliveries.isEmpty() )
        return;

    List<InjectorData::Delivery>::Iterator di( d->deliveries );
    while ( di ) {
        Query * q =
            new Query( "insert into deliveries "
                       "(sender,message,injected_at,expires_at) "
                       "values ($1,$2,current_timestamp,"
                       "current_timestamp+interval '2 days')", 0 );
        q->bind( 1, di->sender->id() );
        q->bind( 2, di->message->databaseId() );
        d->transaction->enqueue( q );

        uint n = 0;
        List<Address>::Iterator it( di->recipients );
        while ( it ) {
            Query * q =
                new Query(
                    "insert into delivery_recipients (delivery,recipient) "
                    "values ("
                    "currval(pg_get_serial_sequence('deliveries','id')),"
                    "$1)", 0
                );
            q->bind( 1, it->id() );
            d->transaction->enqueue( q );
            n++;
            ++it;
        }

        log( "Spooling message " + fn( di->message->databaseId() ) +
             " for delivery to " + fn( n ) +
             " remote recipients", Log::Significant );

        ++di;
    }

    d->transaction->enqueue( new Query( "notify deliveries_updated", 0 ) );
}


/*! This private function inserts rows into the part_numbers table for
    each new message.
*/

void Injector::linkBodyparts()
{
    Query *q =
        new Query( "copy part_numbers (message,part,bodypart,bytes,lines) "
                   "from stdin with binary", 0 );

    List<Message>::Iterator it( d->messages );
    while ( it ) {
        Message * m = it;
        uint mid = m->databaseId();

        // A fake target part for top-level RFC 822 header fields.
        insertPartNumber( q, mid, "" );

        List<Bodypart>::Iterator bi( m->allBodyparts() );
        while ( bi ) {
            Bodypart * b = bi;
            String pn( m->partNumber( b ) );

            insertPartNumber( q, mid, pn, b->id(),
                              b->numEncodedBytes(),
                              b->numEncodedLines() );

            if ( b->message() )
                insertPartNumber( q, mid, pn + ".rfc822", b->id(),
                                  b->numEncodedBytes(),
                                  b->numEncodedLines() );

            ++bi;
        }

        ++it;
    }

    d->transaction->enqueue( q );
}


/*! This private helper is used by linkBodyparts() to add a single row
    of data to \a q for \a message, \a part, and \a bodypart.
    If bodypart is smaller than 0, a NULL value is inserted instead.
    If \a bytes and \a lines are greater than or equal to 0, their
    values are inserted along with the \a bodypart.
*/

void Injector::insertPartNumber( Query *q, uint message,
                                 const String &part, int bodypart,
                                 int bytes, int lines )
{
    q->bind( 1, message, Query::Binary );
    q->bind( 2, part, Query::Binary );

    if ( bodypart > 0 )
        q->bind( 3, bodypart, Query::Binary );
    else
        q->bindNull( 3 );

    if ( bytes >= 0 )
        q->bind( 4, bytes, Query::Binary );
    else
        q->bindNull( 4 );

    if ( lines >= 0 )
        q->bind( 5, lines, Query::Binary );
    else
        q->bindNull( 5 );

    q->submitLine();
}


/*! This private function inserts entries into the header_fields table
    for each new message.
*/

void Injector::linkHeaderFields()
{
    Query *q =
        new Query( "copy header_fields "
                   "(message,part,position,field,value) "
                   "from stdin with binary", 0 );

    List< FieldLink >::Iterator it( d->fieldLinks );
    while ( it ) {
        FieldLink *link = it;

        uint t = FieldName::id( link->hf->name() );
        if ( !t )
            t = link->hf->type(); // XXX and what if this too fails?

        q->bind( 1, link->messageId, Query::Binary );
        q->bind( 2, link->part, Query::Binary );
        q->bind( 3, link->position, Query::Binary );
        q->bind( 4, t, Query::Binary );
        q->bind( 5, link->hf->value(), Query::Binary );
        q->submitLine();

        ++it;
    }

    d->transaction->enqueue( q );
}


/*! This private function inserts one entry per AddressLink into the
    address_fields table for each new message.
*/

void Injector::linkAddresses()
{
    Query * q =
        new Query( "copy address_fields "
                   "(message,part,position,field,number,address) "
                   "from stdin with binary", 0 );

    List< AddressLink >::Iterator it( d->addressLinks );
    while ( it ) {
        AddressLink *link = it;

        q->bind( 1, link->messageId, Query::Binary );
        q->bind( 2, link->part, Query::Binary );
        q->bind( 3, link->position, Query::Binary );
        q->bind( 4, link->type, Query::Binary );
        q->bind( 5, link->number, Query::Binary );
        q->bind( 6, link->address->id(), Query::Binary );
        q->submitLine();

        ++it;
    }

    d->transaction->enqueue( q );
}


/*! This private function inserts entries into the date_fields table
    for each new message.
*/

void Injector::linkDates()
{
    List< FieldLink >::Iterator it( d->dateLinks );
    while ( it ) {
        FieldLink * link = it;
        DateField * df = (DateField *)link->hf;

        Query * q =
           new Query( "insert into date_fields (message,value) "
                       "values ($1,$2)", 0 );
        q->bind( 1, link->messageId );
        q->bind( 2, df->date()->isoDateTime() );
        d->transaction->enqueue( q );

        ++it;
    }
}


/*! Logs a little information about the messages to be injected, and a
    little more for the special case of a single message being injected
    into a single mailbox.
*/

void Injector::logMessageDetails()
{
    if ( d->messages->count() > 1 ) {
        log( "Injecting " + fn( d->messages->count() ) + " "
             "messages", Log::Significant );
    }
    else {
        Message * m = d->messages->first();

        String msg( "Injecting message " );

        String id;
        Header * h = m->header();
        if ( h )
            id = h->messageId();
        if ( id.isEmpty() )
            id = "<>";
        msg.append( id );

        String dest( " into " );
        List<Mailbox> * mailboxes = m->mailboxes();
        Mailbox * mb = mailboxes->first();
        if ( mb ) {
            dest.append( mb->name().ascii() );
        }
        if ( mailboxes->count() > 1 ) {
            dest.append( " (and " );
            dest.append( fn( mailboxes->count()-1 ) );
            dest.append( " other mailboxes)" );
        }
        if ( mailboxes->count() > 0 )
            msg.append( dest );

        log( msg, Log::Significant );
    }
}


/*! This function announces the injection of a message into the relevant
    mailboxes, using ocd. It should be called only when the Injector has
    completed successfully (done(), but not failed()).

    The Mailbox objects in this process are notified immediately, to
    avoid timing-dependent behaviour within one process.
*/

void Injector::announce()
{
    List<Message>::Iterator it( d->messages );
    while ( it ) {
        Message * m = it;
        List<Mailbox>::Iterator mi( m->mailboxes() );
        while ( mi ) {
            Mailbox * mb = mi;
            uint uid = m->uid( mb );
            int64 ms = m->modSeq( mb );

            List<Session>::Iterator si( mb->sessions() );
            if ( si )
                MessageCache::insert( mb, uid, m );

            while ( si ) {
                MessageSet dummy;
                dummy.add( uid );
                si->addUnannounced( dummy );
                ++si;
            }

            if ( mb->uidnext() <= uid || mb->nextModSeq() <= ms )
                mb->setUidnextAndNextModSeq( 1+uid, 1+ms );

            ++mi;
        }
        ++it;
    }
}


/*! Starts creating Flag objects for the flags we need to store for
    this message.
*/

void Injector::createFlags()
{
    StringList unknown;

    List<Message>::Iterator it( d->messages );
    while ( it ) {
        Message * m = it;
        List<Mailbox>::Iterator mi( m->mailboxes() );
        while ( mi ) {
            Mailbox * mb = mi;
            StringList::Iterator i( m->flags( mb ) );
            while ( i ) {
                if ( Flag::id( *i ) == 0 )
                    unknown.append( *i );
                ++i;
            }
            ++mi;
        }
        ++it;
    }

    if ( !unknown.isEmpty() )
        d->flagCreation = Flag::create( unknown, d->transaction, this );
}


/*! Creates the AnnotationName objects needed to create the annotation
    entries specified with the message.
*/

void Injector::createAnnotationNames()
{
    Dict<int> seen;
    StringList unknown;

    List<Message>::Iterator it( d->messages );
    while ( it ) {
        Message * m = it;
        List<Mailbox>::Iterator mi( m->mailboxes() );
        while ( mi ) {
            Mailbox * mb = mi;
            List<Annotation>::Iterator ai( m->annotations( mb ) );
            while ( ai ) {
                Annotation * a = ai;
                String n( a->entryName() );

                if ( AnnotationName::id( n ) == 0 && !seen.contains( n ) ) {
                    unknown.append( n );
                    seen.insert( n, 0 );
                }

                ++ai;
            }
            ++mi;
        }
        ++it;
    }

    if ( !unknown.isEmpty() )
        d->annotationCreation =
            AnnotationName::create( unknown, d->transaction, this );
}


/*! Inserts the flag table entries linking flag_names to the
    mailboxes/uids we occupy.
*/

void Injector::linkFlags()
{
    Query * q =
        new Query( "copy flags (mailbox,uid,flag) "
                   "from stdin with binary", this );

    uint flags = 0;
    List<Message>::Iterator it( d->messages );
    while ( it ) {
        Message * m = it;
        List<Mailbox>::Iterator mi( m->mailboxes() );
        while ( mi ) {
            Mailbox * mb = mi;
            StringList::Iterator i( m->flags( mb ) );
            while ( i ) {
                flags++;
                q->bind( 1, mb->id(), Query::Binary );
                q->bind( 2, m->uid( mb ), Query::Binary );
                q->bind( 3, Flag::id( *i ), Query::Binary );
                q->submitLine();
                ++i;
            }
            ++mi;
        }
        ++it;
    }

    if ( flags )
        d->transaction->enqueue( q );
}


/*! Inserts the appropriate entries into the annotations table. */

void Injector::linkAnnotations()
{
    Query * q =
        new Query( "copy annotations (mailbox,uid,name,value,owner) "
                   "from stdin with binary", this );

    uint annotations = 0;
    List<Message>::Iterator it( d->messages );
    while ( it ) {
        Message * m = it;
        List<Mailbox>::Iterator mi( m->mailboxes() );
        while ( mi ) {
            Mailbox * mb = mi;
            List<Annotation>::Iterator ai( m->annotations( mb ) );
            while ( ai ) {
                annotations++;

                uint aid( AnnotationName::id( ai->entryName() ) );

                q->bind( 1, mb->id(), Query::Binary );
                q->bind( 2, m->uid( mb ), Query::Binary );
                q->bind( 3, aid, Query::Binary );
                q->bind( 4, ai->value(), Query::Binary );
                if ( ai->ownerId() == 0 )
                    q->bindNull( 5 );
                else
                    q->bind( 5, ai->ownerId(), Query::Binary );
                ++ai;
            }
            ++mi;
        }
        ++it;
    }

    if ( annotations )
        d->transaction->enqueue( q );
}


/*! If any of the messages are wrapped, this function inserts rows into
    the unparsed_messages table for them.
*/

void Injector::handleWrapping()
{
    Query * q =
        new Query( "copy unparsed_messages (bodypart) "
                   "from stdin with binary", this );

    uint wrapped = 0;
    List<Message>::Iterator it( d->messages );
    while ( it ) {
        Message * m = it;
        if ( m->isWrapped() ) {
            wrapped++;

            List<Bodypart>::Iterator bi( m->allBodyparts() );
            while ( bi ) {
                Bodypart * b = bi;
                if ( m->partNumber( b ) == "2" ) {
                    q->bind( 1, b->id() );
                    q->submitLine();
                }
                ++bi;
            }
        }
        ++it;
    }

    if ( wrapped )
        d->transaction->enqueue( q );
}


/*! Returns a sensible internaldate for \a m in \a mb. If
    Message::internalDate() is not null, it is used, otherwise this
    function tries to obtain a date heuristically.
*/

uint Injector::internalDate( Mailbox * mb, Message * m ) const
{
    if ( !m || !mb )
        return 0;
    if ( m->internalDate( mb ) )
        return m->internalDate( mb );

    // first: try the most recent received field. this should be
    // very close to the correct internaldate.
    Date id;
    List< HeaderField >::Iterator it( m->header()->fields() );
    while ( it && !id.valid() ) {
        if ( it->type() == HeaderField::Received ) {
            String v = it->rfc822();
            int i = 0;
            while ( v.find( ';', i+1 ) > 0 )
                i = v.find( ';', i+1 );
            if ( i >= 0 )
                id.setRfc822( v.mid( i+1 ) );
        }
        ++it;
    }

    // if that fails, try the message's date.
    if ( !id.valid() ) {
        Date * date = m->header()->date();
        if ( date )
            id.setUnixTime( date->unixTime() ); // ick
    }

    // and if all else fails, now.
    if ( !id.valid() )
        id.setCurrentTime();

    m->setInternalDate( mb, id.unixTime() );
    return id.unixTime();
}
