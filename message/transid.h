#ifndef TRANSID_H
#define TRANSID_H

#include "estring.h"
#include "address.h"

class TransID
    : public Garbage
{
public:
    TransID();
    TransID( const EString & );

    bool valid() const;
    EString error() const;

    EString toString() const;

private:
    class TransIDData * d;

    void init( const EString & );

    void setError( const EString & );
    void validate();
};

#endif
