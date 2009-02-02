// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#ifndef MULTIPART_H
#define MULTIPART_H

#include "list.h"

class Header;
class Message;
class Bodypart;
class ContentType;


class Multipart
    : public Garbage
{
public:
    Multipart();
    virtual ~Multipart();

    Header * header() const;
    void setHeader( Header * );

    Multipart * parent() const;
    void setParent( Multipart * );

    virtual bool isMessage() const;
    virtual bool isBodypart() const;

    List< Bodypart > * children() const;

    void appendMultipart( EString & ) const;
    void appendAnyPart( EString &, const Bodypart *, ContentType * ) const;
    void appendTextPart( EString &, const Bodypart *, ContentType * ) const;

private:
    Header * h;
    Multipart * p;
    List< Bodypart > * parts;
};


#endif
