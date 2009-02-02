// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#ifndef STRING_H
#define STRING_H

#include "global.h"


class StringData
    : public Garbage
{
private:
    StringData(): str( 0 ), len( 0 ), max( 0 ) {
        setFirstNonPointer( &len );
    }
    StringData( int );

    friend class String;
    friend bool operator==( const class String &, const class String & );
    friend bool operator==( const class String &, const char * );
    void * operator new( size_t, uint );
    void * operator new( size_t s ) { return Garbage::operator new( s); }

    char * str;
    uint len;
    uint max;
};


class String
    : public Garbage
{
public:
    String();
    String( const char * );
    String( const char *, uint );
    String( const String & );
    ~String();

    String & operator=( const String & );
    String & operator=( const char * );
    String & operator+=( const String & str ) { append( str ); return *this; }

    void operator delete( void * );

    // const, returns zero when used beyond the end
    inline char operator[]( uint i ) const {
        if ( !d )
            return 0;
        if ( i >= d->len )
            return 0;
        return d->str[i];
    }

    inline char at( uint i ) const { return (*this)[i]; }

    inline bool isEmpty() const { return !d || !d->len; }
    inline uint length() const { return d ? d->len : 0; }
    inline uint capacity() const { return d ? d->max : 0; }
    inline const char * data() const { return d ? (const char*)d->str : 0; }
    const char * cstr();
    const char * cstr() const;

    String lower() const;
    String upper() const;
    String headerCased() const;
    String mid( uint, uint = UINT_MAX ) const;
    String simplified() const;
    String trimmed() const;
    String stripCRLF() const;
    String hex() const;
    bool isQuoted( char = '"', char = '\\' ) const;
    String unquoted( char = '"', char = '\\' ) const;
    String quoted( char = '"', char = '\\' ) const;

    enum Boring { Totally, IMAP, MIME };
    bool boring( Boring = Totally ) const;

    bool startsWith( const String & ) const;
    bool startsWith( const char * ) const;
    bool endsWith( const String & ) const;
    bool endsWith( const char * ) const;
    uint number( bool *, uint = 10 ) const;
    static String fromNumber( int64, uint = 10 );
    void appendNumber( int64, uint = 10 );
    static String humanNumber( int64 );

    int find( char, int=0 ) const;
    int find( const String &, int=0 ) const;
    bool contains( const String & ) const;
    bool contains( const char ) const;
    bool containsWord( const String & ) const;
    String section( const String &, uint ) const;

    void replace( const String &, const String & );

    void append( char );
    void appendNumber( int, int = 10 );
    void appendNumber( uint, int = 10 );
    void append( const String & );
    void append( const char *, uint );
    void append( const char * );

    void prepend( const String & );

    void reserve( uint );
    void reserve2( uint );
    void truncate( uint = 0 );
    void setLength( uint );

    enum Encoding { Binary, Base64, QP, Uuencode };
    String encoded( Encoding, uint = 0 ) const;
    String decoded( Encoding ) const;

    String eURI() const;
    String deURI() const;
    String de64() const;
    String deUue() const;
    String e64( uint = 0 ) const;
    String deQP( bool = false ) const;
    String eQP( bool = false, bool = false ) const;
    bool needsQP() const;

    friend inline bool operator==( const String &, const String & );
    friend bool operator==( const String &, const char * );

    bool operator<( const String & ) const;
    bool operator>( const String & ) const;
    bool operator<=( const String & ) const;
    bool operator>=( const String & ) const;

    bool operator<( const char * ) const;

    int compare( const String & ) const;

    inline void detach() { if ( !modifiable() ) reserve( length() ); }

    bool modifiable() const { return d && d->max > 0; }

    void print() const;

    String anonymised() const;

    String crlf() const;

    String wrapped( uint linelength,
                    const String & firstPrefix, const String & otherPrefix,
                    bool spaceAtEOL ) const;

private:
    StringData * d;
};


// since operator== is called so often, we provide fastish inlines
inline bool operator==( const String & a, const String & b ) {
    uint al = a.length();
    uint bl = b.length();
    if ( !al && !bl )
        return true;
    if ( !al || !bl || al != bl )
        return false;
    if ( a.d == b.d )
        return true;
    uint i = 0;
    while ( i < al && i < bl && a.d->str[i] == b.d->str[i] )
        i++;
    if ( i < al )
        return false;
    return true;
}


inline bool operator==( const String & a, const char * b ) {
    uint al = a.length();
    if ( !b || !*b ) {
        if ( al == 0 )
            return true;
        return false;
    }
    uint i = 0;
    while ( i < al && a.d->str[i] == b[i] && b[i] != '\0' )
        i++;
    if ( i < al )
        return false;
    if ( b[i] != '\0' )
        return false;
    return true;
}


inline bool operator==( const char * a, const String & b ) {
    return b == a;
}


inline bool operator!=( const String & a, const char * b ) {
    return !( a == b );
}


inline bool operator!=( const char * a, const String & b ) {
    return !( b == a );
}


inline bool operator!=( const String & a, const String & b ) {
    return !( a == b );
}


extern const String operator+( const String & a, const String & b );
extern const String operator+=( const String & a, const String & b );


inline String fn( int64 n, uint b = 10 )
{
    return String::fromNumber( n, b );
}


#endif