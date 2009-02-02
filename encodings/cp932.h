// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#ifndef CP932
#define CP932

#include "codec.h"


class Cp932Codec
    : public Codec
{
public:
    Cp932Codec();

    EString fromUnicode( const UString & );
    UString toUnicode( const EString & );
};


#endif
