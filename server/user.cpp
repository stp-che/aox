// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include "user.h"

#include "address.h"
#include "mailbox.h"
#include "query.h"
#include "configuration.h"
#include "transaction.h"
#include "addresscache.h"


class UserData
{
public:
    UserData(): id( 0 ), inbox( 0 ), home( 0 ), address( 0 ),
                q( 0 ), createQuery( 0 ), t( 0 ), user( 0 ),
                state( User::Unverified ),
                mode( LoungingAround )
        {}
    String login;
    String secret;
    uint id;
    Mailbox * inbox;
    Mailbox * home;
    Address * address;
    Query * q;
    Query *createQuery;
    Transaction * t;
    EventHandler * user;
    String error;
    User::State state;

    enum Operation {
        LoungingAround,
        Creating,
        Renaming,
        Refreshing,
        Removing
    };
    Operation mode;
};


/*! \class User user.h

    The User class models a single Mailstore user, which may be able
    to log in, own Mailbox objects, etc.
*/


/*! Constructs an empty User. The result does not map to anything in
    the database.
*/

User::User()
    : d( new UserData )
{
    // nothing
}


/*! Sets this User object to have login \a string. The database is not
    updated - \a string is not used except to create Query objects
    during e.g. refresh().
*/

void User::setLogin( const String & string )
{
    d->login = string;
}


/*! Returns the User's login string, which is an empty string
    initially and is set up by refresh().
*/

String User::login() const
{
    return d->login;
}


/*! Sets this User to have \a secret as password. The database isn't
    updated unless e.g. create() is called.
*/

void User::setSecret( const String & secret )
{
    d->secret = secret;
}


/*! Returns the User's secret (password), which is an empty string
    until refresh() has fetched the database contents.
*/

String User::secret() const
{
    return d->secret;
}


/*! Returns a pointer to the user's inbox, or a null pointer if this
    object doesn't know it or if the user has none.
*/

Mailbox * User::inbox() const
{
    return d->inbox;
}


/*! Sets this User object to have address \a a. The database is not
    updated - \a a is not used except maybe to search in refresh().
*/

void User::setAddress( Address * a )
{
    d->address = a;
}


/*! Returns the address belonging to this User object, or a null
    pointer if this User has no Address.
*/

Address * User::address()
{
    if ( !d->address ) {
        String dom = Configuration::hostname();
        uint i = dom.find( '.' );
        if ( i > 0 )
            dom = dom.mid( i+1 );
        d->address = new Address( "", d->login, dom );
    }
    return d->address;
}


static PreparedStatement * psl;
static PreparedStatement * psa;


/*! Starts refreshing this object from the database, and remembers to
    call \a user when the refresh is complete.
*/

void User::refresh( EventHandler * user )
{
    if ( d->q )
        return;
    d->user = user;
    if ( !psl ) {
        psl = new PreparedStatement(
            "select u.id, u.address, u.inbox, n.name as parentspace, "
            "u.login, u.id, u.secret, a.name, a.localpart, a.domain "
            "from users u, addresses a, namespaces n where "
            "u.login=$1 and u.id=a.id and n.id=u.parentspace"
        );

        psa = new PreparedStatement(
            "select u.id, u.address, u.inbox, n.name as parentspace, "
            "u.login, u.id, u.secret, a.name, a.localpart, a.domain "
            "from users u, addresses a, namespaces n where "
            "u.address=a.id and a.localpart=$1 and lower(a.domain)=$2 "
            "and n.id=u.parentspace"
        );
    }
    if ( !d->login.isEmpty() ) {
        d->q = new Query( *psl, this );
        d->q->bind( 1, d->login );
    }
    else if ( d->address ) {
        d->q = new Query( *psa, this );
        d->q->bind( 1, d->address->localpart() );
        d->q->bind( 2, d->address->domain().lower() );
    }
    if ( d->q ) {
        d->q->execute();
        d->mode = UserData::Refreshing;
    }
    else {
        user->notify();
    }
}


/*! Parses the query results for refresh(). */

void User::refreshHelper()
{
    if ( !d->q || !d->q->done() )
        return;

    d->state = Nonexistent;
    Row *r = d->q->nextRow();
    if ( r ) {
        d->id = r->getInt( "id" );
        d->login = r->getString( "login" );
        d->secret = r->getString( "secret" );
        d->id = r->getInt( "id" );
        d->inbox = Mailbox::find( r->getInt( "inbox" ) );
        d->home = Mailbox::obtain( r->getString( "parentspace" ) + "/" +
                                   d->login,
                                   true );
        String n = r->getString( "name" );
        String l = r->getString( "localpart" );
        String h = r->getString( "domain" );
        d->address = new Address( n, l, h );
        d->state = Refreshed;
    }
    if ( d->user )
        d->user->notify();
}


void User::execute()
{
    switch( d->mode ) {
    case UserData::Creating:
        createHelper();
        break;
    case UserData::Renaming:
        renameHelper();
        break;
    case UserData::Refreshing:
        refreshHelper();
        break;
    case UserData::Removing:
        removeHelper();
        break;
    case UserData::LoungingAround:
        break;
    }
}


/*! Creates this user in the database notifies \a user afterwards. If
    the user could not be created, error() returns a message about
    what went wrong.
*/

Query *User::create( EventHandler * user )
{
    Query *q = new Query( user );

    if ( !user || !valid() )
        return 0;

    if ( exists() ) {
        q->setError( "User exists already" );
        return q;
    }

    d->t = new Transaction( this );
    d->mode = UserData::Creating;
    d->user = user;
    d->createQuery = q;
    createHelper();
    return q;
}


/*! This private function carries out create() work on behalf of
    execute().
*/

void User::createHelper()
{
    Address * a = address();
    if ( !a->id() ) {
        // note: this doesn't wrap the address insert in our transaction
        List<Address> l;
        l.append( a );
        AddressCache::lookup( &l, this );
        return;
    }

    if ( !d->q ) {
        d->q = new Query( "insert into mailboxes (name) values "
                          "( (select name from namespaces where id="
                          "   (select max(id) from namespaces)) ||"
                          "  '/' || $1 || '/INBOX' )",
                          this );
        d->q->bind( 1, d->login );
        d->t->enqueue( d->q );

        Query * q2
            = new Query( "insert into users "
                         "(address,inbox,parentspace,login,secret)"
                         "values ($1,"
                         "(select id from mailboxes where name="
                         " (select name from namespaces where id="
                         "   (select max(id) from namespaces)) ||"
                         "  '/' || $2 || '/INBOX' ),"
                         "(select id from namespaces where id="
                         "   (select max(id) from namespaces)),"
                         "$2,$3)",
                         this );
        q2->bind( 1, a->id() );
        q2->bind( 2, d->login );
        q2->bind( 4, d->secret );
        d->t->enqueue( q2 );
        d->t->commit();
    }
    if ( !d->t->done() )
        return;
    if ( d->t->failed() )
        d->createQuery->setError( d->t->error() );
    else
        d->createQuery->setState( Query::Completed );
    d->user->notify();
}


/*! Returns true if this user is valid, that is, if it has the
    information that must be present in order to write it to the
    database and do not have defaults.

    Sets error() if applicable.
*/

bool User::valid()
{
    if ( d->login.isEmpty() ) {
        d->error = "Login name must be supplied";
        return false;
    }
    if ( d->secret.isEmpty() ) {
        d->error = "Login name <" + d->login + "> has no password";
        return false;
    }

    return true;
}


/*! Returns a textual description of the last error seen, or a null
    string if everything is in order. The string is set by valid() and
    perhaps other functions.
*/

String User::error() const
{
    return d->error;
}


/*! Returns true if this user is known to exist in the database, and
    false if it's unknown or doesn't exist.
*/

bool User::exists()
{
    return d->id > 0;
}


/*! Finishes the work of rename(). */

void User::renameHelper()
{
    if ( !d->q->done() )
        return;
    if ( d->q->failed() ) {
        d->error = "SQL error during user update: " + d->q->error();
        refresh( d->user );
    }
    d->user->notify();
}


/*! Renames this User to \a newLogin and notifies \a user when the
    operation is complete. exists() must be true to call this
    function.
*/

void User::rename( const String & newLogin, EventHandler * user  )
{
    if ( !exists() ) {
        d->error = "Cannot rename nonexistent user";
        return;
    }
    d->q = new Query( "update users set login=$1 where id=$2", this );
    d->q->bind( 1, newLogin );
    d->q->bind( 2, d->id );
    d->q->execute();
    d->login = newLogin;
}


/*! Renames the password of this User to \a newSecret and notifies \a
    user when the operation is complete. exists() must be true to call
    this function.
*/

void User::changeSecret( const String & newSecret, EventHandler * user )
{
    if ( !exists() ) {
        d->error = "Cannot set password for nonexistent user";
        return;
    }
    d->q = new Query( "update users set secret=$1 where id=$2", this );
    d->q->bind( 1, newSecret );
    d->q->bind( 2, d->id );
    d->q->execute();
    d->secret = newSecret;
}


/*! Removes this user from the database and notifies \a user when the
    operation is complete.
*/

void User::remove( EventHandler * user )
{
    if ( !exists() )
        return;
    d->q = new Query( "delete from users where id=$1", this );
    d->q->bind( 1, d->id );
}


/*! Finishes the work of remove(). */

void User::removeHelper()
{
    if ( d->q->done() )
        d->id = 0;
}


/*! Returns the user's "home directory" - the mailbox under which all
    of the user's mailboxes reside.

    This is read-only since at the moment, the mailstore servers only
    permit one setting: "/users/" + login. However, the database
    permits more namespaces than just "/users", so one day this may
    change.
*/

Mailbox * User::home() const
{
    return d->home;
}


/*! Returns the user's ID, ie. the primary key from the database, used
    to link various other tables to this user.
*/

uint User::id() const
{
    return d->id;
}


/*! Returns the user's state, which is either Unverified (the object
  has made no attempt to refresh itself from the database), Refreshed
  (the object was successfully refreshed) or Nonexistent (the object
  tried to refresh itself, but there was no corresponsing user in the
  database).

  The state is Unverified initially and is changed by refresh().
*/

User::State User::state() const
{
    return d->state;
}
