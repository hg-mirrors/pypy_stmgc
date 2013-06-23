#ifndef _DUHTON_AME_H_
#define _DUHTON_AME_H_

#include <setjmp.h>
#include "atomic_ops.h"
#include "../duhton.h"


DuObject *_Du_AME_read_from_global(DuObject *glob, owner_version_t *vers_out);
void _Du_AME_oreclist_insert(DuObject *glob);
DuObject *_Du_AME_writebarrier(DuObject *glob);
DuObject *_Du_AME_getlocal(DuObject *glob);

void _Du_AME_InitThreadDescriptor(void);
void _Du_AME_FiniThreadDescriptor(void);
void _Du_AME_StartTransaction(jmp_buf *);
void _Du_AME_CommitTransaction(void);

void Du_AME_TryInevitable(void);


#define Du_AME_READ(ob, READ_OPERATIONS)                                \
    while (1) {                                                         \
        DuObject *__du_ame_ob = (DuObject *)(ob);                       \
        if (Du_AME_GLOBAL(__du_ame_ob)) {                               \
            owner_version_t __du_ame_version;                           \
            __du_ame_ob = _Du_AME_read_from_global(__du_ame_ob,         \
                                                   &__du_ame_version);  \
            (ob) = (typeof(ob))__du_ame_ob;                             \
            if (Du_AME_GLOBAL(__du_ame_ob)) {                           \
                CFENCE;                                                 \
                READ_OPERATIONS;                                        \
                CFENCE;                                                 \
                __du_ame_ob = (DuObject *)(ob);                         \
                if (__du_ame_ob->ob_version != __du_ame_version)        \
                    continue;                                           \
                _Du_AME_oreclist_insert(__du_ame_ob);                   \
                break;                                                  \
            }                                                           \
        }                                                               \
        READ_OPERATIONS;                                                \
        break;                                                          \
    }


#define Du_AME_WRITE(ob)                                                \
    do {                                                                \
        if (Du_AME_GLOBAL(ob))                                          \
            (ob) = (typeof(ob))_Du_AME_writebarrier((DuObject *)(ob));  \
    } while (0)


#define Du_AME_INEVITABLE(ob)                                           \
    do {                                                                \
        Du_AME_TryInevitable();                                         \
        if (Du_AME_GLOBAL(ob))                                          \
            (ob) = (typeof(ob))_Du_AME_getlocal((DuObject *)(ob));      \
    } while (0)
/* XXX must also wait for ob to be unlocked, if necessary */


#endif  /* _DUHTON_AME_H_ */
