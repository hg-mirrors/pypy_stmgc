#ifndef _STM_FINALIZER_H_
#define _STM_FINALIZER_H_

#include <stdint.h>

/* see deal_with_objects_with_finalizers() for explanation of these fields */
struct finalizers_s {
    long lock;
    struct stm_priv_segment_info_s * running_trigger_now; /* our PSEG, if we are running triggers */
    struct list_s *objects_with_finalizers;
    struct list_s *probably_young_objects_with_finalizers; /* empty on g_finalizers! */
    struct list_s *run_finalizers;
};

static void mark_visit_from_finalizer_pending(void);
static void deal_with_young_objects_with_destructors(void);
static void deal_with_old_objects_with_destructors(void);
static void deal_with_objects_with_finalizers(void);

static void setup_finalizer(void);
static void teardown_finalizer(void);

static void _commit_finalizers(void);
static void abort_finalizers(struct stm_priv_segment_info_s *);

#define commit_finalizers()   do {              \
    if (STM_PSEGMENT->finalizers != NULL)       \
        _commit_finalizers();                   \
} while (0)


/* regular finalizers (objs from already-committed transactions) */
static struct finalizers_s g_finalizers;
static struct {
    int count;
    stm_finalizer_trigger_fn *triggers;
} g_finalizer_triggers;


static void _invoke_general_finalizers(stm_thread_local_t *tl);
static void _invoke_local_finalizers(void);

#define invoke_general_finalizers(tl)    do {   \
     _invoke_general_finalizers(tl);         \
} while (0)


#define exec_local_finalizers()  do {                   \
     _invoke_local_finalizers();                     \
} while (0)

#endif
