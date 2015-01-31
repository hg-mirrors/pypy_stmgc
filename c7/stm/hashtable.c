/*
Design of stmgc's "hashtable" objects
=====================================

A "hashtable" is theoretically a lazily-filled array of objects of
length 2**64.  Initially it is full of NULLs.  It's obviously
implemented as a dictionary in which NULL objects are not needed.

A real dictionary can be implemented on top of it, by using the index
`hash(key)` in the hashtable, and storing a list of `(key, value)`
pairs at that index (usually only one, unless there is a hash
collision).

The main operations on a hashtable are reading or writing an object at a
given index.  It also supports iterating its non-NULL entries.

There are two markers for every index (a read and a write marker).
This is unlike regular arrays, which have only two markers in total.

Additionally, we use the read marker for the hashtable object itself
to mean "we are iterating".  When a transaction has got this "global"
read marker and another transaction attempts to create a new key/value
pair via stm_hashtable_{lookup,read,write}, the call immediately fails
with a read/write conflict.  This gives priority to the iterating
transaction rather than the modifying transaction, which is probably
what we want.

XXX NO: when creating new key/value objects, we should copy the read
marker from the hashtableobj to the new key/value object.  I *think*
this gives a correct and better result XXX


Implementation
--------------

First idea: have the hashtable in raw memory, pointing to "entry"
objects (which are regular, GC- and STM-managed objects).  The entry
objects themselves point to the user-specified objects.  The entry
objects hold the read/write markers.  Every entry object, once
created, stays around.  It is only removed by the next major GC if it
points to NULL and its read/write markers are not set in any
currently-running transaction.

References
----------

Inspired by: http://ppl.stanford.edu/papers/podc011-bronson.pdf
*/


uint32_t stm_hashtable_entry_userdata;


#define INITIAL_HASHTABLE_SIZE   8
#define PERTURB_SHIFT            5
#define RESIZING_LOCK            0

typedef struct {
    uintptr_t mask;

    /* 'resize_counter' start at an odd value, and is decremented (by
       6) for every new item put in 'items'.  When it crosses 0, we
       instead allocate a bigger table and change 'resize_counter' to
       be a regular pointer to it (which is then even).  The whole
       structure is immutable then.

       The field 'resize_counter' also works as a write lock: changes
       go via the intermediate value RESIZING_LOCK (0).

       In addition, 'resize_counter' can be the negation of the odd
       number that it would normally be: in this case it is "probably
       write-protected" (see stm_hashtable_next()).
    */
    intptr_t resize_counter;

    stm_hashtable_entry_t *items[INITIAL_HASHTABLE_SIZE];
} stm_hashtable_table_t;

#define IS_EVEN(p) (((p) & 1) == 0)

struct stm_hashtable_s {
    stm_hashtable_table_t *table;
    stm_hashtable_table_t initial_table;
    uint64_t additions;
};


static inline void init_table(stm_hashtable_table_t *table, uintptr_t itemcount)
{
    table->mask = itemcount - 1;
    table->resize_counter = itemcount * 4 + 1;
    memset(table->items, 0, itemcount * sizeof(stm_hashtable_entry_t *));
}

stm_hashtable_t *stm_hashtable_create(void)
{
    stm_hashtable_t *hashtable = malloc(sizeof(stm_hashtable_t));
    assert(hashtable);
    hashtable->table = &hashtable->initial_table;
    hashtable->additions = 0;
    init_table(&hashtable->initial_table, INITIAL_HASHTABLE_SIZE);
    return hashtable;
}

void stm_hashtable_free(stm_hashtable_t *hashtable)
{
    intptr_t rc = hashtable->initial_table.resize_counter;
    free(hashtable);
    while (IS_EVEN(rc)) {
        assert(rc != RESIZING_LOCK);

        stm_hashtable_table_t *table = (stm_hashtable_table_t *)rc;
        rc = table->resize_counter;
        free(table);
    }
}

static bool _stm_was_read_by_anybody(object_t *obj)
{
    long i;
    for (i = 1; i <= NB_SEGMENTS; i++) {
        char *remote_base = get_segment_base(i);
        uint8_t remote_version = get_segment(i)->transaction_read_version;
        if (was_read_remote(remote_base, obj, remote_version))
            return true;
    }
    return false;
}

#define VOLATILE_HASHTABLE(p)    ((volatile stm_hashtable_t *)(p))
#define VOLATILE_TABLE(p)  ((volatile stm_hashtable_table_t *)(p))

static void _insert_clean(stm_hashtable_table_t *table,
                          stm_hashtable_entry_t *entry)
{
    uintptr_t mask = table->mask;
    uintptr_t i = entry->index & mask;
    if (table->items[i] == NULL) {
        table->items[i] = entry;
        return;
    }

    uintptr_t perturb = entry->index;
    while (1) {
        i = (i << 2) + i + perturb + 1;
        i &= mask;
        if (table->items[i] == NULL) {
            table->items[i] = entry;
            return;
        }

        perturb >>= PERTURB_SHIFT;
    }
}

static void _stm_rehash_hashtable(stm_hashtable_t *hashtable,
                                  uintptr_t biggercount,
                                  int remove_unread_from_seg,
                                  bool rc_must_be_negative)
{
    dprintf(("rehash %p to %ld, remove_unread_from_seg=%d\n",
             hashtable, biggercount, remove_unread_from_seg));

    size_t size = (offsetof(stm_hashtable_table_t, items)
                   + biggercount * sizeof(stm_hashtable_entry_t *));
    stm_hashtable_table_t *biggertable = malloc(size);
    assert(biggertable);   // XXX

    stm_hashtable_table_t *table = hashtable->table;
    table->resize_counter = (intptr_t)biggertable;
    /* ^^^ this unlocks the table by writing a non-zero value to
       table->resize_counter, but the new value is a pointer to the
       new bigger table, so IS_EVEN() is still true */
    assert(IS_EVEN(table->resize_counter));

    init_table(biggertable, biggercount);

    uintptr_t j, mask = table->mask;
    intptr_t rc = biggertable->resize_counter;
    char *segment_base = get_segment_base(remove_unread_from_seg);
    for (j = 0; j <= mask; j++) {
        stm_hashtable_entry_t *entry = table->items[j];
        if (entry == NULL)
            continue;
        if (remove_unread_from_seg != 0) {
            if (((struct stm_hashtable_entry_s *)
                       REAL_ADDRESS(segment_base, entry))->object == NULL &&
                   !_stm_was_read_by_anybody((object_t *)entry)) {
                dprintf(("  removing dead %p\n", entry));
                continue;
            }
        }
        _insert_clean(biggertable, entry);
        rc -= 6;
    }
    assert(rc > 0);
    biggertable->resize_counter = rc_must_be_negative ? -rc : rc;

    write_fence();   /* make sure that 'biggertable' is valid here,
                        and make sure 'table->resize_counter' is updated
                        ('table' must be immutable from now on). */
    VOLATILE_HASHTABLE(hashtable)->table = biggertable;
}

stm_hashtable_entry_t *stm_hashtable_lookup(object_t *hashtableobj,
                                            stm_hashtable_t *hashtable,
                                            uintptr_t index)
{
    stm_hashtable_table_t *table;
    uintptr_t mask;
    uintptr_t i;
    stm_hashtable_entry_t *entry;

 restart:
    /* classical dict lookup logic */
    table = VOLATILE_HASHTABLE(hashtable)->table;
    mask = table->mask;      /* read-only field */
    i = index & mask;
    entry = VOLATILE_TABLE(table)->items[i];
    if (entry != NULL) {
        if (entry->index == index)
            return entry;           /* found at the first try */

        uintptr_t perturb = index;
        while (1) {
            i = (i << 2) + i + perturb + 1;
            i &= mask;
            entry = VOLATILE_TABLE(table)->items[i];
            if (entry != NULL) {
                if (entry->index == index)
                    return entry;    /* found */
            }
            else
                break;
            perturb >>= PERTURB_SHIFT;
        }
    }
    /* here, we didn't find the 'entry' with the correct index.  Note
       that even if the same 'table' is modified or resized by other
       threads concurrently, any new item found from a race condition
       would anyway contain NULL in the present segment (ensured by
       the first write_fence() below).  If the 'table' grows an entry
       just after we checked above, then we go ahead and lock the
       table; but after we get the lock, we will notice the new entry
       (ensured by the second write_fence() below) and restart the
       whole process.
     */

    intptr_t rc = VOLATILE_TABLE(table)->resize_counter;
    bool rc_must_be_negative = false;

    /* if rc is RESIZING_LOCK (which is 0, so even), a concurrent thread
       is writing to the hashtable.  Or, if rc is another even number, it is
       actually a pointer to the next version of the table, installed
       just now.  In both cases, this thread must simply spin loop.
    */
    if (IS_EVEN(rc)) {
        spin_loop();
        goto restart;
    }
    /* in the other cases, we need to grab the RESIZING_LOCK.
     */
    if (!__sync_bool_compare_and_swap(&table->resize_counter,
                                      rc, RESIZING_LOCK)) {
        goto restart;
    }
    /* we now have the lock.  The only table with a non-even value of
       'resize_counter' should be the last one in the chain, so if we
       succeeded in locking it, check this. */
    assert(table == hashtable->table);

    /* Check that 'table->items[i]' is still NULL,
       i.e. hasn't been populated under our feet.
    */
    if (table->items[i] != NULL) {
        table->resize_counter = rc;    /* unlock */
        goto restart;
    }
    /* if rc is greater than 6, there is enough room for a new
       item in the current table.
    */
 retry_adding:
    if (rc > 6) {
        /* we can only enter here once!  If we allocate stuff, we may
           run the GC, and so 'hashtableobj' might move afterwards. */
        if (_is_in_nursery(hashtableobj)) {
            entry = (stm_hashtable_entry_t *)
                stm_allocate(sizeof(stm_hashtable_entry_t));
            entry->userdata = stm_hashtable_entry_userdata;
            entry->index = index;
            entry->object = NULL;
            hashtable->additions = STM_SEGMENT->segment_num;
        }
        else {
            /* for a non-nursery 'hashtableobj', we pretend that the
               'entry' object we're about to return was already
               existing all along, with NULL in all segments.  If the
               caller of this function is going to modify the 'object'
               field, it will call stm_write(entry) first, which will
               correctly schedule 'entry' for write propagation.  We
               do that even if 'hashtableobj' was created by the
               running transaction: the new 'entry' object is created
               as if it was older than the transaction.

               Note the following difference: if 'hashtableobj' is
               still in the nursery (case above), the 'entry' object
               is also allocated from the nursery, and after a minor
               collection it ages as an old-but-created-by-the-
               current-transaction object.  We could try to emulate
               this here, or to create young 'entry' objects, but
               doing either of these would require careful
               synchronization with other pieces of the code that may
               change.
            */
            acquire_privatization_lock();
            char *p = allocate_outside_nursery_large(
                          sizeof(stm_hashtable_entry_t));
            entry = (stm_hashtable_entry_t *)(p - stm_object_pages);

            long j;
            for (j = 0; j <= NB_SEGMENTS; j++) {
                struct stm_hashtable_entry_s *e;
                e = (struct stm_hashtable_entry_s *)
                        REAL_ADDRESS(get_segment_base(j), entry);
                e->header.stm_flags = GCFLAG_WRITE_BARRIER;
                e->userdata = stm_hashtable_entry_userdata;
                e->index = index;
                e->object = NULL;
            }
            hashtable->additions += 0x100;
            release_privatization_lock();
        }
        write_fence();     /* make sure 'entry' is fully initialized here */
        table->items[i] = entry;
        write_fence();     /* make sure 'table->items' is written here */
        rc -= 6;
        VOLATILE_TABLE(table)->resize_counter = (
                                  rc_must_be_negative ? -rc : rc);  /* unlock */
        return entry;
    }
    else if (rc > 0) {
        /* if rc is smaller than 6, we must allocate a new bigger table.
         */
        uintptr_t biggercount = table->mask + 1;
        if (biggercount < 50000)
            biggercount *= 4;
        else
            biggercount *= 2;
        _stm_rehash_hashtable(hashtable, biggercount, /*remove_unread=*/0,
                              rc_must_be_negative);
        goto restart;
    }
    else {
        assert(rc < 0);
        assert(!_is_in_nursery(hashtableobj));

        /* if rc is negative, the hashtable is probably write-protected.
           Check if the read marker of the 'hashtableobj' is set in
           another segment.
         */
        int j, my_segment = STM_SEGMENT->segment_num;
        for (j = 1; j <= NB_SEGMENTS; j++) {
            if (j == my_segment)
                continue;
            if (was_read_remote(get_segment_base(j), hashtableobj)) {
                /* conflict! */
                table->resize_counter = rc;    /* unlock */
                if (write_read_contention_management(j, hashtableobj)) {
                    /* If we reach this point, we didn't abort, but we
                       had to wait for the other thread to commit.  If
                       we did, then we have to restart. */
                    return true;
                    ...;
                    
                    }
                    /* we aborted the other transaction without waiting, so
                       we can just break out of this loop on
                       modified_old_objects and continue with the next
                       segment */
                    
                    xxx;
                }
            }
        }

        /* if even in this thread's segment it was not read, then there
           is no point in keeping it write-protected.  So we set
           'rc_must_be_negative', i.e. keep it write-protected, iff
           it was read locally.
        */
        rc_must_be_negative = was_read_local(hashtableobj);
        rc = -rc;
        goto retry_adding;
    }
}

object_t *stm_hashtable_read(object_t *hobj, stm_hashtable_t *hashtable,
                             uintptr_t key)
{
    stm_hashtable_entry_t *e = stm_hashtable_lookup(hobj, hashtable, key);
    stm_read((object_t *)e);
    return e->object;
}

void stm_hashtable_write(object_t *hobj, stm_hashtable_t *hashtable,
                         uintptr_t key, object_t *nvalue,
                         stm_thread_local_t *tl)
{
    STM_PUSH_ROOT(*tl, nvalue);
    stm_hashtable_entry_t *e = stm_hashtable_lookup(hobj, hashtable, key);
    stm_write((object_t *)e);
    STM_POP_ROOT(*tl, nvalue);
    e->object = nvalue;
}

struct stm_hashtable_entry_s *
stm_hashtable_next(object_t *hobj, stm_hashtable_t *hashtable,
                   uintptr_t *pposition, stm_thread_local_t *tl)
{
    /* this assumes the simple c7 model whereby commits only occur with
       all other transaction paused at a known point. */
    stm_hashtable_table_t *table;
    intptr_t rc;

    /* First set the read marker.  It will be left as long as we're running
       the same transaction.  Note that this code assumes that nothing else
       can set the read marker!  Also, if 'hobj' is still in the nursery,
       it was created by this transaction and there is nothing to do.
    */
    if (!_is_in_nursery(hobj) && !was_read_local(hobj)) {

        stm_read(hobj);

        /* Change the 'resize_counter' field to its negative value.  This
           must be done after we've set the read marker. */
     restart:
        table = VOLATILE_HASHTABLE(hashtable)->table;
        rc = VOLATILE_TABLE(table)->resize_counter;
        if (IS_EVEN(rc)) {
            spin_loop();
            goto restart;
        }
        if (!__sync_bool_compare_and_swap(&table->resize_counter, rc,
                                          rc > 0 ? -rc : rc))
            goto restart;
        /* Note that we did a compare-and-swap even if rc was already
           negative.  This is needed for its memory-ordering effect,
           to ensure that from now on the other threads do see our
           read marker set. */
    }
    else {
        /* Read marker already set.  Assume (and assert) that we
           already set a negative value into 'resize_counter'.
           Changes of 'table' or 'resize_counter' under our feet
           should not be possible here.
        */
        table = hashtable->table;

        if (!_is_in_nursery(hobj)) {
            assert(!IS_EVEN(table->resize_counter) &&
                   table->resize_counter < 0);
        }
    }

    /* At this point, the hashtable is write-protected: no other
       thread may add new key/value objects nor grow/replace the
       'table'.  The hashtable will remain write-protected as long as
       this transaction is running.  Note that *this* thread is
       allowed to continue modifying the hashtable (unless another
       thread did also set a write protection).
    */
    uintptr_t position = *pposition;
    uintptr_t mask = table->mask;
    stm_hashtable_entry_t *entry;

    while (position <= mask) {
        entry = table->items[position++];
        if (entry != NULL) {
            *pposition = position;
            return entry;
        }
    }
    *pposition = (uintptr_t)-1;
    return NULL;
}

static void _stm_compact_hashtable(stm_hashtable_t *hashtable)
{
    stm_hashtable_table_t *table = hashtable->table;
    intptr_t rc = table->resize_counter;
    assert(!IS_EVEN(rc));

    if ((hashtable->additions >> 8) * 4 > table->mask) {
        int segment_num = (hashtable->additions & 0xFF);
        if (!segment_num) segment_num = 1;
        hashtable->additions = segment_num;
        uintptr_t initial_rc = (table->mask + 1) * 4 + 1;
        uintptr_t num_entries_times_6 = initial_rc - (rc < 0 ? -rc : rc);
        uintptr_t count = INITIAL_HASHTABLE_SIZE;
        while (count * 4 < num_entries_times_6)
            count *= 2;
        /* sanity-check: 'num_entries_times_6 < initial_rc', and so 'count'
           can never grow larger than the current table size. */
        assert(count <= table->mask + 1);

        dprintf(("compact with %ld items:\n", num_entries_times_6 / 6));
        _stm_rehash_hashtable(hashtable, count, /*remove_unread=*/segment_num,
                              /*rc_must_be_negative=*/rc < 0);
    }

    table = hashtable->table;
    assert(!IS_EVEN(table->resize_counter));

    if (table != &hashtable->initial_table) {
        intptr_t rc = hashtable->initial_table.resize_counter;
        while (1) {
            assert(IS_EVEN(rc));
            assert(rc != RESIZING_LOCK);

            stm_hashtable_table_t *old_table = (stm_hashtable_table_t *)rc;
            if (old_table == table)
                break;
            rc = old_table->resize_counter;
            free(old_table);
        }
        hashtable->initial_table.resize_counter = (intptr_t)table;
        assert(IS_EVEN(hashtable->initial_table.resize_counter));
    }
}

void stm_hashtable_tracefn(stm_hashtable_t *hashtable, void trace(object_t **))
{
    if (trace == TRACE_FOR_MAJOR_COLLECTION)
        _stm_compact_hashtable(hashtable);

    stm_hashtable_table_t *table;
    table = VOLATILE_HASHTABLE(hashtable)->table;

    uintptr_t j, mask = table->mask;
    for (j = 0; j <= mask; j++) {
        stm_hashtable_entry_t *volatile *pentry;
        pentry = &VOLATILE_TABLE(table)->items[j];
        if (*pentry != NULL) {
            trace((object_t **)pentry);
        }
    }
}
