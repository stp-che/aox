#include "transid.h"


class TransIDData
    : public Garbage
{
public:
    EString origin;
    EString error;
};


TransID::TransID()
    : d( new TransIDData )
{
}

TransID::TransID( const EString &s )
    : d( 0 )
{
    init( s );
}

void TransID::init( const EString &s )
{
    d = new TransIDData;
    d->origin = s;
    validate();
}

bool TransID::valid() const
{
    return error().isEmpty();
}

void TransID::setError( const EString &message )
{
    d->error = message;
}

EString TransID::error() const
{
    return d->error;
}

EString TransID::toString() const
{
    return d->origin;
}

void TransID::validate()
{
    EString invalidFomatMsg = "Invalid format (usage: <12345@mail.org>)";

    if ( d->origin.isEmpty() ) {
        setError( invalidFomatMsg );
        return;
    }

    if ( d->origin[0] != '<' || d->origin[d->origin.length()-1] != '>' ) {
        setError( invalidFomatMsg );
        return;
    }

    AddressParser ap( d->origin );
    if ( !ap.error().isEmpty() || ap.addresses()->count() != 1 ) {
        setError( invalidFomatMsg );
        return;
    }

    Address * a = ap.addresses()->firstElement();
    if ( a->type() != Address::Normal ) {
        setError( invalidFomatMsg );
        return;
    }
}
