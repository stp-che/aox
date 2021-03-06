// Copyright 2009 The Archiveopteryx Developers <info@aox.org>

#ifndef INTEGERSET_H
#define INTEGERSET_H

#include "estring.h"


class IntegerSet
    : public Garbage
{
public:
    IntegerSet();
    IntegerSet( const IntegerSet & );

    IntegerSet& operator=( const IntegerSet & );

    uint smallest() const;
    uint largest() const;
    uint count() const;
    bool isEmpty() const;

    bool contains( uint ) const;
    bool contains( const IntegerSet & ) const;

    uint value( uint ) const;
    uint index( uint ) const;

    EString set() const;
    EString csl() const;

    void add( uint, uint );
    void add( uint n ) { add( n, n ); }
    void add( const IntegerSet & );

    void remove( uint );
    void remove( uint, uint );
    void remove( const IntegerSet & );
    void clear();

    IntegerSet intersection( const IntegerSet & ) const;

private:
    class SetData * d;
    void recount() const;
};


#endif
