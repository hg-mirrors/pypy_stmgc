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
given index.  It also supports fetching the list of non-NULL entries.

There are two markers for every index (a read and a write marker).
This is unlike regular arrays, which have only two markers in total.

Additionally, we use the read marker for the hashtable object itself
to mean "we have read the complete list of keys".  This plays the role
of a "global" read marker: when any thread adds a new key/value object
to the hashtable, this new object's read marker is initialized with a
copy of the "global" read marker --- in all segments.


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
    */
    uintptr_t resize_counter;

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
    uintptr_t rc = hashtable->initial_table.resize_counter;
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
    /* can only be safely called during major GC, when all other threads
       are suspended */
    assert(_has_mutex());

    long i;
    for (i = 1; i < NB_SEGMENTS; i++) {
        if (get_priv_segment(i)->transaction_state == TS_NONE)
            continue;
        if (was_read_remote(get_segment_base(i), obj))
            return true;
    }
    return false;
}

#define VOLATILE_HASHTABLE(p)    ((volatile stm_hashtable_t *)(p))
#define VOLATILE_TABLE(p)  ((volatile stm_hashtable_table_t *)(p))

static void _insert_clean(stm_hashtable_table_t *table,
                          stm_hashtable_entry_t *entry,
                          uintptr_t index)
{
    uintptr_t mask = table->mask;
    uintptr_t i = index & mask;
    if (table->items[i] == NULL) {
        table->items[i] = entry;
        return;
    }

    uintptr_t perturb = index;
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
                                  char *segment_base)
{
    dprintf(("rehash %p to size %ld, segment_base=%p\n",
             hashtable, biggercount, segment_base));

    size_t size = (offsetof(stm_hashtable_table_t, items)
                   + biggercount * sizeof(stm_hashtable_entry_t *));
    stm_hashtable_table_t *biggertable = malloc(size);
    assert(biggertable);   // XXX

    stm_hashtable_table_t *table = hashtable->table;
    table->resize_counter = (uintptr_t)biggertable;
    /* ^^^ this unlocks the table by writing a non-zero value to
       table->resize_counter, but the new value is a pointer to the
       new bigger table, so IS_EVEN() is still true */
    assert(IS_EVEN(table->resize_counter));

    init_table(biggertable, biggercount);

    uintptr_t j, mask = table->mask;
    uintptr_t rc = biggertable->resize_counter;
    for (j = 0; j <= mask; j++) {
        stm_hashtable_entry_t *entry = table->items[j];
        if (entry == NULL)
            continue;
        if (segment_base != NULL) {
            /* -> compaction during major GC */
            if (((struct stm_hashtable_entry_s *)
                       REAL_ADDRESS(segment_base, entry))->object == NULL &&
                   !_stm_was_read_by_anybody((object_t *)entry)) {
                dprintf(("  removing dead %p\n", entry));
                continue;
            }
        }

        uintptr_t eindex;
        if (segment_base == NULL)
            eindex = entry->index;   /* read from STM_SEGMENT */
        else
            eindex = ((struct stm_hashtable_entry_s *)
                       REAL_ADDRESS(segment_base, entry))->index;

        dprintf(("  insert_clean %p at index=%ld\n",
                 entry, eindex));
        _insert_clean(biggertable, entry, eindex);
        assert(rc > 6);
        rc -= 6;
    }
    biggertable->resize_counter = rc;

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

    uintptr_t rc = VOLATILE_TABLE(table)->resize_counter;

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
    if (rc > 6) {
        /* we can only enter here once!  If we allocate stuff, we may
           run the GC, and so 'hashtableobj' might move afterwards. */
        if (_is_in_nursery(hashtableobj)
            && will_allocate_in_nursery(sizeof(stm_hashtable_entry_t))) {
            /* this also means that the hashtable is from this
               transaction and not visible to other segments yet, so
               the new entry can be nursery-allocated. */
            entry = (stm_hashtable_entry_t *)
                stm_allocate(sizeof(stm_hashtable_entry_t));
            entry->userdata = stm_hashtable_entry_userdata;
            entry->index = index;
            entry->object = NULL;
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
            struct stm_hashtable_entry_s initial = {
                .userdata = stm_hashtable_entry_userdata,
                .index = index,
                .object = NULL
            };
            entry = (stm_hashtable_entry_t *)
                stm_allocate_preexisting(sizeof(stm_hashtable_entry_t),
                                         (char *)&initial.header);
            hashtable->additions++;
            /* make sure .object is NULL in all segments before
               "publishing" the entry in the hashtable.  In other words,
               the following write_fence() prevents a partially
               initialized 'entry' from showing up in table->items[i],
               where it could be read from other threads. */
            write_fence();
        }
        table->items[i] = entry;
        write_fence();     /* make sure 'table->items' is written here */
        VOLATILE_TABLE(table)->resize_counter = rc - 6;    /* unlock */
        return entry;
    }
    else {
        /* if rc is smaller than 6, we must allocate a new bigger table.
         */
        uintptr_t biggercount = table->mask + 1;
        if (biggercount < 50000)
            biggercount *= 4;
        else
            biggercount *= 2;
        _stm_rehash_hashtable(hashtable, biggercount, /*segment_base=*/NULL);
        goto restart;
    }
}

object_t *stm_hashtable_read(object_t *hobj, stm_hashtable_t *hashtable,
                             uintptr_t key)
{
    stm_hashtable_entry_t *e = stm_hashtable_lookup(hobj, hashtable, key);
    stm_read((object_t *)e);
    return e->object;
}

void stm_hashtable_write_entry(object_t *hobj, stm_hashtable_entry_t *entry,
                               object_t *nvalue)
{
    if (_STM_WRITE_CHECK_SLOWPATH((object_t *)entry)) {

        stm_write((object_t *)entry);

        /* this restriction may be lifted, see test_new_entry_if_nursery_full: */
        assert(IMPLY(_is_young((object_t *)entry), _is_young(hobj)));

        uintptr_t i = list_count(STM_PSEGMENT->modified_old_objects);
        if (i > 0 && list_item(STM_PSEGMENT->modified_old_objects, i - 3)
                     == (uintptr_t)entry) {
            /* The stm_write() above recorded a write to 'entry'.  Here,
               we add another stm_undo_s to modified_old_objects with
               TYPE_MODIFIED_HASHTABLE.  It is ignored everywhere except
               in _stm_validate().

               The goal is that this TYPE_MODIFIED_HASHTABLE ends up in
               the commit log's 'cl_written' array.  Later, another
               transaction validating that log will check two things:

               - the regular stm_undo_s entry put by stm_write() above
                 will make the other transaction check that it didn't
                 read the same 'entry' object;

                 - the TYPE_MODIFIED_HASHTABLE entry we're adding now
                   will make the other transaction check that it didn't
                   do any stm_hashtable_list() on the complete hashtable.
            */
            acquire_modification_lock_wr(STM_SEGMENT->segment_num);
            STM_PSEGMENT->modified_old_objects = list_append3(
                STM_PSEGMENT->modified_old_objects,
                TYPE_POSITION_MARKER,      /* type1 */
                TYPE_MODIFIED_HASHTABLE,   /* type2 */
                (uintptr_t)hobj);          /* modif_hashtable */
            release_modification_lock_wr(STM_SEGMENT->segment_num);
        }
    }
    entry->object = nvalue;
}

void stm_hashtable_write(object_t *hobj, stm_hashtable_t *hashtable,
                         uintptr_t key, object_t *nvalue,
                         stm_thread_local_t *tl)
{
    STM_PUSH_ROOT(*tl, nvalue);
    STM_PUSH_ROOT(*tl, hobj);
    stm_hashtable_entry_t *e = stm_hashtable_lookup(hobj, hashtable, key);
    STM_POP_ROOT(*tl, hobj);
    STM_POP_ROOT(*tl, nvalue);
    stm_hashtable_write_entry(hobj, e, nvalue);
}

long stm_hashtable_length_upper_bound(stm_hashtable_t *hashtable)
{
    stm_hashtable_table_t *table;
    uintptr_t rc;

 restart:
    table = VOLATILE_HASHTABLE(hashtable)->table;
    rc = VOLATILE_TABLE(table)->resize_counter;
    if (IS_EVEN(rc)) {
        spin_loop();
        goto restart;
    }

    uintptr_t initial_rc = (table->mask + 1) * 4 + 1;
    uintptr_t num_entries_times_6 = initial_rc - rc;
    return num_entries_times_6 / 6;
}

long stm_hashtable_list(object_t *hobj, stm_hashtable_t *hashtable,
                        stm_hashtable_entry_t * TLPREFIX *results)
{
    /* Set the read marker.  It will be left as long as we're running
       the same transaction.
    */
    stm_read(hobj);

    /* Get the table.  No synchronization is needed: we may miss some
       entries that are being added, but they would contain NULL in
       this segment anyway. */
    stm_hashtable_table_t *table = VOLATILE_HASHTABLE(hashtable)->table;

    /* Read all entries, check which ones are not NULL, count them,
       and optionally list them in 'results'.
    */
    uintptr_t i, mask = table->mask;
    stm_hashtable_entry_t *entry;
    long nresult = 0;

    if (results != NULL) {
        /* collect the results in the provided list */
        for (i = 0; i <= mask; i++) {
            entry = VOLATILE_TABLE(table)->items[i];
            if (entry != NULL) {
                stm_read((object_t *)entry);
                if (entry->object != NULL)
                    results[nresult++] = entry;
            }
        }
    }
    else {
        /* don't collect, just get the exact number of results */
        for (i = 0; i <= mask; i++) {
            entry = VOLATILE_TABLE(table)->items[i];
            if (entry != NULL) {
                stm_read((object_t *)entry);
                if (entry->object != NULL)
                    nresult++;
            }
        }
    }
    return nresult;
}

static void _stm_compact_hashtable(struct object_s *hobj,
                                   stm_hashtable_t *hashtable)
{
    stm_hashtable_table_t *table = hashtable->table;
    uintptr_t rc = table->resize_counter;
    assert(!IS_EVEN(rc));

    if (hashtable->additions * 4 > table->mask) {
        hashtable->additions = 0;

        /* If 'hobj' was created in some current transaction, i.e. if it is
           now an overflow object, then we have the risk that some of its
           entry objects were not created with stm_allocate_preexisting().
           In that situation, a valid workaround is to read all entry
           objects in the segment of the running transaction.  Otherwise,
           the base case is to read them all from segment zero.
        */
        long segnum = get_segment_of_linear_address((char *)hobj);
        if (!IS_OVERFLOW_OBJ(get_priv_segment(segnum), hobj))
            segnum = 0;

        uintptr_t initial_rc = (table->mask + 1) * 4 + 1;
        uintptr_t num_entries_times_6 = initial_rc - rc;
        uintptr_t count = INITIAL_HASHTABLE_SIZE;
        while (count * 4 < num_entries_times_6)
            count *= 2;
        /* sanity-check: 'num_entries_times_6 < initial_rc', and so 'count'
           can never grow larger than the current table size. */
        assert(count <= table->mask + 1);

        dprintf(("compact with %ld items:\n", num_entries_times_6 / 6));
        _stm_rehash_hashtable(hashtable, count, get_segment_base(segnum));
    }

    table = hashtable->table;
    assert(!IS_EVEN(table->resize_counter));

    if (table != &hashtable->initial_table) {
        uintptr_t rc = hashtable->initial_table.resize_counter;
        while (1) {
            assert(IS_EVEN(rc));
            assert(rc != RESIZING_LOCK);

            stm_hashtable_table_t *old_table = (stm_hashtable_table_t *)rc;
            if (old_table == table)
                break;
            rc = old_table->resize_counter;
            free(old_table);
        }
        hashtable->initial_table.resize_counter = (uintptr_t)table;
        assert(IS_EVEN(hashtable->initial_table.resize_counter));
    }
}

void stm_hashtable_tracefn(struct object_s *hobj, stm_hashtable_t *hashtable,
                           void trace(object_t **))
{
    if (trace == TRACE_FOR_MAJOR_COLLECTION)
        _stm_compact_hashtable(hobj, hashtable);

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
