// Copyright 2009 The Archiveopteryx Developers <info@aox.org>

#ifndef NOOP_H
#define NOOP_H

#include "command.h"


class Noop
    : public Command
{
public:
    void execute();
};


class Check
    : public Command
{
public:
    void execute();
};


#endif
