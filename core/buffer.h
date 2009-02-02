// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#ifndef BUFFER_H
#define BUFFER_H

#include "global.h"
#include "list.h"


class EString;
class Filter;


class Buffer
    : public Garbage
{
public:
    Buffer();

    void addFilter( Filter * );

    void append( const EString & );
    void append( const char *, uint = 0 );

    void read( int );
    void write( int );

    bool eof() const;
    uint error() const;
    uint size() const { return bytes; }
    void remove( uint );
    EString string( uint ) const;
    EString * removeLine( uint = 0 );

    char operator[]( uint i ) const {
        if ( i >= bytes )
            return 0;

        i += firstused;
        Vector *v = vecs.firstElement();
        if ( v && v->len > i )
            return *( v->base + i );

        return at( i );
    }

private:
    char at( uint ) const;

private:
    struct Vector
        : public Garbage
    {
        Vector() : base( 0 ), len( 0 ) {
            setFirstNonPointer( &len );
        }
        char *base;
        // no pointers after this line
        uint len;
    };

    List< Vector > vecs;
    Filter * filter;
    Buffer * next;
    uint firstused, firstfree;
    bool seenEOF;
    uint bytes;
    uint err;
};


#endif
