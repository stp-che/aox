// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#ifndef CP949
#define CP949

#include "codec.h"


class Cp949Codec
    : public Codec
{
public:
    Cp949Codec( const char * = 0 );

    EString fromUnicode( const UString & );
    UString toUnicode( const EString & );
};


#endif
