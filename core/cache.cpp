// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include "cache.h"

#include "list.h"
#include "allocator.h"


static List<Cache> * caches;


/*! \class Cache cache.h
  
    The Cache class is an abstract superclass which allows keeping
    objects in RAM until the next garbage allocation
    (Allocator::free().

    In practice many objects stay around taking up RAM until GC, so we
    might as well use them. For example, if a Message is used several
    times in quick succession, why shouldn't we use the copy that
    actually is there in RAM?

    Subclasses of Cache have to provide cache insertion and
    retrieval. This class provides only one bit of core functionality,
    namely clearing the cache at GC time.
*/


/*! Constructs an empty Cache. This constructor makes sure the object
    will not be freed during garbage collection, and that clear() will
    be called when appropriate.
*/

Cache::Cache()
    : Garbage()
{
    if ( !::caches ) {
        ::caches = new List<Cache>;
        Allocator::addEternal( ::caches, "RAM caches" );
    }

    ::caches->append( this );
}


/*! Destroys the cache and ensures that clear() won't be called any
    more.
*/

Cache::~Cache()
{
    if ( ::caches )
        ::caches->remove( this );
}


/*! Calls clear() for each currently extant Cache. Called from
    Allocator::free().
*/

void Cache::clearAllCaches()
{
    List<Cache>::Iterator i( ::caches );
    while ( i ) {
        Cache * c = i;
        ++i;
        c->clear(); // careful: no iterator pointing to c meanwhile
    }
}