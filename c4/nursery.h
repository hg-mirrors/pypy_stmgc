#ifndef _SRCSTM_NURSERY_H
#define _SRCSTM_NURSERY_H

#ifndef GC_NURSERY
#define GC_NURSERY        4194304    /* 4 MB */
#endif


#define NURSERY_FIELDS_DECL                             \
    char *nursery_current;                              \
    char *nursery_end;                                  \
    char *nursery_base;                                 \
    struct GcPtrList old_objects_to_trace;

struct tx_descriptor;  /* from et.h */

void stmgc_init_nursery(void);
void stmgc_done_nursery(void);
void stmgc_minor_collect(void);
int stmgc_minor_collect_anything_to_do(struct tx_descriptor *);

#endif
