#ifndef _SRCSTM_NURSERY_H
#define _SRCSTM_NURSERY_H

#ifndef GC_NURSERY
#define GC_NURSERY        4194304    /* 4 MB */
//#define GC_NURSERY        (1<<20)    /* 1 MB */
#endif


#define NURSERY_FIELDS_DECL                                             \
    /* the nursery */                                                   \
    char *nursery_current;                                              \
    char *nursery_end;                                                  \
    char *nursery_base;                                                 \
                                                                        \
    /* Between collections, we add to 'old_objects_to_trace' the        \
       private objects that are old but may contain pointers to         \
       young objects.  During minor collections the same list is        \
       used to record all other old objects pending tracing; in         \
       other words minor collection is a process that works             \
       until the list is empty again. */                                \
    struct GcPtrList old_objects_to_trace;                              \
                                                                        \
    /* 'public_with_young_copy' is a list of all public objects         \
       that are outdated and whose 'h_revision' points to a             \
       young object. */                                                 \
    struct GcPtrList public_with_young_copy;                            \
                                                                        \
    /* These numbers are initially zero, but after a minor              \
       collection, they are set to the size of the two lists            \
       'private_from_protected' and 'list_of_read_objects'.             \
       It's used on the following minor collection, if we're            \
       still in the same transaction, to know that the initial          \
       part of the lists cannot contain young objects any more. */      \
    long num_private_from_protected_known_old;                          \
    long num_read_objects_known_old;


struct tx_descriptor;  /* from et.h */

void stmgc_init_nursery(void);
void stmgc_done_nursery(void);
void stmgc_minor_collect(void);
void stmgc_minor_collect_no_abort(void);
int stmgc_minor_collect_anything_to_do(struct tx_descriptor *);
gcptr stmgc_duplicate(gcptr);
gcptr stmgc_duplicate_old(gcptr);

#endif
