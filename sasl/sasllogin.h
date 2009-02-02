// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#ifndef SASLLOGIN_H
#define SASLLOGIN_H

#include "mechanism.h"


class SaslLogin
    : public SaslMechanism
{
public:
    SaslLogin( EventHandler * );
    EString challenge();
    void parseResponse( const EString & );
};


#endif
