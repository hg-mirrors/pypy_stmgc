#ifndef _SRCSTM_STMTLS_H
#define _SRCSTM_STMTLS_H

#ifndef GC_NURSERY
#define GC_NURSERY        4194304    /* 4 MB */
#endif


#define NURSERY_FIELDS_DECL                             \
    char *nursery_current;                              \
    char *nursery_end;                                  \
    char *nursery;                                      \
    gcptr *shadowstack;                                 \
    gcptr **shadowstack_end_ref;                        \
    gcptr *thread_local_obj_ref;                        \
                                                        \
    revision_t collection_lock, debug_nursery_access;   \
    struct G2L young_objects_outside_nursery;           \
    struct GcPtrList old_objects_to_trace;              \
    long num_read_objects_known_old;                    \
                                                        \
    struct GcPtrList protected_with_private_copy;       \
    struct G2L public_to_private;                       \
    struct GcPtrList private_old_pointing_to_young;     \
    struct GcPtrList public_to_young;                   \
    long num_public_to_protected;                       \
    struct GcPtrList stolen_objects;


struct tx_descriptor;  /* from et.h */

enum protection_class_t { K_PRIVATE, K_PROTECTED, K_PUBLIC,
                          K_OLD_PRIVATE  /* <-only for dclassify() */ };

gcptr stmgc_duplicate(gcptr, revision_t);
void stmgc_start_transaction(struct tx_descriptor *);
void stmgc_stop_transaction(struct tx_descriptor *);
void stmgc_suspend_commit_transaction(struct tx_descriptor *d);
void stmgc_committed_transaction(struct tx_descriptor *);
void stmgc_abort_transaction(struct tx_descriptor *);
void stmgc_init_tls(void);
void stmgc_done_tls(void);
void stmgc_minor_collect(void);
void stmgc_minor_collect_no_abort(void);
int stmgc_minor_collect_anything_to_do(struct tx_descriptor *);
void stmgc_write_barrier(gcptr);
enum protection_class_t stmgc_classify(gcptr);
int stmgc_is_young_in(struct tx_descriptor *, gcptr);
void stmgc_public_to_foreign_protected(gcptr);
int stmgc_nursery_hiding(struct tx_descriptor *, int);
void stmgc_normalize_stolen_objects(void);

#ifdef _GC_DEBUG
int is_young(gcptr);
#else
#  define is_young(o)  (((o)->h_tid & GCFLAG_OLD) == 0)
#endif

#endif
