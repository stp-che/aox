#ifndef INJECTOR_H
#define INJECTOR_H

#include "header.h"
#include "event.h"
#include "list.h"

class Message;
class Mailbox;


class Injector
    : public EventHandler
{
public:
    Injector( const Message *, List< Mailbox > *, EventHandler * );
    virtual ~Injector();

    bool done() const;
    bool failed() const;
    void execute();

private:
    class InjectorData *d;

    void selectUids();
    void updateAddresses();
    void insertBodyparts();
};


#endif
