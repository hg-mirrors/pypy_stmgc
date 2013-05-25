#ifndef _SRCSTM_IMPL_H
#define _SRCSTM_IMPL_H

#ifdef _GC_ON_CPYTHON
#  include <Python.h>
#else
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#  endif
#  ifndef _XOPEN_SOURCE
#    define _XOPEN_SOURCE 500
#  endif
#endif

#define DUMP_EXTRA

#include <stddef.h>
#include <setjmp.h>
#include <pthread.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "stmgc.h"
#include "atomic_ops.h"
#include "lists.h"
#include "dbgmem.h"
#include "nursery.h"
#include "gcpage.h"
#include "et.h"
#include "stmsync.h"

#endif
