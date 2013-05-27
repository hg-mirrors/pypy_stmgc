/* -*- c-basic-offset: 2 -*- */

/* XXX assumes that time never wraps around (in a 'long'), which may be
 * correct on 64-bit machines but not on 32-bit machines if the process
 * runs for long enough.
 */
#include "stmimpl.h"


__thread struct tx_descriptor *thread_descriptor = NULL;

/* 'global_cur_time' is normally a multiple of 2, except when we turn
   a transaction inevitable: we then add 1 to it. */
static revision_t global_cur_time = 2;

/* 'next_locked_value' is incremented by two for every thread that starts.
   XXX it should be fixed at some point because right now the process will
   die if we start more than 0x7fff threads. */
static revision_t next_locked_value = (LOCKED + 1) | 1;

/* a negative odd number that uniquely identifies the currently running
   transaction (but the number in aborted transactions is reused).
   Because we don't know yet the value of 'global_cur_time' that we'll
   be assigned when we commit, we use the (negative of) the value of
   'global_cur_time' when we committed the previous transaction. */
__thread revision_t stm_local_revision;


revision_t stm_global_cur_time(void)  /* for tests */
{
  return global_cur_time;
}
revision_t stm_local_rev(void)        /* for tests */
{
  return stm_local_revision;
}
struct tx_descriptor *stm_thread_descriptor(void)  /* for tests */
{
  return thread_descriptor;
}

/************************************************************/

static void ValidateNow(struct tx_descriptor *);
static void CancelLocks(struct tx_descriptor *d);

static _Bool is_inevitable(struct tx_descriptor *d)
{
  /* Assert that we are running a transaction.
   *      Returns True if this transaction is inevitable. */
  assert(d->active == 1 + !d->setjmp_buf);
  return d->active == 2;
}

static pthread_mutex_t mutex_inevitable = PTHREAD_MUTEX_INITIALIZER;

static void inev_mutex_acquire(void)
{   /* must save roots around this call */
  stm_stop_sharedlock();
  pthread_mutex_lock(&mutex_inevitable);
  stm_start_sharedlock();
}

static void inev_mutex_release(void)
{
  pthread_mutex_unlock(&mutex_inevitable);
}

/************************************************************/

static gcptr HeadOfRevisionChainList(struct tx_descriptor *d, gcptr G)
{
  gcptr R = G;
  revision_t v;

 retry:
  v = ACCESS_ONCE(R->h_revision);
  if (!(v & 1))  // "is a pointer", i.e.
    {            //      "has a more recent revision"
      if (v & 2)
        {
        old_to_young:
          v &= ~2;
          if (UNLIKELY(!stmgc_is_young_in(d, (gcptr)v)))
            {
              stmgc_public_to_foreign_protected(R);
              goto retry;
            }
          R = (gcptr)v;
          goto retry;
        }

      gcptr R_prev = R;
      R = (gcptr)v;

      /* retry */
      v = ACCESS_ONCE(R->h_revision);
      if (!(v & 1))  // "is a pointer", i.e.
        {            //      "has a more recent revision"
          if (v & 2)
            goto old_to_young;

          /* we update R_prev->h_revision as a shortcut */
          /* XXX check if this really gives a worse performance than only
             doing this write occasionally based on a counter in d */
          R = (gcptr)v;
          if (R->h_revision == stm_local_revision)
            {
              /* must not update an older h_revision to go directly to
                 the private copy at the end of a chain of protected
                 objects! */
              return R;
            }
          R_prev->h_revision = v;
          goto retry;
        }
    }

  if (UNLIKELY(v > d->start_time))   // object too recent?
    {
      if (v >= LOCKED)
        {
          SpinLoop(SPLP_LOCKED_INFLIGHT);
          goto retry;                // spinloop until it is no longer LOCKED
        }
      ValidateNow(d);                  // try to move start_time forward
      goto retry;                      // restart searching from R
    }
  return R;
}

static inline gcptr AddInReadSet(struct tx_descriptor *d, gcptr R)
{
  fprintf(stderr, "AddInReadSet(%p)\n", R);
  d->count_reads++;
  if (!fxcache_add(&d->recent_reads_cache, R)) {
      /* not in the cache: it may be the first time we see it,
       * so insert it into the list */
      gcptrlist_insert(&d->list_of_read_objects, R);
  }
      //      break;

      //  case 2:
      /* already in the cache, and FX_THRESHOLD reached */
      //      return Localize(d, R);
      //  }
  return R;
}

gcptr stm_DirectReadBarrier(gcptr G)
{
  gcptr R;
  struct tx_descriptor *d = thread_descriptor;
  assert(d->active >= 1);

  /* XXX optimize me based on common patterns */
  R = HeadOfRevisionChainList(d, G);

  if (R->h_tid & GCFLAG_PUBLIC_TO_PRIVATE)
    {
      wlog_t *entry;
      gcptr L;
      G2L_FIND(d->public_to_private, R, entry, goto not_found);
      L = entry->val;
      assert(L->h_revision == stm_local_revision);
#if 0
      if (R_Container && !(R_Container->h_tid & GCFLAG_GLOBAL))
        {    /* R_Container is a local object */
          gcptr *ref = (gcptr *)(((char *)R_Container) + offset);
          *ref = L;   /* fix in-place */
        }
#endif
      return L;

    not_found:;
    }
  R = AddInReadSet(d, R);
  return R;
}

static gcptr _latest_gcptr(gcptr R)
{
  /* don't use, for tests only */
  revision_t v;
 retry:
  v = R->h_revision;
  if (!(v & 1))  // "is a pointer", i.e.
    {            //      "has a more recent revision"
      if (v & 2)
        {
          v &= ~2;
          if (!stmgc_is_young_in(thread_descriptor, (gcptr)v))
            return NULL;   /* can't access */
        }
      R = (gcptr)v;
      goto retry;
    }
  return R;
}

gcptr _stm_nonrecord_barrier(gcptr obj, int *result)
{
  /* warning, this is for tests only, and it is not thread-safe! */
  enum protection_class_t e = stmgc_classify(obj);
  if (e == K_PRIVATE)
    {
      *result = 2;   /* 'obj' a private object to start with */
      return obj;
    }
  obj = _latest_gcptr(obj);
  if (obj == NULL)
    {
      assert(e == K_PUBLIC);
      *result = 3;   /* can't check anything: we'd need foreign access */
      return NULL;
    }
  if (stmgc_classify(obj) == K_PRIVATE)
    {
      *result = 1;
      return obj;
    }

  struct tx_descriptor *d = thread_descriptor;
  wlog_t *item;
  G2L_LOOP_FORWARD(d->public_to_private, item)
    {
      gcptr R = item->addr;
      gcptr L = item->val;
      if (_latest_gcptr(R) == obj)
        {
          /* 'obj' has a private version.  The way we detect this lets us
             find it even if we already have a committed version that
             will cause conflict. */
          *result = 1;
          return L;
        }
    } G2L_LOOP_END;

  if (obj->h_revision > d->start_time)
    {
      /* 'obj' has no private version, and the public version was modified */
      *result = -1;
      return NULL;
    }
  else
    {
      /* 'obj' has only an up-to-date public version */
      *result = 0;
      return obj;
    }
}

#if 0
void *stm_DirectReadBarrierFromR(void *G1, void *R_Container1, size_t offset)
{
  return _direct_read_barrier((gcptr)G1, (gcptr)R_Container1, offset);
}
#endif

gcptr stm_RepeatReadBarrier(gcptr O)
{
  abort();//XXX
#if 0
  // LatestGlobalRevision(O) would either return O or abort
  // the whole transaction, so omitting it is not wrong
  struct tx_descriptor *d = thread_descriptor;
  gcptr L;
  wlog_t *entry;
  G2L_FIND(d->global_to_local, O, entry, return O);
  L = entry->val;
  assert(L->h_revision == stm_local_revision);
  return L;
#endif
}

static gcptr LocalizeProtected(struct tx_descriptor *d, gcptr R)
{
  assert(!(R->h_tid & GCFLAG_PUBLIC_TO_PRIVATE));

  /* duplicate, and save the original R->h_revision into an extra
     word allocated just after L */
  assert(R->h_revision & 1);
  gcptr L = stmgc_duplicate(R, R->h_revision);

  /* cross-thread memory barrier: make sure the local object is correct
     and has h_revision == stm_local_revision, and the extra word is
     written as well; when it is done, and only then, then we change
     R->h_revision */
  smp_wmb();

  R->h_revision = (revision_t)L;

  gcptrlist_insert(&d->protected_with_private_copy, R);
  AddInReadSet(d, R);
  /*mark*/
  return L;
}

static gcptr LocalizePublic(struct tx_descriptor *d, gcptr R)
{
  if (R->h_tid & GCFLAG_PUBLIC_TO_PRIVATE)
    {
      wlog_t *entry;
      gcptr L;
      G2L_FIND(d->public_to_private, R, entry, goto not_found);
      L = entry->val;
      assert(L->h_revision == stm_local_revision);
      return L;
    }
  else
    R->h_tid |= GCFLAG_PUBLIC_TO_PRIVATE;

 not_found:;
  gcptr L = stmgc_duplicate(R, 0);
  assert(L->h_revision == stm_local_revision);
  g2l_insert(&d->public_to_private, R, L);
  gcptrlist_insert(&d->public_to_young, R);
  AddInReadSet(d, R);
  /*mark*/
  return L;
}

gcptr stm_WriteBarrier(gcptr P)
{
  gcptr R, W;
  struct tx_descriptor *d = thread_descriptor;
  assert(d->active >= 1);

  /* must normalize the situation now, otherwise we risk that
     LocalizePublic creates a new private version of a public
     object that has got one, attached to the equivalent stolen
     protected object */
  if (gcptrlist_size(&d->stolen_objects) > 0)
    stmgc_normalize_stolen_objects();

  /* XXX optimize me based on common patterns */
  R = HeadOfRevisionChainList(d, P);

  switch (stmgc_classify(R)) {
  case K_PRIVATE:   W = R;                       break;
  case K_PROTECTED: W = LocalizeProtected(d, R); break;
  case K_PUBLIC:    W = LocalizePublic(d, R);    break;
  default: abort();
  }

  if (W->h_tid & GCFLAG_WRITE_BARRIER)
    stmgc_write_barrier(W);

  return W;
}

/************************************************************/

static revision_t GetGlobalCurTime(struct tx_descriptor *d)
{
  assert(!is_inevitable(d));    // must not be myself inevitable
  return ACCESS_ONCE(global_cur_time) & ~1;
}

static _Bool ValidateDuringTransaction(struct tx_descriptor *d,
                                       _Bool during_commit)
{
  long i, size = d->list_of_read_objects.size;
  gcptr *items = d->list_of_read_objects.items;

  for (i=0; i<size; i++)
    {
      gcptr R = items[i];
      revision_t v;
    retry:
      v = ACCESS_ONCE(R->h_revision);
      if (!(v & 1))               // "is a pointer", i.e.
        {                         //   "has a more recent revision"
          /* ... unless it is a GCFLAG_STOLEN object */
          if (R->h_tid & GCFLAG_STOLEN)
            {
              assert(is_young(R));
              assert(!is_young((gcptr)v));
              R = (gcptr)v;
              goto retry;
            }
          return 0;               // really has a more recent revision
        }
      if (v >= LOCKED)            // locked
        {
          if (!during_commit)
            {
              assert(v != d->my_lock);    // we don't hold any lock
              SpinLoop(SPLP_LOCKED_VALIDATE);
              goto retry;
            }
          else
            {
              if (v != d->my_lock)         // not locked by me: conflict
                return 0;
            }
        }
    }
  return 1;
}

static void ValidateNow(struct tx_descriptor *d)
{
  d->start_time = GetGlobalCurTime(d);   // copy from the global time
  fprintf(stderr, "et.c: ValidateNow: %ld\n", (long)d->start_time);
  if (!ValidateDuringTransaction(d, 0))
    AbortTransaction(ABRT_VALIDATE_INFLIGHT);
}

/************************************************************/

void SpinLoop(int num)
{
  struct tx_descriptor *d = thread_descriptor;
  assert(d->active >= 1);
  assert(num < SPINLOOP_REASONS);
  d->num_spinloops[num]++;
  smp_spinloop();
}

#if 0
size_t _stm_decode_abort_info(struct tx_descriptor *d, long long elapsed_time,
                              int abort_reason, char *output);
#endif

void AbortTransaction(int num)
{
  struct tx_descriptor *d = thread_descriptor;
  unsigned long limit;
  struct timespec now;
  long long elapsed_time;

  assert(d->active != 0);
  assert(!is_inevitable(d));
  assert(num < ABORT_REASONS);
  d->num_aborts[num]++;

  CancelLocks(d);

  /* compute the elapsed time */
  if (d->start_real_time.tv_nsec != -1 &&
      clock_gettime(CLOCK_MONOTONIC, &now) >= 0) {
    elapsed_time = now.tv_sec - d->start_real_time.tv_sec;
    elapsed_time *= 1000000000;
    elapsed_time += now.tv_nsec - d->start_real_time.tv_nsec;
    if (elapsed_time < 1)
      elapsed_time = 1;
  }
  else {
    elapsed_time = 1;
  }

#if 0
  size_t size;
  if (elapsed_time >= d->longest_abort_info_time)
    {
      /* decode the 'abortinfo' and produce a human-readable summary in
         the string 'longest_abort_info' */
      size = _stm_decode_abort_info(d, elapsed_time, num, NULL);
      free(d->longest_abort_info);
      d->longest_abort_info = malloc(size);
      if (d->longest_abort_info == NULL)
        d->longest_abort_info_time = 0;   /* out of memory! */
      else
        {
          if (_stm_decode_abort_info(d, elapsed_time,
                                     num, d->longest_abort_info) != size)
            {
              fprintf(stderr,
                      "during stm abort: object mutated unexpectedly\n");
              abort();
            }
          d->longest_abort_info_time = elapsed_time;
        }
    }
#endif

#if 0
  /* run the undo log in reverse order, cancelling the values set by
     stm_ThreadLocalRef_LLSet(). */
  if (d->undolog.size > 0) {
      gcptr *item = d->undolog.items;
      long i;
      for (i=d->undolog.size; i>=0; i-=2) {
          void **addr = (void **)(item[i-2]);
          void *oldvalue = (void *)(item[i-1]);
          *addr = oldvalue;
      }
  }
#endif

  /* upon abort, set the reads size limit to 94% of how much was read
     so far.  This should ensure that, assuming the retry does the same
     thing, it will commit just before it reaches the conflicting point.
     Note that we should never *increase* the read length limit here. */
  limit = d->count_reads;
  if (limit > d->reads_size_limit_nonatomic) {  /* can occur if atomic */
      limit = d->reads_size_limit_nonatomic;
  }
  if (limit > 0) {
      limit -= (limit >> 4);
      d->reads_size_limit_nonatomic = limit;
  }

  gcptrlist_clear(&d->list_of_read_objects);
  stmgc_abort_transaction(d);

  fprintf(stderr, "[%lx] abort %d\n", (long)d->my_lock, num);
  if (num != ABRT_MANUAL && d->max_aborts >= 0 && !d->max_aborts--)
    {
      fprintf(stderr, "unexpected abort!\n");
      abort();
    }

  // notifies the CPU that we're potentially in a spin loop
  SpinLoop(SPLP_ABORT);
  // jump back to the setjmp_buf (this call does not return)
  d->active = 0;
  stm_stop_sharedlock();
  longjmp(*d->setjmp_buf, 1);
}

void AbortTransactionAfterCollect(struct tx_descriptor *d, int reason)
{
  if (d->active >= 0)
    {
      assert(d->active == 1);   /* not 2, which means inevitable */
      d->active = -reason;
    }
  assert(d->active < 0);
}

void AbortNowIfDelayed(void)
{
  struct tx_descriptor *d = thread_descriptor;
  if (d->active < 0)
    {
      int reason = -d->active;
      d->active = 1;
      AbortTransaction(reason);
    }
}

/************************************************************/

static void update_reads_size_limit(struct tx_descriptor *d)
{
  /* 'reads_size_limit' is set to ULONG_MAX if we are atomic; else
     we copy the value from reads_size_limit_nonatomic. */
  d->reads_size_limit = d->atomic ? ULONG_MAX : d->reads_size_limit_nonatomic;
}

static void init_transaction(struct tx_descriptor *d)
{
  assert(d->active == 0);
  stm_start_sharedlock();

  if (clock_gettime(CLOCK_MONOTONIC, &d->start_real_time) < 0) {
    d->start_real_time.tv_nsec = -1;
  }
  assert(d->list_of_read_objects.size == 0);
  stmgc_start_transaction(d);

  d->count_reads = 1;
  fxcache_clear(&d->recent_reads_cache);
#if 0
  gcptrlist_clear(&d->undolog);
#endif
  gcptrlist_clear(&d->abortinfo);
}

void BeginTransaction(jmp_buf* buf)
{
  struct tx_descriptor *d = thread_descriptor;
  init_transaction(d);
  d->active = 1;
  d->setjmp_buf = buf;
  d->start_time = GetGlobalCurTime(d);
  update_reads_size_limit(d);
}

static void AcquireLocks(struct tx_descriptor *d)
{
  revision_t my_lock = d->my_lock;
  wlog_t *item;

  if (!g2l_any_entry(&d->public_to_private))
    return;

  G2L_LOOP_FORWARD(d->public_to_private, item)
    {
      gcptr R = item->addr;
      revision_t v;
    retry:
      assert(R->h_tid & GCFLAG_OLD);
      v = ACCESS_ONCE(R->h_revision);
      if (!(v & 1))            // "is a pointer", i.e.
        {                      //   "has a more recent revision"
          assert(v != 0);
          AbortTransaction(ABRT_COMMIT);
        }
      if (v >= LOCKED)         // already locked by someone else
        {
          // we can always spinloop here: deadlocks should be impossible,
          // because G2L_LOOP_FORWARD should ensure a consistent ordering
          // of the R's.
          assert(v != my_lock);
          SpinLoop(SPLP_LOCKED_COMMIT);
          goto retry;
        }
      if (!bool_cas(&R->h_revision, v, my_lock))
        goto retry;

      gcptr L = item->val;
      assert(L->h_revision == stm_local_revision);
      assert(v != stm_local_revision);
      L->h_revision = v;   /* store temporarily this value here */

    } G2L_LOOP_END;
}

static void CancelLocks(struct tx_descriptor *d)
{
  wlog_t *item;

  if (!g2l_any_entry(&d->public_to_private))
    return;

  G2L_LOOP_FORWARD(d->public_to_private, item)
    {
      gcptr L = item->val;
      revision_t v = L->h_revision;
      if (v == stm_local_revision)
        break;    /* done */
      L->h_revision = stm_local_revision;

      gcptr R = item->addr;
#ifdef DUMP_EXTRA
      fprintf(stderr, "%p->h_revision = %p (CancelLocks)\n", R, (gcptr)v);
#endif
      ACCESS_ONCE(R->h_revision) = v;

    } G2L_LOOP_END;
}

static pthread_mutex_t mutex_prebuilt_gcroots = PTHREAD_MUTEX_INITIALIZER;

static void UpdateChainHeads(struct tx_descriptor *d, revision_t cur_time,
                             revision_t localrev)
{
  wlog_t *item;
  revision_t new_revision = cur_time + 1;     // make an odd number
  assert(new_revision & 1);

  if (!g2l_any_entry(&d->public_to_private))
    return;

  G2L_LOOP_FORWARD(d->public_to_private, item)
    {
      gcptr L = item->val;
      assert(!(L->h_tid & GCFLAG_VISITED));
      assert(!(L->h_tid & GCFLAG_PUBLIC_TO_PRIVATE));
      assert(!(L->h_tid & GCFLAG_PREBUILT_ORIGINAL));
      assert(!(L->h_tid & GCFLAG_NURSERY_MOVED));
      assert(!(L->h_tid & GCFLAG_STOLEN));
      assert(L->h_revision != localrev);   /* modified by AcquireLocks() */

#ifdef DUMP_EXTRA
      fprintf(stderr, "%p->h_revision = %p (UpdateChainHeads)\n",
              L, (gcptr)new_revision);
#endif
      L->h_revision = new_revision;

      if (is_young(L))
        {
          item->val = (gcptr)(((revision_t)L) | 2);
#ifdef DUMP_EXTRA
          fprintf(stderr, "PUBLIC-TO-PROTECTED:\n");
          /*mark*/
#endif
        }

    } G2L_LOOP_END;

  smp_wmb(); /* a memory barrier: make sure the new L->h_revisions are visible
                from other threads before we change the R->h_revisions */

  G2L_LOOP_FORWARD(d->public_to_private, item)
    {
      gcptr R = item->addr;
      revision_t v = (revision_t)item->val;

      assert(R->h_tid & GCFLAG_PUBLIC_TO_PRIVATE);
      assert(!(R->h_tid & GCFLAG_NURSERY_MOVED));
      assert(!(R->h_tid & GCFLAG_STOLEN));
      assert(!is_young(R));
      assert(R->h_revision != localrev);

#ifdef DUMP_EXTRA
      fprintf(stderr, "%p->h_revision = %p (UpdateChainHeads2)\n",
              R, (gcptr)v);
      /*mark*/
#endif
      ACCESS_ONCE(R->h_revision) = v;

      if (R->h_tid & GCFLAG_PREBUILT_ORIGINAL)
        {
          /* cannot possibly get here more than once for a given value of R */
          pthread_mutex_lock(&mutex_prebuilt_gcroots);
          gcptrlist_insert(&stm_prebuilt_gcroots, R);
          pthread_mutex_unlock(&mutex_prebuilt_gcroots);
          /*mark*/
        }

    } G2L_LOOP_END;

  g2l_clear(&d->public_to_private);
}

void CommitTransaction(void)
{   /* must save roots around this call */
  revision_t cur_time;
  struct tx_descriptor *d = thread_descriptor;
  assert(d->active >= 1);

  stmgc_stop_transaction(d);
  AcquireLocks(d);

  if (is_inevitable(d))
    {
      // no-one else can have changed global_cur_time if I'm inevitable
      cur_time = d->start_time;
      if (!bool_cas(&global_cur_time, cur_time + 1, cur_time + 2))
        {
          assert(!"global_cur_time modified even though we are inev.");
          abort();
        }
      inev_mutex_release();
    }
  else
    {
      while (1)
        {
          cur_time = ACCESS_ONCE(global_cur_time);
          if (cur_time & 1)
            {                    // there is another inevitable transaction
              CancelLocks(d);
              stmgc_suspend_commit_transaction(d);
              inev_mutex_acquire();   // wait until released
              inev_mutex_release();
              stmgc_stop_transaction(d);
              AcquireLocks(d);
              continue;
            }
          if (bool_cas(&global_cur_time, cur_time, cur_time + 2))
            break;
        }
      // validate (but skip validation if nobody else committed)
      if (cur_time != d->start_time)
        if (!ValidateDuringTransaction(d, 1))
          AbortTransaction(ABRT_VALIDATE_COMMIT);
    }
  /* we cannot abort any more from here */
  d->setjmp_buf = NULL;
  gcptrlist_clear(&d->list_of_read_objects);

  fprintf(stderr, "\n"
          "*************************************\n"
          "**************************************  committed %ld\n"
          "*************************************\n",
          (long)cur_time);

  revision_t localrev = stm_local_revision;
  revision_t newrev = -(cur_time + 1);
  assert(newrev & 1);
  ACCESS_ONCE(stm_local_revision) = newrev;

  UpdateChainHeads(d, cur_time, localrev);

  stmgc_committed_transaction(d);
  d->num_commits++;
  d->active = 0;
  stm_stop_sharedlock();
}

/************************************************************/

static void make_inevitable(struct tx_descriptor *d)
{
  d->setjmp_buf = NULL;
  d->active = 2;
  d->reads_size_limit_nonatomic = 0;
  update_reads_size_limit(d);
}

static revision_t acquire_inev_mutex_and_mark_global_cur_time(void)
{   /* must save roots around this call */
  revision_t cur_time;

  inev_mutex_acquire();
  while (1)
    {
      cur_time = ACCESS_ONCE(global_cur_time);
      assert((cur_time & 1) == 0);
      if (bool_cas(&global_cur_time, cur_time, cur_time + 1))
        break;
      /* else try again */
    }
  return cur_time;
}

void BecomeInevitable(const char *why)
{   /* must save roots around this call */
  revision_t cur_time;
  struct tx_descriptor *d = thread_descriptor;
  if (d == NULL || d->active != 1)
    return;  /* I am already inevitable, or not in a transaction at all
                (XXX statically we should know when we're outside
                a transaction) */

  fprintf(stderr, "[%lx] inevitable: %s\n", (long)d->my_lock, why);

  cur_time = acquire_inev_mutex_and_mark_global_cur_time();
  if (d->start_time != cur_time)
    {
      d->start_time = cur_time;
      if (!ValidateDuringTransaction(d, 0))
        {
          global_cur_time = cur_time;   // revert from (cur_time + 1)
          inev_mutex_release();
          AbortTransaction(ABRT_VALIDATE_INEV);
        }
    }
  make_inevitable(d);    /* cannot abort any more */
}

void BeginInevitableTransaction(void)
{   /* must save roots around this call */
  struct tx_descriptor *d = thread_descriptor;
  revision_t cur_time;

  init_transaction(d);
  cur_time = acquire_inev_mutex_and_mark_global_cur_time();
  d->start_time = cur_time;
  make_inevitable(d);
}

/************************************************************/

#if 0
static _Bool _PtrEq_Globals(gcptr G1, gcptr G2)
{
  /* This is a mess, because G1 and G2 can be different pointers to "the
     same" object, and it's hard to determine that.  Description of the
     idealized problem: we have chained lists following the 'h_revision'
     field from G1 and from G2, and we must return True if the chained
     lists end in the same object G and False if they don't.

     It's possible that G1 != G2 but G1->h_revision == G2->h_revision.
     More complicated cases are also possible.  This occurs because of
     random updates done in LatestGlobalRevision().  Note also that
     other threads can do concurrent updates.

     For now we simply use LatestGlobalRevision() and abort the current
     transaction if we see a too recent object.
  */
  struct tx_descriptor *d = thread_descriptor;
  if (G1->h_tid & GCFLAG_POSSIBLY_OUTDATED)
    G1 = LatestGlobalRevision(d, G1);
  if (G2->h_tid & GCFLAG_POSSIBLY_OUTDATED)
    G2 = LatestGlobalRevision(d, G2);
  return G1 == G2;
}

static _Bool _PtrEq_GlobalLocalCopy(gcptr G, gcptr L)
{
  struct tx_descriptor *d = thread_descriptor;
  wlog_t *entry;
  gcptr R;

  if (G->h_tid & GCFLAG_POSSIBLY_OUTDATED)
    R = LatestGlobalRevision(d, G);
  else
    R = G;

  G2L_FIND(d->global_to_local, R, entry, return 0);
  return L == entry->val;
}

_Bool stm_PtrEq(gcptr P1, gcptr P2)
{
  if (P1 == P2)
    return 1;
  else if (P1 == NULL || P2 == NULL)   /* and not P1 == P2 == NULL */
    return 0;

  if (P1->h_revision != stm_local_revision)
    {
      if (P2->h_revision != stm_local_revision)
        {
          /* P1 and P2 are two globals */
          return _PtrEq_Globals(P1, P2);
        }
      else if (P2->h_tid & GCFLAG_LOCAL_COPY)
        {
          /* P1 is a global, P2 is a local copy */
          return _PtrEq_GlobalLocalCopy(P1, P2);
        }
      else
        return 0;   /* P1 is global, P2 is new */
    }
  /* P1 is local, i.e. either new or a local copy */
  if (P2->h_revision != stm_local_revision)
    {
      if (P1->h_tid & GCFLAG_LOCAL_COPY)
        {
          /* P2 is a global, P1 is a local copy */
          return _PtrEq_GlobalLocalCopy(P2, P1);
        }
      else
        return 0;   /* P1 is new, P2 is global */
    }
  /* P1 and P2 are both locals (and P1 != P2) */
  return 0;
}
#endif
_Bool stm_PtrEq(gcptr P1, gcptr P2)
{
  abort();//XXX
}

/************************************************************/

#if 0
void stm_ThreadLocalRef_LLSet(void **addr, void *newvalue)
{
  struct tx_descriptor *d = thread_descriptor;
  gcptrlist_insert2(&d->undolog, (gcptr)addr, (gcptr)*addr);
  *addr = newvalue;
}
#endif

/************************************************************/

int DescriptorInit(void)
{
  if (GCFLAG_PREBUILT != PREBUILT_FLAGS)
    {
      fprintf(stderr, "fix PREBUILT_FLAGS in stmgc.h by giving "
                      "it the same value as GCFLAG_PREBUILT!\n");
      abort();
    }

  if (thread_descriptor == NULL)
    {
      struct tx_descriptor *d = stm_malloc(sizeof(struct tx_descriptor));
      memset(d, 0, sizeof(struct tx_descriptor));

      /* initialize 'my_lock' to be a unique odd number > LOCKED */
      while (1)
        {
          d->my_lock = ACCESS_ONCE(next_locked_value);
          if (d->my_lock > INTPTR_MAX - 2)
            {
              /* XXX fix this limitation */
              fprintf(stderr, "XXX error: too many threads ever created "
                              "in this process");
              abort();
            }
          if (bool_cas(&next_locked_value, d->my_lock, d->my_lock + 2))
            break;
        }
      assert(d->my_lock & 1);
      assert(d->my_lock > LOCKED);
      stm_local_revision = -d->my_lock;   /* a unique negative odd value */
      d->local_revision_ref = &stm_local_revision;
      d->max_aborts = -1;
      thread_descriptor = d;

      fprintf(stderr, "[%lx] pthread %lx starting\n",
              (long)d->my_lock, (long)pthread_self());

      return 1;
    }
  else
    return 0;
}

void DescriptorDone(void)
{
    struct tx_descriptor *d = thread_descriptor;
    assert(d != NULL);
    assert(d->active == 0);

    thread_descriptor = NULL;

    gcptrlist_delete(&d->list_of_read_objects);
    gcptrlist_delete(&d->abortinfo);
    free(d->longest_abort_info);
#if 0
    gcptrlist_delete(&d->undolog);
#endif

    int num_aborts = 0, num_spinloops = 0;
    int i;
    char line[256], *p = line;

    for (i=0; i<ABORT_REASONS; i++)
        num_aborts += d->num_aborts[i];
    for (i=0; i<SPINLOOP_REASONS; i++)
        num_spinloops += d->num_spinloops[i];

    p += sprintf(p, "[%lx] finishing: %d commits, %d aborts ",
                 (long)d->my_lock,
                 d->num_commits,
                 num_aborts);

    for (i=0; i<ABORT_REASONS; i++)
        p += sprintf(p, "%c%d", i == 0 ? '[' : ',',
                     d->num_aborts[i]);

    for (i=1; i<SPINLOOP_REASONS; i++)  /* num_spinloops[0] == num_aborts */
        p += sprintf(p, "%c%d", i == 1 ? '|' : ',',
                     d->num_spinloops[i]);

    p += sprintf(p, "]\n");
    fwrite(line, 1, p - line, stderr);

    stm_free(d, sizeof(struct tx_descriptor));
}
