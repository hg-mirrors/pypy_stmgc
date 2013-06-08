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

/* a negative odd number that identifies the currently running
   transaction within the thread. */
__thread revision_t stm_private_rev_num;


revision_t stm_global_cur_time(void)  /* for tests */
{
  return global_cur_time;
}
revision_t get_private_rev_num(void)        /* for tests */
{
  return stm_private_rev_num;
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

#if 0
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
#endif

static void steal(gcptr P)
{
  struct tx_public_descriptor *foreign_pd;
  revision_t target_descriptor_index;
  revision_t v = ACCESS_ONCE(P->h_revision);
  if ((v & 3) != 2)
    return;
  target_descriptor_index = *(revision_t *)(v & ~(HANDLE_BLOCK_SIZE-1));
  //foreign_pd = ACCESS_ONCE(stm_descriptor_array[target_descriptor_index]);
  abort();
}

gcptr stm_DirectReadBarrier(gcptr G)
{
  struct tx_descriptor *d = thread_descriptor;
  gcptr P = G;
  revision_t v;

  if (P->h_tid & GCFLAG_PUBLIC)
    {
      /* follow the chained list of h_revision's as long as they are
         regular pointers */
    retry:
      v = ACCESS_ONCE(P->h_revision);
      if (!(v & 1))  // "is a pointer", i.e.
        {            //      "has a more recent revision"
          if (v & 2)
            goto old_to_young;
          assert(P->h_tid & GCFLAG_PUBLIC);

          gcptr P_prev = P;
          P = (gcptr)v;

          /* if we land on a P in read_barrier_cache: just return it */
          if (FXCACHE_AT(P) == P)
            {
              fprintf(stderr, "read_barrier: %p -> %p fxcache\n", G, P);
              return P;
            }

          v = ACCESS_ONCE(P->h_revision);
          if (!(v & 1))  // "is a pointer", i.e.
            {            //      "has a more recent revision"
              if (v & 2)
                goto old_to_young;
              assert(P->h_tid & GCFLAG_PUBLIC);

              /* we update P_prev->h_revision as a shortcut */
              /* XXX check if this really gives a worse performance than only
                 doing this write occasionally based on a counter in d */
              P_prev->h_revision = v;
              P = (gcptr)v;
              goto retry;
            }
        }

      if (P->h_tid & GCFLAG_PUBLIC_TO_PRIVATE)
        {
          wlog_t *item;
          G2L_FIND(d->public_to_private, P, item, goto no_private_obj);

          P = item->val;
          assert(!(P->h_tid & GCFLAG_PUBLIC));
          assert(P->h_revision == stm_private_rev_num);
          fprintf(stderr, "read_barrier: %p -> %p public_to_private\n", G, P);
          return P;
        }
    no_private_obj:

      if (UNLIKELY(v > d->start_time))   // object too recent?
        {
          if (v >= LOCKED)
            {
              SpinLoop(SPLP_LOCKED_INFLIGHT);
              goto retry;           // spinloop until it is no longer LOCKED
            }
          ValidateNow(d);                  // try to move start_time forward
          goto retry;                      // restart searching from P
        }
      fprintf(stderr, "read_barrier: %p -> %p public\n", G, P);
    }
  else
    {
      fprintf(stderr, "read_barrier: %p -> %p protected\n", G, P);
    }

 register_in_list_of_read_objects:
  fxcache_add(&d->recent_reads_cache, P);
  gcptrlist_insert(&d->list_of_read_objects, P);
  return P;

 old_to_young:;
  revision_t target_descriptor_index;
  target_descriptor_index = *(revision_t *)(v & ~(HANDLE_BLOCK_SIZE-1));
  if (target_descriptor_index == d->public_descriptor_index)
    {
      P = (gcptr)(*(revision_t *)(v - 2));
      assert(!(P->h_tid & GCFLAG_PUBLIC));
      if (P->h_revision == stm_private_rev_num)
        {
          fprintf(stderr, "read_barrier: %p -> %p handle "
                  "private\n", G, P);
          return P;
        }
      else if (FXCACHE_AT(P) == P)
        {
          fprintf(stderr, "read_barrier: %p -> %p handle "
                  "protected fxcache\n", G, P);
          return P;
        }
      else
        {
          fprintf(stderr, "read_barrier: %p -> %p handle "
                  "protected\n", G, P);
          goto register_in_list_of_read_objects;
        }
    }
  else
    {
      /* stealing */
      fprintf(stderr, "read_barrier: %p -> stealing %p...", G, (gcptr)v);
      steal(P);
      goto retry;
    }
}

#if 0
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
  struct tx_descriptor *d = thread_descriptor;
  if (gcptrlist_size(&d->stolen_objects) > 0)
    stmgc_normalize_stolen_objects();

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
#endif

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

gcptr stmgc_duplicate(gcptr P)
{
  size_t size = stmcb_size(P);
  gcptr L = stm_malloc(size);
  memcpy(L, P, size);
  return L;
}

static gcptr LocalizeProtected(struct tx_descriptor *d, gcptr P)
{
  gcptr B;

  assert(P->h_revision != stm_private_rev_num);
  assert(!(P->h_tid & GCFLAG_PUBLIC));
  assert(!(P->h_tid & GCFLAG_PUBLIC_TO_PRIVATE));
  assert(!(P->h_tid & GCFLAG_BACKUP_COPY));
  assert(!(P->h_tid & GCFLAG_STUB));

  if (P->h_revision & 1)
    {
      /* does not have a backup yet */
      B = stmgc_duplicate(P);
      B->h_tid |= GCFLAG_BACKUP_COPY;
    }
  else
    {
      size_t size = stmcb_size(P);
      B = (gcptr)P->h_revision;
      assert(B->h_tid & GCFLAG_BACKUP_COPY);
      memcpy(B + 1, P + 1, size - sizeof(*B));
    }
  assert(B->h_tid & GCFLAG_BACKUP_COPY);
  gcptrlist_insert2(&d->public_descriptor->active_backup_copies, P, B);
  P->h_revision = stm_private_rev_num;
  return P;
}

static gcptr LocalizePublic(struct tx_descriptor *d, gcptr R)
{
  assert(R->h_tid & GCFLAG_PUBLIC);
  if (R->h_tid & GCFLAG_PUBLIC_TO_PRIVATE)
    {
      wlog_t *entry;
      gcptr L;
      G2L_FIND(d->public_to_private, R, entry, goto not_found);
      L = entry->val;
      assert(L->h_revision == stm_private_rev_num);   /* private object */
      return L;
    }
  R->h_tid |= GCFLAG_PUBLIC_TO_PRIVATE;

 not_found:;
  gcptr L = stmgc_duplicate(R);
  assert(!(L->h_tid & GCFLAG_BACKUP_COPY));
  assert(!(L->h_tid & GCFLAG_STOLEN));
  assert(!(L->h_tid & GCFLAG_STUB));
  L->h_tid &= ~(GCFLAG_OLD               |
                GCFLAG_VISITED           |
                GCFLAG_PUBLIC            |
                GCFLAG_PREBUILT_ORIGINAL |
                GCFLAG_PUBLIC_TO_PRIVATE |
                GCFLAG_WRITE_BARRIER     |
                0);
  L->h_revision = stm_private_rev_num;
  g2l_insert(&d->public_to_private, R, L);
  fprintf(stderr, "write_barrier: adding %p -> %p to public_to_private\n",
          R, L);

  /* must remove R from the read_barrier_cache, because returning R is no
     longer a valid result */
  fxcache_remove(&d->recent_reads_cache, R);

  return L;
}

gcptr stm_WriteBarrier(gcptr P)
{
  gcptr W;
  struct tx_descriptor *d = thread_descriptor;
  assert(d->active >= 1);

  P = stm_read_barrier(P);

  if (P->h_tid & GCFLAG_PUBLIC)
    W = LocalizePublic(d, P);
  else
    W = LocalizeProtected(d, P);

  fprintf(stderr, "write_barrier: %p -> %p\n", P, W);

  return W;
}

gcptr stm_get_backup_copy(gcptr P)
{
  struct tx_public_descriptor *pd = thread_descriptor->public_descriptor;
  long i, size = pd->active_backup_copies.size;
  gcptr *items = pd->active_backup_copies.items;
  for (i = 0; i < size; i += 2)
    if (items[i] == P)
      return items[i + 1];
  return NULL;
}

gcptr stm_get_read_obj(long index)
{
  struct tx_descriptor *d = thread_descriptor;
  if (index < gcptrlist_size(&d->list_of_read_objects))
    return d->list_of_read_objects.items[index];
  return NULL;
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
  abort();
#if 0
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
          /* ... unless it's a protected-to-private link */
          if (((gcptr)v)->h_revision == stm_local_revision)
            continue;
          /* ... or unless it is a GCFLAG_STOLEN object */
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
#endif
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
  gcptrlist_clear(&d->public_descriptor->active_backup_copies);
  abort();//stmgc_abort_transaction(d);

  fprintf(stderr,
          "\n"
          "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
          "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
          "!!!!!!!!!!!!!!!!!!!!!  [%lx] abort %d\n"
          "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
          "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
          "\n", (long)d->public_descriptor_index, num);
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
  assert(d->public_descriptor->active_backup_copies.size == 0);
  assert(!g2l_any_entry(&d->public_to_private));

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
      assert(L->h_revision == stm_private_rev_num);
      assert(v != stm_private_rev_num);
      L->h_revision = v;   /* store temporarily this value here */

    } G2L_LOOP_END;
}

static void CancelLocks(struct tx_descriptor *d)
{
  abort();
#if 0
  revision_t my_lock = d->my_lock;
  wlog_t *item;

  if (!g2l_any_entry(&d->public_to_private))
    return;

  G2L_LOOP_FORWARD(d->public_to_private, item)
    {
      gcptr R = item->addr;
      gcptr L = item->val;
      revision_t v = L->h_revision;
      if (v == stm_local_revision)
        {
          assert(R->h_revision != my_lock);
          break;    /* done */
        }
      L->h_revision = stm_local_revision;

#ifdef DUMP_EXTRA
      fprintf(stderr, "%p->h_revision = %p (CancelLocks)\n", R, (gcptr)v);
#endif
      assert(R->h_revision == my_lock);
      ACCESS_ONCE(R->h_revision) = v;

    } G2L_LOOP_END;
#endif
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

    } G2L_LOOP_END;

  smp_wmb(); /* a memory barrier: make sure the new L->h_revisions are visible
                from other threads before we change the R->h_revisions */

  G2L_LOOP_FORWARD(d->public_to_private, item)
    {
      gcptr R = item->addr;
      revision_t v = (revision_t)item->val;

      assert(R->h_tid & GCFLAG_PUBLIC);
      assert(R->h_tid & GCFLAG_PUBLIC_TO_PRIVATE);
      assert(!(R->h_tid & GCFLAG_NURSERY_MOVED));
      assert(!(R->h_tid & GCFLAG_STOLEN));
      assert(R->h_revision != localrev);

      /* XXX compactify and don't leak! */
      revision_t *handle_block = stm_malloc(3 * WORD);
      handle_block = (revision_t *)
        ((((intptr_t)handle_block) + HANDLE_BLOCK_SIZE-1)
         & ~(HANDLE_BLOCK_SIZE-1));
      handle_block[0] = d->public_descriptor_index;
      handle_block[1] = v;

      revision_t w = ((revision_t)(handle_block + 1)) + 2;

#ifdef DUMP_EXTRA
      fprintf(stderr, "%p->h_revision = %p (UpdateChainHeads2)\n",
              R, (gcptr)w);
      /*mark*/
#endif
      ACCESS_ONCE(R->h_revision) = w;

#if 0
      if (R->h_tid & GCFLAG_PREBUILT_ORIGINAL)
        {
          /* cannot possibly get here more than once for a given value of R */
          pthread_mutex_lock(&mutex_prebuilt_gcroots);
          gcptrlist_insert(&stm_prebuilt_gcroots, R);
          pthread_mutex_unlock(&mutex_prebuilt_gcroots);
          /*mark*/
        }
#endif

    } G2L_LOOP_END;

  g2l_clear(&d->public_to_private);
}

#if 0
void UpdateProtectedChainHeads(struct tx_descriptor *d, revision_t cur_time,
                               revision_t localrev)
{
  revision_t new_revision = cur_time + 1;     // make an odd number
  assert(new_revision & 1);

  long i, size = d->protected_with_private_copy.size;
  gcptr *items = d->protected_with_private_copy.items;
  for (i = 0; i < size; i++)
    {
      gcptr R = items[i];
      if (R->h_tid & GCFLAG_STOLEN)       /* ignore stolen objects */
        continue;
      gcptr L = (gcptr)R->h_revision;
      assert(L->h_revision == localrev);
      L->h_revision = new_revision;
    }
}
#endif

void TurnPrivateWithBackupToProtected(struct tx_descriptor *d,
                                      revision_t cur_time)
{
  long i, size = d->public_descriptor->active_backup_copies.size;
  gcptr *items = d->public_descriptor->active_backup_copies.items;

  for (i = 0; i < size; i += 2)
    {
      gcptr P = items[i];
      gcptr B = items[i + 1];
      assert(P->h_revision == stm_private_rev_num);
      assert(B->h_tid & GCFLAG_BACKUP_COPY);
      B->h_revision = cur_time;
      P->h_revision = (revision_t)B;
    };
  gcptrlist_clear(&d->public_descriptor->active_backup_copies);
}

void CommitTransaction(void)
{   /* must save roots around this call */
  revision_t cur_time;
  struct tx_descriptor *d = thread_descriptor;
  assert(d->active >= 1);

  spinlock_acquire(d->public_descriptor->collection_lock, 'C');  /*committing*/
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
              spinlock_release(d->public_descriptor->collection_lock);
              inev_mutex_acquire();   // wait until released
              inev_mutex_release();
              spinlock_acquire(d->public_descriptor->collection_lock, 'C');
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

  TurnPrivateWithBackupToProtected(d, cur_time);

  revision_t localrev = stm_private_rev_num;
  //UpdateProtectedChainHeads(d, cur_time, localrev);
  //smp_wmb();

  revision_t newrev = -(cur_time + 1);
  assert(newrev & 1);
  ACCESS_ONCE(stm_private_rev_num) = newrev;
  fprintf(stderr, "%p: stm_local_revision = %ld\n", d, (long)newrev);
  assert(d->private_revision_ref = &stm_private_rev_num);

  UpdateChainHeads(d, cur_time, localrev);

  spinlock_release(d->public_descriptor->collection_lock);
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

  fprintf(stderr, "[%lx] inevitable: %s\n",
          (long)d->public_descriptor_index, why);

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

struct tx_public_descriptor *stm_descriptor_array[MAX_THREADS] = {0};
static revision_t descriptor_array_free_list = 0;
static revision_t descriptor_array_lock = 0;

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
      revision_t i;
      struct tx_descriptor *d = stm_malloc(sizeof(struct tx_descriptor));
      memset(d, 0, sizeof(struct tx_descriptor));
      spinlock_acquire(descriptor_array_lock, 1);

      struct tx_public_descriptor *pd;
      i = descriptor_array_free_list;
      pd = stm_descriptor_array[i];
      if (pd != NULL) {
          /* we are reusing 'pd' */
          descriptor_array_free_list = pd->free_list_next;
          assert(descriptor_array_free_list >= 0);
      }
      else {
          /* no item in the free list */
          descriptor_array_free_list = i + 1;
          if (descriptor_array_free_list == MAX_THREADS) {
              fprintf(stderr, "error: too many threads at the same time "
                              "in this process");
              abort();
          }
          pd = stm_malloc(sizeof(struct tx_public_descriptor));
          memset(pd, 0, sizeof(struct tx_public_descriptor));
          stm_descriptor_array[i] = pd;
      }
      pd->free_list_next = -1;

      d->public_descriptor = pd;
      d->public_descriptor_index = i;
      d->my_lock = LOCKED + 2 * i;
      assert(d->my_lock & 1);
      assert(d->my_lock >= LOCKED);
      stm_private_rev_num = -1;
      d->private_revision_ref = &stm_private_rev_num;
      d->max_aborts = -1;
      thread_descriptor = d;

      fprintf(stderr, "[%lx] pthread %lx starting\n",
              (long)d->public_descriptor_index, (long)pthread_self());

      spinlock_release(descriptor_array_lock);
      return 1;
    }
  else
    return 0;
}

void DescriptorDone(void)
{
    revision_t i;
    struct tx_descriptor *d = thread_descriptor;
    assert(d != NULL);
    assert(d->active == 0);

    d->public_descriptor->collection_lock = 0;    /* unlock */

    spinlock_acquire(descriptor_array_lock, 1);
    i = d->public_descriptor_index;
    assert(stm_descriptor_array[i] == d->public_descriptor);
    d->public_descriptor->free_list_next = descriptor_array_free_list;
    descriptor_array_free_list = i;
    spinlock_release(descriptor_array_lock);

    thread_descriptor = NULL;

    g2l_delete(&d->public_to_private);
    assert(d->public_descriptor->active_backup_copies.size == 0);
    gcptrlist_delete(&d->public_descriptor->active_backup_copies);
    gcptrlist_delete(&d->list_of_read_objects);
    gcptrlist_delete(&d->abortinfo);
    free(d->longest_abort_info);
#if 0
    gcptrlist_delete(&d->undolog);
#endif

    int num_aborts = 0, num_spinloops = 0;
    char line[256], *p = line;

    for (i=0; i<ABORT_REASONS; i++)
        num_aborts += d->num_aborts[i];
    for (i=0; i<SPINLOOP_REASONS; i++)
        num_spinloops += d->num_spinloops[i];

    p += sprintf(p, "[%lx] finishing: %d commits, %d aborts ",
                 (long)d->public_descriptor_index,
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
