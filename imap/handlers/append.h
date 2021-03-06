// Copyright 2009 The Archiveopteryx Developers <info@aox.org>

#ifndef APPEND_H
#define APPEND_H

#include "command.h"


class Append
    : public Command
{
public:
    Append();

    void parse();
    void execute();

private:
    uint number( uint );
    void process( class Appendage * );

    class AppendData * d;
};


#endif
