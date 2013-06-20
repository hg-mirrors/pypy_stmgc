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
static int is_private(gcptr P)
{
  return (P->h_revision == stm_private_rev_num) ||
    (P->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED);
}
int _stm_is_private(gcptr P)
{
  return is_private(P);
}
void stm_clear_read_cache(void)
{
  fxcache_clear(&thread_descriptor->recent_reads_cache);
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

gcptr stm_DirectReadBarrier(gcptr G)
{
  struct tx_descriptor *d = thread_descriptor;
  gcptr P = G;
  revision_t v;

 restart_all:
  if (P->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED)
    {
      assert(IS_POINTER(P->h_revision));   /* pointer to the backup copy */

      /* check P->h_revision->h_revision: if a pointer, then it means
         the backup copy has been stolen into a public object and then
         modified by some other thread.  Abort. */
      if (IS_POINTER(((gcptr)P->h_revision)->h_revision))
        AbortTransaction(ABRT_STOLEN_MODIFIED);

      goto add_in_recent_reads_cache;
    }
  /* else, for the rest of this function, we can assume that P was not
     a private copy */

  if (P->h_tid & GCFLAG_PUBLIC)
    {
      /* follow the chained list of h_revision's as long as they are
         regular pointers.  We will only find more public objects
         along this chain.
      */
    restart_all_public:
      assert(P->h_tid & GCFLAG_PUBLIC);
      v = ACCESS_ONCE(P->h_revision);
      if (IS_POINTER(v))  /* "is a pointer", "has a more recent revision" */
        {
        retry:
          if (v & 2)
            goto follow_stub;

          gcptr P_prev = P;
          P = (gcptr)v;
          assert((P->h_tid & GCFLAG_PUBLIC) ||
                 (P_prev->h_tid & GCFLAG_NURSERY_MOVED));

          v = ACCESS_ONCE(P->h_revision);

          if (IS_POINTER(v))
            {
              if (v & 2)
                goto follow_stub;

              /* we update P_prev->h_revision as a shortcut */
              /* XXX check if this really gives a worse performance than only
                 doing this write occasionally based on a counter in d */
              P_prev->h_revision = v;
              P = (gcptr)v;
              v = ACCESS_ONCE(P->h_revision);
              if (IS_POINTER(v))
                goto retry;
            }

          /* We reach this point if P != G only.  Check again the
             read_barrier_cache: if P now hits the cache, just return it
          */
          if (FXCACHE_AT(P) == P)
            {
              fprintf(stderr, "read_barrier: %p -> %p fxcache\n", G, P);
              return P;
            }
        }

      /* If we land on a P with GCFLAG_PUBLIC_TO_PRIVATE, it might be
         because *we* have an entry in d->public_to_private.  (It might
         also be someone else.)
      */
      if (P->h_tid & GCFLAG_PUBLIC_TO_PRIVATE)
        {
          wlog_t *item;
        retry_public_to_private:;
          G2L_FIND(d->public_to_private, P, item, goto no_private_obj);

          /* We have a key in 'public_to_private'.  The value is the
             corresponding private object. */
          P = item->val;
          assert(!(P->h_tid & GCFLAG_PUBLIC));
          assert(is_private(P));
          fprintf(stderr, "read_barrier: %p -> %p public_to_private\n", G, P);
          return P;

        no_private_obj:
          /* Key not found.  It might be because there really is none, or
             because we still have it waiting in 'stolen_objects'. */
          if (d->public_descriptor->stolen_objects.size > 0)
            {
              spinlock_acquire(d->public_descriptor->collection_lock, 'N');
              stm_normalize_stolen_objects(d);
              spinlock_release(d->public_descriptor->collection_lock);
              goto retry_public_to_private;
            }
        }

      /* The answer is a public object.  Is it too recent? */
      if (UNLIKELY(v > d->start_time))
        {
          if (v >= LOCKED)
            {
              SpinLoop(SPLP_LOCKED_INFLIGHT);
              goto restart_all_public; // spinloop until it is no longer LOCKED
            }
          ValidateNow(d);                  // try to move start_time forward
          goto restart_all_public;         // restart searching from P
        }
      fprintf(stderr, "read_barrier: %p -> %p public\n", G, P);
    }
  else
    {
      /* Not private and not public: it's a protected object
       */
      fprintf(stderr, "read_barrier: %p -> %p protected\n", G, P);

      /* The risks are not high, but in parallel it's possible for the
         object to be stolen by another thread and become public, after
         which it can be outdated by another commit.  So the following
         assert can actually fail in that case. */
      /*assert(P->h_revision & 1);*/
    }

  fprintf(stderr, "readobj: %p\n", P);
  gcptrlist_insert(&d->list_of_read_objects, P);

 add_in_recent_reads_cache:
  /* The risks are that the following assert fails, because the flag was
     added just now by a parallel thread during stealing... */
  /*assert(!(P->h_tid & GCFLAG_NURSERY_MOVED));*/
  fxcache_add(&d->recent_reads_cache, P);
  return P;

 follow_stub:;
  /* We know that P is a stub object, because only stubs can have
     an h_revision that is == 2 mod 4.
  */
  struct tx_public_descriptor *foreign_pd = STUB_THREAD(P);
  if (foreign_pd == d->public_descriptor)
    {
      /* Same thread: dereference the pointer directly.  It's possible
         we reach any kind of object, even a public object, in case it
         was stolen.  So we just repeat the whole procedure. */
      P = (gcptr)(v - 2);
      fprintf(stderr, "read_barrier: %p -> %p via stub\n  ", G, P);

      if (UNLIKELY((P->h_revision != stm_private_rev_num) &&
                   (FXCACHE_AT(P) != P)))
        goto restart_all;

      return P;
    }
  else
    {
      /* stealing */
      fprintf(stderr, "read_barrier: %p -> stealing %p...\n  ", G, P);
      stm_steal_stub(P);

      assert(P->h_tid & GCFLAG_PUBLIC);
      goto restart_all_public;
    }
}

static gcptr _match_public_to_private(gcptr P, gcptr pubobj, gcptr privobj,
                                      int from_stolen)
{
  gcptr org_pubobj = pubobj;
  while ((pubobj->h_revision & 3) == 0)
    {
      assert(pubobj != P);
      pubobj = (gcptr)pubobj->h_revision;
    }
  if (pubobj == P || ((P->h_revision & 3) == 2 &&
                      pubobj->h_revision == P->h_revision))
    {
      assert(!(org_pubobj->h_tid & GCFLAG_STUB));
      assert(!(privobj->h_tid & GCFLAG_PUBLIC));
      assert(is_private(privobj));
      if (P != org_pubobj)
        fprintf(stderr, "| actually %p ", org_pubobj);
      if (from_stolen)
        fprintf(stderr, "-stolen");
      else
        assert(org_pubobj->h_tid & GCFLAG_PUBLIC_TO_PRIVATE);
      fprintf(stderr, "-public_to_private-> %p private\n", privobj);
      return privobj;
    }
  return NULL;
}

static gcptr _find_public_to_private(gcptr P)
{
  gcptr R;
  wlog_t *item;
  struct tx_descriptor *d = thread_descriptor;

  G2L_LOOP_FORWARD(d->public_to_private, item)
    {
      assert(item->addr->h_tid & GCFLAG_PUBLIC_TO_PRIVATE);
      R = _match_public_to_private(P, item->addr, item->val, 0);
      if (R != NULL)
        return R;

    } G2L_LOOP_END;

  long i, size = d->public_descriptor->stolen_objects.size;
  gcptr *items = d->public_descriptor->stolen_objects.items;

  for (i = 0; i < size; i += 2)
    {
      if (items[i + 1] == NULL)
        continue;
      R = _match_public_to_private(P, items[i], items[i + 1], 1);
      if (R != NULL)
        return R;
    }

  return NULL;
}

static void _check_flags(gcptr P)
{
  struct tx_descriptor *d = thread_descriptor;
  if (P->h_tid & GCFLAG_STUB)
    {
      fprintf(stderr, "S");
    }
  int is_old = (P->h_tid & GCFLAG_OLD) != 0;
  int in_nurs = (d->nursery_base <= (char*)P && ((char*)P) < d->nursery_end);
  if (in_nurs)
    {
      assert(!is_old);
      fprintf(stderr, "Y ");
    }
  else
    {
      assert(is_old);
      fprintf(stderr, "O ");
    }
}

gcptr _stm_nonrecord_barrier(gcptr P)
{
  /* follows the logic in stm_DirectReadBarrier() */
  struct tx_descriptor *d = thread_descriptor;
  revision_t v;

  fprintf(stderr, "_stm_nonrecord_barrier: %p ", P);
  _check_flags(P);

 restart_all:
  if (P->h_revision == stm_private_rev_num)
    {
      /* private */
      fprintf(stderr, "private\n");
      return P;
    }

  if (P->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED)
    {
      /* private too, with a backup copy */
      assert(IS_POINTER(P->h_revision));
      fprintf(stderr, "private_from_protected\n");
      return P;
    }

  if (P->h_tid & GCFLAG_PUBLIC)
    {
      fprintf(stderr, "public ");

      while (v = P->h_revision, IS_POINTER(v))
        {
          if (P->h_tid & GCFLAG_NURSERY_MOVED)
            fprintf(stderr, "nursery_moved ");

          if (v & 2)
            {
              fprintf(stderr, "stub ");
              gcptr L = _find_public_to_private(P);
              if (L != NULL)
                return L;
              goto follow_stub;
            }

          P = (gcptr)v;
          assert(P->h_tid & GCFLAG_PUBLIC);
          fprintf(stderr, "-> %p public ", P);
          _check_flags(P);
        }

      gcptr L = _find_public_to_private(P);
      if (L != NULL)
        return L;

      if (UNLIKELY(v > d->start_time))
        {
          fprintf(stderr, "too recent!\n");
          return NULL;   // object too recent
        }
      fprintf(stderr, "\n");
    }
  else
    {
      fprintf(stderr, "protected\n");
    }
  return P;

 follow_stub:;
  if (STUB_THREAD(P) == d->public_descriptor)
    {
      P = (gcptr)(v - 2);
      fprintf(stderr, "-> %p ", P);
      _check_flags(P);
    }
  else
    {
      P = (gcptr)(v - 2);
      /* cannot _check_flags(P): foreign! */
      fprintf(stderr, "-foreign-> %p ", P);
      if (P->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED)
        {
          P = (gcptr)P->h_revision;     /* the backup copy */
          /* cannot _check_flags(P): foreign! */
          fprintf(stderr, "-backup-> %p ", P);
        }
      if (!(P->h_tid & GCFLAG_PUBLIC))
        {
          fprintf(stderr, "protected by someone else!\n");
          return (gcptr)-1;
        }
    }
  /* cannot _check_flags(P): foreign! */
  goto restart_all;
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

static gcptr LocalizeProtected(struct tx_descriptor *d, gcptr P)
{
  gcptr B;

  assert(P->h_revision != stm_private_rev_num);
  assert(P->h_revision & 1);
  assert(!(P->h_tid & GCFLAG_PUBLIC_TO_PRIVATE));
  assert(!(P->h_tid & GCFLAG_BACKUP_COPY));
  assert(!(P->h_tid & GCFLAG_STUB));
  assert(!(P->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED));

  B = stmgc_duplicate_old(P);
  B->h_tid |= GCFLAG_BACKUP_COPY;

  P->h_tid |= GCFLAG_PRIVATE_FROM_PROTECTED;
  P->h_revision = (revision_t)B;

  gcptrlist_insert(&d->private_from_protected, P);
  fprintf(stderr, "private_from_protected: insert %p (backup %p)\n", P, B);

  return P;   /* always returns its arg: the object is converted in-place */
}

static gcptr LocalizePublic(struct tx_descriptor *d, gcptr R)
{
  assert(R->h_tid & GCFLAG_PUBLIC);
  assert(!(R->h_tid & GCFLAG_NURSERY_MOVED));

#ifdef _GC_DEBUG
  wlog_t *entry;
  G2L_FIND(d->public_to_private, R, entry, goto not_found);
  assert(!"R is already in public_to_private");
 not_found:
#endif

  R->h_tid |= GCFLAG_PUBLIC_TO_PRIVATE;

  /* note that stmgc_duplicate() usually returns a young object, but may
     return an old one if the nursery is full at this moment. */
  gcptr L = stmgc_duplicate(R);
  assert(!(L->h_tid & GCFLAG_BACKUP_COPY));
  assert(!(L->h_tid & GCFLAG_STUB));
  assert(!(L->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED));
  L->h_tid &= ~(GCFLAG_VISITED           |
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

static inline void record_write_barrier(gcptr P)
{
  if (P->h_tid & GCFLAG_WRITE_BARRIER)
    {
      P->h_tid &= ~GCFLAG_WRITE_BARRIER;
      gcptrlist_insert(&thread_descriptor->old_objects_to_trace, P);
    }
}

gcptr stm_WriteBarrier(gcptr P)
{
  if (is_private(P))
    {
      /* If we have GCFLAG_WRITE_BARRIER in P, then list it into
         old_objects_to_trace: it's a private object that may be
         modified by the program after we return, and the mutation may
         be to write young pointers (in fact it's a common case).
      */
      record_write_barrier(P);
      return P;
    }

  gcptr R, W;
  R = stm_read_barrier(P);

  if (is_private(R))
    {
      record_write_barrier(R);
      return R;
    }

  struct tx_descriptor *d = thread_descriptor;
  assert(d->active >= 1);

  /* We need the collection_lock for the sequel; this is required notably
     because we're about to edit flags on a protected object.
  */
  spinlock_acquire(d->public_descriptor->collection_lock, 'L');
  if (d->public_descriptor->stolen_objects.size != 0)
    stm_normalize_stolen_objects(d);

  if (R->h_tid & GCFLAG_PUBLIC)
    {
      /* Make and return a new (young) private copy of the public R.
         Add R into the list 'public_with_young_copy', unless W is
         actually an old object, in which case we need to record W.
      */
      if (R->h_tid & GCFLAG_NURSERY_MOVED)
        {
          /* Bah, the object turned into this kind of stub, possibly
             while we were waiting for the collection_lock, because it
             was stolen by someone else.  Use R->h_revision instead. */
          assert(IS_POINTER(R->h_revision));
          R = (gcptr)R->h_revision;
          assert(R->h_tid & GCFLAG_PUBLIC);
        }
      assert(R->h_tid & GCFLAG_OLD);
      W = LocalizePublic(d, R);
      assert(is_private(W));

      if (W->h_tid & GCFLAG_OLD)
        {
          W->h_tid |= GCFLAG_WRITE_BARRIER;
          record_write_barrier(W);
        }
      else
        gcptrlist_insert(&d->public_with_young_copy, R);
    }
  else
    {
      /* Turn the protected copy in-place into a private copy.  If it's
         an old object that still has GCFLAG_WRITE_BARRIER, then we must
         also record it in the list 'old_objects_to_trace'. */
      W = LocalizeProtected(d, R);
      assert(is_private(W));
      record_write_barrier(W);
    }

  spinlock_release(d->public_descriptor->collection_lock);

  fprintf(stderr, "write_barrier: %p -> %p -> %p\n", P, R, W);

  return W;
}

gcptr stm_get_private_from_protected(long index)
{
  struct tx_descriptor *d = thread_descriptor;
  if (index < gcptrlist_size(&d->private_from_protected))
    return d->private_from_protected.items[index];
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
  long i, size = d->list_of_read_objects.size;
  gcptr *items = d->list_of_read_objects.items;

  for (i=0; i<size; i++)
    {
      gcptr R = items[i];
      revision_t v;
    retry:
      v = ACCESS_ONCE(R->h_revision);
      if (IS_POINTER(v))  /* "is a pointer", i.e. has a more recent revision */
        {
          if (R->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED)
            {
              /* such an object R might be listed in list_of_read_objects
                 before it was turned from protected to private */
              continue;
            }
          else if ((R->h_tid & (GCFLAG_PUBLIC | GCFLAG_NURSERY_MOVED))
                            == (GCFLAG_PUBLIC | GCFLAG_NURSERY_MOVED))
            {
              /* such an object is identical to the one it points to */
              R = (gcptr)v;
              goto retry;
            }
          else
            {
              fprintf(stderr, "validation failed: "
                      "%p has a more recent revision\n", R);
              return 0;
            }
        }
      if (v >= LOCKED)            // locked
        {
          if (!during_commit)
            {
              assert(v != d->my_lock);    // we don't hold any lock
              /* spinloop until the other thread releases its lock */
              SpinLoop(SPLP_LOCKED_VALIDATE);
              goto retry;
            }
          else
            {
              if (v != d->my_lock)         // not locked by me: conflict
                {
                  /* A case that can occur: two threads A and B are both
                     committing, thread A locked object a, thread B
                     locked object b, and then thread A tries to
                     validate the reads it did on object b and
                     vice-versa.  In this case both threads cannot
                     commit, but if they both enter the SpinLoop()
                     above, then they will livelock.

                     XXX This might lead both threads to cancel by
                     reaching this point.  It might be possible to be
                     more clever and let one of the threads commit
                     anyway.
                  */
                  fprintf(stderr, "validation failed: "
                          "%p is locked by another thread\n", R);
                  return 0;
                }
            }
        }
    }
  return 1;
}

static void ValidateNow(struct tx_descriptor *d)
{
  d->start_time = GetGlobalCurTime(d);   // copy from the global time
  fprintf(stderr, "et.c: ValidateNow: %ld\n", (long)d->start_time);

  /* subtle: we have to normalize stolen objects, because doing so
     might add a few extra objects in the list_of_read_objects */
  if (d->public_descriptor->stolen_objects.size != 0)
    {
      spinlock_acquire(d->public_descriptor->collection_lock, 'N');
      stm_normalize_stolen_objects(d);
      spinlock_release(d->public_descriptor->collection_lock);
    }

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

void AbortPrivateFromProtected(struct tx_descriptor *d);

void AbortTransaction(int num)
{
  struct tx_descriptor *d = thread_descriptor;
  unsigned long limit;
  struct timespec now;
  long long elapsed_time;

  /* acquire the lock, but don't double-acquire it if already committing */
  if (d->public_descriptor->collection_lock != 'C') {
    spinlock_acquire(d->public_descriptor->collection_lock, 'C');
    if (d->public_descriptor->stolen_objects.size != 0)
      stm_normalize_stolen_objects(d);
  }


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

  AbortPrivateFromProtected(d);
  gcptrlist_clear(&d->list_of_read_objects);
  g2l_clear(&d->public_to_private);

  /* release the lock */
  spinlock_release(d->public_descriptor->collection_lock);

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
      fprintf(stderr, "abort %d after collect!\n", reason);
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
  assert(d->private_from_protected.size == 0);
  assert(d->num_private_from_protected_known_old == 0);
  assert(d->num_read_objects_known_old == 0);
  assert(!g2l_any_entry(&d->public_to_private));

  d->count_reads = 1;
  fxcache_clear(&d->recent_reads_cache);
#if 0
  gcptrlist_clear(&d->undolog);
  gcptrlist_clear(&d->abortinfo);
#endif
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

  assert(d->public_descriptor->stolen_objects.size == 0);

  if (!g2l_any_entry(&d->public_to_private))
    return;

  G2L_LOOP_FORWARD(d->public_to_private, item)
    {
      gcptr R = item->addr;
      revision_t v;
    retry:
      assert(R->h_tid & GCFLAG_PUBLIC);
      v = ACCESS_ONCE(R->h_revision);
      if (IS_POINTER(v))     /* "has a more recent revision" */
        {
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
      assert(L->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED ?
             L->h_revision == (revision_t)R :
             L->h_revision == stm_private_rev_num);
      assert(v != stm_private_rev_num);
      assert(v & 1);
      L->h_revision = v;   /* store temporarily this value here */

    } G2L_LOOP_END;
}

static void CancelLocks(struct tx_descriptor *d)
{
  revision_t my_lock = d->my_lock;
  wlog_t *item;

  if (!g2l_any_entry(&d->public_to_private))
    return;

  G2L_LOOP_FORWARD(d->public_to_private, item)
    {
      gcptr R = item->addr;
      gcptr L = item->val;
      if (L == NULL)
        continue;

      revision_t expected, v = L->h_revision;

      if (L->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED)
        expected = (revision_t)R;
      else
        expected = stm_private_rev_num;

      if (v == expected)
        {
          assert(R->h_revision != my_lock);
          break;    /* done */
        }

      L->h_revision = expected;

#ifdef DUMP_EXTRA
      fprintf(stderr, "%p->h_revision = %p (CancelLocks)\n", R, (gcptr)v);
#endif
      assert(R->h_revision == my_lock);
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
      assert(L->h_revision != localrev);   /* modified by AcquireLocks() */

#ifdef DUMP_EXTRA
      fprintf(stderr, "%p->h_revision = %p (UpdateChainHeads)\n",
              L, (gcptr)new_revision);
#endif
      L->h_revision = new_revision;

      gcptr stub = stm_stub_malloc(d->public_descriptor);
      stub->h_tid = (L->h_tid & STM_USER_TID_MASK) | GCFLAG_PUBLIC
                                                   | GCFLAG_STUB
                                                   | GCFLAG_OLD;
      stub->h_revision = ((revision_t)L) | 2;
      item->val = stub;

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
      assert(R->h_revision != localrev);

#ifdef DUMP_EXTRA
      fprintf(stderr, "%p->h_revision = %p (stub to %p)\n",
              R, (gcptr)v, (gcptr)item->val->h_revision);
      /*mark*/
#endif
      ACCESS_ONCE(R->h_revision) = v;

      if (R->h_tid & GCFLAG_PREBUILT_ORIGINAL)
        {
          /* cannot possibly get here more than once for a given value of R */
          pthread_mutex_lock(&mutex_prebuilt_gcroots);
          gcptrlist_insert(&stm_prebuilt_gcroots, R);
          pthread_mutex_unlock(&mutex_prebuilt_gcroots);
        }

    } G2L_LOOP_END;

  g2l_clear(&d->public_to_private);
}

void CommitPrivateFromProtected(struct tx_descriptor *d, revision_t cur_time)
{
  long i, size = d->private_from_protected.size;
  gcptr *items = d->private_from_protected.items;
  revision_t new_revision = cur_time + 1;     // make an odd number
  assert(new_revision & 1);
  assert(d->public_descriptor->stolen_objects.size == 0);

  for (i = 0; i < size; i++)
    {
      gcptr P = items[i];
      assert(P->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED);
      P->h_tid &= ~GCFLAG_PRIVATE_FROM_PROTECTED;

      if (!IS_POINTER(P->h_revision))
        {
          /* This case occurs when a GCFLAG_PRIVATE_FROM_PROTECTED object
             is stolen: it ends up as a value in 'public_to_private'.
             Its h_revision is then mangled by AcquireLocks(). */
          assert(P->h_revision != stm_private_rev_num);
          continue;
        }

      gcptr B = (gcptr)P->h_revision;
      P->h_revision = new_revision;

      if (B->h_tid & GCFLAG_PUBLIC)
        {
          /* B was stolen */
          while (1)
            {
              revision_t v = ACCESS_ONCE(B->h_revision);
              if (IS_POINTER(v))    /* "was modified" */
                AbortTransaction(ABRT_STOLEN_MODIFIED);

              if (bool_cas(&B->h_revision, v, (revision_t)P))
                break;
            }
        }
      else
        {
          stmgcpage_free(B);
          fprintf(stderr, "commit: free backup at %p\n", B);
        }
    };
  gcptrlist_clear(&d->private_from_protected);
  d->num_private_from_protected_known_old = 0;
  d->num_read_objects_known_old = 0;
  fprintf(stderr, "private_from_protected: clear (commit)\n");
}

void AbortPrivateFromProtected(struct tx_descriptor *d)
{
  long i, size = d->private_from_protected.size;
  gcptr *items = d->private_from_protected.items;

  for (i = 0; i < size; i++)
    {
      gcptr P = items[i];
      assert(P->h_tid & GCFLAG_PRIVATE_FROM_PROTECTED);
      assert(IS_POINTER(P->h_revision));
      P->h_tid &= ~GCFLAG_PRIVATE_FROM_PROTECTED;

      gcptr B = (gcptr)P->h_revision;
      assert(B->h_tid & GCFLAG_OLD);

      if (B->h_tid & GCFLAG_PUBLIC)
        {
          assert(!(B->h_tid & GCFLAG_BACKUP_COPY));
          P->h_tid |= GCFLAG_PUBLIC;
          if (!(P->h_tid & GCFLAG_OLD)) P->h_tid |= GCFLAG_NURSERY_MOVED;
          /* P becomes a public outdated object.  It may create an
             exception documented in doc-objects.txt: a public but young
             object.  It's still fine because it should only be seen by
             other threads during stealing, and as it's outdated,
             stealing will follow its h_revision (to B).
          */
        }
      else
        {
          /* copy the backup copy B back over the now-protected object P,
             and then free B, which will not be used any more. */
          size_t size = stmcb_size(B);
          assert(B->h_tid & GCFLAG_BACKUP_COPY);
          memcpy(((char *)P) + offsetof(struct stm_object_s, h_revision),
                 ((char *)B) + offsetof(struct stm_object_s, h_revision),
                 size - offsetof(struct stm_object_s, h_revision));
          assert(!(P->h_tid & GCFLAG_BACKUP_COPY));
          stmgcpage_free(B);
          fprintf(stderr, "abort: free backup at %p\n", B);
        }
    };
  gcptrlist_clear(&d->private_from_protected);
  d->num_private_from_protected_known_old = 0;
  d->num_read_objects_known_old = 0;
  fprintf(stderr, "private_from_protected: clear (abort)\n");
}

void CommitTransaction(void)
{   /* must save roots around this call */
  revision_t cur_time;
  struct tx_descriptor *d = thread_descriptor;
  assert(d->active >= 1);

  spinlock_acquire(d->public_descriptor->collection_lock, 'C');  /*committing*/
  if (d->public_descriptor->stolen_objects.size != 0)
    stm_normalize_stolen_objects(d);
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
              if (d->public_descriptor->stolen_objects.size != 0)
                stm_normalize_stolen_objects(d);

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

  CommitPrivateFromProtected(d, cur_time);

  /* we cannot abort any more from here */
  d->setjmp_buf = NULL;
  gcptrlist_clear(&d->list_of_read_objects);

  fprintf(stderr, "\n"
          "*************************************\n"
          "**************************************  committed %ld\n"
          "*************************************\n",
          (long)cur_time);

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

struct tx_descriptor *stm_tx_head = NULL;
struct tx_public_descriptor *stm_descriptor_array[MAX_THREADS] = {0};
static revision_t descriptor_array_free_list = 0;

void _stm_test_forget_previous_state(void)
{
  /* debug: reset all global states, between tests */
  fprintf(stderr, "=======================================================\n");
  assert(thread_descriptor == NULL);
  memset(stm_descriptor_array, 0, sizeof(stm_descriptor_array));
  descriptor_array_free_list = 0;
  stm_tx_head = NULL;
  stmgcpage_count(2);  /* reset */
}

struct tx_public_descriptor *stm_get_free_public_descriptor(revision_t *pindex)
{
  if (*pindex < 0)
    *pindex = descriptor_array_free_list;

  struct tx_public_descriptor *pd = stm_descriptor_array[*pindex];
  if (pd != NULL)
    {
      *pindex = pd->free_list_next;
      assert(*pindex >= 0);
    }
  return pd;
}

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
      stmgcpage_acquire_global_lock();

      struct tx_public_descriptor *pd;
      i = descriptor_array_free_list;
      pd = stm_descriptor_array[i];
      if (pd != NULL) {
          /* we are reusing 'pd' */
          descriptor_array_free_list = pd->free_list_next;
          assert(descriptor_array_free_list >= 0);
          assert(pd->stolen_objects.size == 0);
          assert(pd->stolen_young_stubs.size == 0);
          assert(pd->collection_lock == 0 || pd->collection_lock == -1);
          pd->collection_lock = 0;
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
      stm_private_rev_num = -d->my_lock;
      d->private_revision_ref = &stm_private_rev_num;
      d->max_aborts = -1;
      d->tx_prev = NULL;
      d->tx_next = stm_tx_head;
      if (d->tx_next != NULL) d->tx_next->tx_prev = d;
      stm_tx_head = d;
      thread_descriptor = d;

      fprintf(stderr, "[%lx] pthread %lx starting\n",
              (long)d->public_descriptor_index, (long)pthread_self());

      stmgcpage_init_tls();
      stmgcpage_release_global_lock();
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
    stmgcpage_acquire_global_lock();

    /* our nursery is empty at this point.  The list 'stolen_objects'
       should have been emptied at the previous minor collection and
       should remain empty because we don't have any young object. */
    assert(d->public_descriptor->stolen_objects.size == 0);
    assert(d->public_descriptor->stolen_young_stubs.size == 0);
    gcptrlist_delete(&d->public_descriptor->stolen_objects);
    gcptrlist_delete(&d->public_descriptor->stolen_young_stubs);

    stmgcpage_done_tls();
    i = d->public_descriptor_index;
    assert(stm_descriptor_array[i] == d->public_descriptor);
    d->public_descriptor->free_list_next = descriptor_array_free_list;
    descriptor_array_free_list = i;
    if (d->tx_prev != NULL) d->tx_prev->tx_next = d->tx_next;
    if (d->tx_next != NULL) d->tx_next->tx_prev = d->tx_prev;
    if (d == stm_tx_head) stm_tx_head = d->tx_next;
    stmgcpage_release_global_lock();

    thread_descriptor = NULL;

    g2l_delete(&d->public_to_private);
    assert(d->private_from_protected.size == 0);
    gcptrlist_delete(&d->private_from_protected);
    gcptrlist_delete(&d->list_of_read_objects);
#if 0
    gcptrlist_delete(&d->abortinfo);
    free(d->longest_abort_info);
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
    fprintf(stderr, "%s", line);

    stm_free(d, sizeof(struct tx_descriptor));
}
