#include "stmimpl.h"


/* Global data (initialized in init_global_data()):

   'full_pages' contains pages that are presumably full and will not be
   reused until the next major collection.
 */
static page_header_t *full_pages[GC_SMALL_REQUESTS];
static local_gcpages_t *finished_thread_gcpages;

/* This maps each small request size to the number of blocks of this size
   that fit in a page.
 */
static int nblocks_for_size[GC_SMALL_REQUESTS];

/* A mutex for major collections and other global operations */
static pthread_mutex_t mutex_gc_lock = PTHREAD_MUTEX_INITIALIZER;

/* This is the head of a double linked list of all tx_descriptors */
static struct tx_descriptor *tx_head = NULL;

/* A count-down: when it reaches 0, run the next major collection */
static revision_t countdown_next_major_coll = GC_MIN;

/* For statistics */
static uintptr_t count_global_pages;

/* Only computed during a major collection */
static uintptr_t mc_total_in_use, mc_total_reserved;

/* For tests */
long stmgcpage_count(int quantity)
{
    switch (quantity) {
    case 0: return count_global_pages;
    case 1: return LOCAL_GCPAGES()->count_pages;
    default: return -1;
    }
}


/***** Support code *****/

void stmgcpage_acquire_global_lock(void)
{
    int err = pthread_mutex_lock(&mutex_gc_lock);
    assert(err == 0);
}

void stmgcpage_release_global_lock(void)
{
    int err = pthread_mutex_unlock(&mutex_gc_lock);
    assert(err == 0);
}


/***** Initialization logic *****/

static void init_global_data(void)
{
    int i;
    for (i = 1; i < GC_SMALL_REQUESTS; i++) {
        nblocks_for_size[i] =
            (GC_PAGE_SIZE - sizeof(page_header_t)) / (WORD * i);
    }
}

void stmgcpage_init_tls(void)
{
    stmgcpage_acquire_global_lock();

    if (nblocks_for_size[1] == 0)
        init_global_data();

    /* build a local_gcpages_t */
    struct tx_descriptor *d = thread_descriptor;
    local_gcpages_t *gcp = stm_malloc(sizeof(local_gcpages_t));
    memset(gcp, 0, sizeof(local_gcpages_t));
    d->local_gcpages = gcp;

    /* insert 'thread_descriptor' at the head of the doubly linked list */
    d->tx_prev = NULL;
    d->tx_next = tx_head;
    if (tx_head) {
        assert(tx_head->tx_prev == NULL);
        tx_head->tx_prev = d;
    }
    tx_head = d;

    stmgcpage_release_global_lock();
}

void stmgcpage_done_tls(void)
{
    /* Send to the shared area all my pages.  For now we don't extract
       the information about which locations are free or not; we just
       leave it to the next major GC to figure them out. */
    local_gcpages_t *gcp = LOCAL_GCPAGES();
    stmgcpage_acquire_global_lock();

    gcp->gcp_next = finished_thread_gcpages;
    finished_thread_gcpages = gcp;
    count_global_pages += gcp->count_pages;

    /* remove 'thread_descriptor' from the doubly linked list */
    struct tx_descriptor *d = thread_descriptor;
    if (d->tx_next) d->tx_next->tx_prev = d->tx_prev;
    if (d->tx_prev) d->tx_prev->tx_next = d->tx_next;
    if (d == tx_head) tx_head = d->tx_next;
    d->tx_next = (struct tx_descriptor *)-1;   /* a random value */

    stmgcpage_release_global_lock();
}

struct tx_descriptor *stm_find_thread_containing_pointer_and_lock(gcptr L)
{
    stmgcpage_acquire_global_lock();

    struct tx_descriptor *d;
    for (d = tx_head; d; d = d->tx_next) {
        if (stmgc_is_young_in(d, L))
            goto found;
    }
    stmgcpage_release_global_lock();
    return NULL;         /* L is not a young pointer anywhere! */

 found:
    /* must acquire the collection_lock before releasing the global lock,
       otherwise 'd' might be freed under our feet */
    spinlock_acquire(d->collection_lock, 'S');  /* stealing */

    stmgcpage_release_global_lock();
    return d;
}


/***** Thread-local allocator *****/

void stmgcpage_reduce_threshold(size_t size)
{
    revision_t next, target;
 restart:
    next = ACCESS_ONCE(countdown_next_major_coll);
    if (next >= size)
        target = next - size;
    else
        target = 0;
    if (!bool_cas(&countdown_next_major_coll, next, target))
        goto restart;
}

static gcptr allocate_new_page(int size_class)
{
    /* Adjust the threshold; the caller is responsible for detecting the
       condition that the threshold reached 0. */
    stmgcpage_reduce_threshold(GC_PAGE_SIZE);

    /* Allocate and return a new page for the given size_class. */
    page_header_t *page = (page_header_t *)stm_malloc(GC_PAGE_SIZE);
    if (!page) {
        fprintf(stderr, "allocate_new_page: out of memory!\n");
        abort();
    }
    local_gcpages_t *gcp = LOCAL_GCPAGES();
    gcp->count_pages++;

    /* Initialize the fields of the resulting page */
    page->next_page = gcp->pages_for_size[size_class];
    gcp->pages_for_size[size_class] = page;

    /* Initialize the chained list in the page */
    gcptr head = (gcptr)(page + 1);
    gcptr current, next;
    int count = nblocks_for_size[size_class];
    int nsize = size_class * WORD;
    int i;
    current = head;
    for (i = 0; i < count - 1; i++) {
        next = (gcptr)(((char *)current) + nsize);
        assert(!(GCFLAG_VISITED & DEBUG_WORD(0xDD)));
        current->h_tid = DEBUG_WORD(0xDD);  /*anything without GCFLAG_VISITED*/
        current->h_revision = (revision_t)next;
        stm_dbgmem_not_used(current, nsize, 0);
        current = next;
    }
    current->h_tid = DEBUG_WORD(0xDD);
    current->h_revision = (revision_t)gcp->free_loc_for_size[size_class];
    stm_dbgmem_not_used(current, nsize, 0);
    gcp->free_loc_for_size[size_class] = head;
    return head;
}

gcptr stmgcpage_malloc(size_t size)
{
    /* Allocates an object of the given 'size'.  This will never run
       a collection: you need to call stmgcpage_possibly_major_collect(0)
       when you know you're at a safe point. */
    if (size <= GC_SMALL_REQUEST_THRESHOLD) {
        gcptr result;
        local_gcpages_t *gcp = LOCAL_GCPAGES();
        int size_class = (size + WORD - 1) / WORD;
        assert(0 < size_class && size_class < GC_SMALL_REQUESTS);

        /* The result is simply 'free_loc_for_size[size_class]' */
        result = gcp->free_loc_for_size[size_class];
        if (!result) {
            result = allocate_new_page(size_class);
        }
        gcp->free_loc_for_size[size_class] = (gcptr)result->h_revision;
        stm_dbgmem_used_again(result, size_class * WORD, 0);
        return result;
    }
    else {
        fprintf(stderr, "XXX stmgcpage_malloc: too big!\n");
        abort();
    }
}

void stmgcpage_free(gcptr obj)
{
    size_t size = stmcb_size(obj);
    if (size <= GC_SMALL_REQUEST_THRESHOLD) {
        local_gcpages_t *gcp = LOCAL_GCPAGES();
        int size_class = (size + WORD - 1) / WORD;
        assert(0 < size_class && size_class < GC_SMALL_REQUESTS);

        /* We simply re-add the object to the right chained list */
        obj->h_revision = (revision_t)gcp->free_loc_for_size[size_class];
        gcp->free_loc_for_size[size_class] = obj;
        stm_dbgmem_not_used(obj, size_class * WORD, 0);
    }
    else {
        fprintf(stderr, "XXX stmgcpage_free: too big!\n");
        abort();
    }
}


/***** Prebuilt roots, added in the list as the transaction that changed
       them commits *****/

struct GcPtrList stm_prebuilt_gcroots = {0};

void stmgcpage_add_prebuilt_root(gcptr obj)
{
    assert(obj->h_tid & GCFLAG_PREBUILT_ORIGINAL);
    gcptrlist_insert(&stm_prebuilt_gcroots, obj);
}

void stm_clear_between_tests(void)
{
    fprintf(stderr, "\n"
            "===============================================================\n"
            "========================[  START  ]============================\n"
            "===============================================================\n"
            "\n");
    gcptrlist_clear(&stm_prebuilt_gcroots);
}


/***** Major collections: marking *****/

static struct GcPtrList objects_to_trace;

static void visit(gcptr *pobj)
{
    gcptr obj = *pobj;
    if (obj == NULL)
        return;

 restart:
    if (obj->h_tid & GCFLAG_VISITED)
        return;    /* already seen */

    if (obj->h_tid & (GCFLAG_PUBLIC_TO_PRIVATE | GCFLAG_STUB)) {
        if (obj->h_revision & 1) { // "is not a ptr", so no more recent version
            obj->h_tid &= ~GCFLAG_PUBLIC_TO_PRIVATE; // see also fix_outdated()
        }
        else {
            obj = (gcptr)obj->h_revision;   // go visit the more recent version
            *pobj = obj;
            goto restart;
        }
    }
    else
        assert(obj->h_revision & 1);

    obj->h_tid |= GCFLAG_VISITED;
    gcptrlist_insert(&objects_to_trace, obj);
}

static void visit_all_objects(void)
{
    while (gcptrlist_size(&objects_to_trace) > 0) {
        gcptr obj = gcptrlist_pop(&objects_to_trace);
        stmcb_trace(obj, &visit);
    }
}

static void mark_prebuilt_roots(void)
{
    /* Note about prebuilt roots: 'stm_prebuilt_gcroots' is a list that
       contains all the ones that have been modified.  Because they are
       themselves not in any page managed by this file, their
       GCFLAG_VISITED will not be removed at the end of the current
       collection.  This is fine because the base object cannot contain
       references to the heap.  So we decided to systematically set
       GCFLAG_VISITED on prebuilt objects. */
    gcptr *pobj = stm_prebuilt_gcroots.items;
    gcptr *pend = stm_prebuilt_gcroots.items + stm_prebuilt_gcroots.size;
    gcptr obj;
    for (; pobj != pend; pobj++) {
        obj = *pobj;
        assert(obj->h_tid & GCFLAG_PREBUILT_ORIGINAL);
        assert(obj->h_tid & GCFLAG_VISITED);
        assert((obj->h_revision & 1) == 0);   /* "is a pointer" */
        visit((gcptr *)&obj->h_revision);
    }
}

static void mark_roots(gcptr *root, gcptr *end)
{
    //assert(*root == END_MARKER);
    //root++;
    while (root != end)
        visit(root++);
}

static void mark_all_stack_roots(void)
{
    struct tx_descriptor *d;
    for (d = tx_head; d; d = d->tx_next) {

        /* the roots pushed on the shadowstack */
        mark_roots(d->shadowstack, *d->shadowstack_end_ref);

        /* the thread-local object */
        visit(d->thread_local_obj_ref);

        /* the current transaction's private copies of public objects */
        wlog_t *item;
        G2L_LOOP_FORWARD(d->public_to_private, item) {

            /* note that 'item->addr' is also in the read set, so if it was
               outdated, it will be found at that time */
            visit(&item->addr);
            visit(&item->val);

        } G2L_LOOP_END;

        /* make sure that the other lists are empty */
        assert(gcptrlist_size(&d->protected_with_private_copy) == 0);
        assert(gcptrlist_size(&d->private_old_pointing_to_young) == 0);
        assert(gcptrlist_size(&d->public_to_young) == 0);
        assert(d->num_public_to_protected == 0);
        assert(gcptrlist_size(&d->stolen_objects) == 0);
    }
}

static struct stm_object_s dead_object_stub = {
    GCFLAG_PREBUILT | GCFLAG_STUB,
    (revision_t)&dead_object_stub
};

static void cleanup_for_thread(struct tx_descriptor *d)
{
    long i;
    gcptr *items = d->list_of_read_objects.items;

    if (d->active < 0)
        return;       /* already "aborted" during forced minor collection */

    for (i = d->list_of_read_objects.size - 1; i >= 0; --i) {
        gcptr obj = items[i];

        /* Warning: in case the object listed is outdated and has been
           replaced with a more recent revision, then it might be the
           case that obj->h_revision doesn't have GCFLAG_VISITED, but
           just removing it is very wrong --- we want 'd' to abort.
        */
        revision_t v = obj->h_revision;
        if (!(v & 1)) {  // "is a pointer"
            /* i.e. has a more recent revision.  Oups. */
            fprintf(stderr,
                    "ABRT_COLLECT_MAJOR: %p was read but modified already\n",
                    obj);
            if (d->max_aborts != 0) {           /* normal path */
                AbortTransactionAfterCollect(d, ABRT_COLLECT_MAJOR);
                return;
            }
            else {     /* for tests */
                items[i] = &dead_object_stub;
                continue;
            }
        }

        /* on the other hand, if we see a non-visited object in the read
           list, then we need to remove it --- it's wrong to just abort.
           Consider the following case: the transaction is inevitable,
           and since it started, it popped objects out of its shadow
           stack.  Some popped objects might become free even if they
           have been read from.  We must not abort such transactions
           (and cannot anyway: they are inevitable!). */
        if (!(obj->h_tid & GCFLAG_VISITED)) {
            items[i] = items[--d->list_of_read_objects.size];
        }
    }

    d->num_read_objects_known_old = d->list_of_read_objects.size;
    fxcache_clear(&d->recent_reads_cache);

    /* We are now after visiting all objects, and we know the
     * transaction isn't aborting because of this collection.  We have
     * cleared GCFLAG_PUBLIC_TO_PRIVATE from public objects at the end
     * of the chain.  Now we have to set it again on public objects that
     * have a private copy.
     */
    wlog_t *item;

    G2L_LOOP_FORWARD(d->public_to_private, item) {

        assert(stmgc_classify(item->addr) == K_PUBLIC);
        /*..rt(stmgc_classify(item->val)  == K_PRIVATE); but in the
            other thread, which becomes: */
        assert(item->val->h_revision == *d->local_revision_ref);

        item->addr->h_tid |= GCFLAG_PUBLIC_TO_PRIVATE;

    } G2L_LOOP_END;
}

static void clean_up_lists_of_read_objects_and_fix_outdated_flags(void)
{
    struct tx_descriptor *d;
    for (d = tx_head; d; d = d->tx_next)
        cleanup_for_thread(d);
}


/***** Major collections: sweeping *****/

static void sweep_pages(local_gcpages_t *gcp, int size_class,
                        page_header_t *lpage)
{
    int objs_per_page = nblocks_for_size[size_class];
    uintptr_t obj_size = size_class * WORD;
    gcptr freelist = gcp->free_loc_for_size[size_class];
    page_header_t *lpagenext;
    int j;
    gcptr p;

    for (; lpage; lpage = lpagenext) {
        lpagenext = lpage->next_page;
        /* sweep 'page': any object with GCFLAG_VISITED stays alive
           and the flag is removed; other locations are marked as free. */
        p = (gcptr)(lpage + 1);
        for (j = 0; j < objs_per_page; j++) {
            if (p->h_tid & GCFLAG_VISITED)
                break;  /* first object that stays alive */
            p = (gcptr)(((char *)p) + obj_size);
        }
        if (j < objs_per_page) {
            /* the page contains at least one object that stays alive */
            lpage->next_page = gcp->pages_for_size[size_class];
            gcp->pages_for_size[size_class] = lpage;
            p = (gcptr)(lpage + 1);
            for (j = 0; j < objs_per_page; j++) {
                if (p->h_tid & GCFLAG_VISITED) {
                    p->h_tid &= ~GCFLAG_VISITED;
                    mc_total_in_use += obj_size;
                }
                else {
#ifdef DUMP_EXTRA
                    if (p->h_tid != DEBUG_WORD(0xDD)) {
                        fprintf(stderr, "| freeing %p\n", p);
                    }
#endif
                    /* skip the assignment if compiled without asserts */
                    assert(!(GCFLAG_VISITED & DEBUG_WORD(0xDD)));
                    assert(p->h_tid = DEBUG_WORD(0xDD));
                    p->h_revision = (revision_t)freelist;
                    stm_dbgmem_not_used(p, size_class * WORD, 0);
                    freelist = p;
                }
                p = (gcptr)(((char *)p) + obj_size);
            }
            mc_total_reserved += obj_size * objs_per_page;
        }
        else {
            /* the page is fully free */
#ifdef DUMP_EXTRA
            p = (gcptr)(lpage + 1);
            for (j = 0; j < objs_per_page; j++) {
                assert(!(p->h_tid & GCFLAG_VISITED));
                if (p->h_tid != DEBUG_WORD(0xDD)) {
                    fprintf(stderr, "| freeing %p (with page %p)\n", p, lpage);
                }
                p = (gcptr)(((char *)p) + obj_size);
            }
#endif
            stm_free(lpage, GC_PAGE_SIZE);
            gcp->count_pages--;
        }
    }
    gcp->free_loc_for_size[size_class] = freelist;
}

static void free_unused_local_pages(local_gcpages_t *gcp)
{
    int i;
    page_header_t *lpage;

    for (i = 1; i < GC_SMALL_REQUESTS; i++) {
        lpage = gcp->pages_for_size[i];
        gcp->pages_for_size[i] = NULL;
        gcp->free_loc_for_size[i] = NULL;
        sweep_pages(gcp, i, lpage);
    }
}

static void free_all_unused_local_pages(void)
{
    struct tx_descriptor *d;
    for (d = tx_head; d; d = d->tx_next)
        free_unused_local_pages(d->local_gcpages);
}

static void free_unused_global_pages(void)
{
    int i;
    page_header_t *gpage;
    local_gcpages_t *gcp = LOCAL_GCPAGES();   /* randomly, take them for us */
    gcp->count_pages += count_global_pages;
    count_global_pages = 0;

    for (i = 1; i < GC_SMALL_REQUESTS; i++) {
        gpage = full_pages[i];
        full_pages[i] = NULL;
        sweep_pages(gcp, i, gpage);
    }

    while (finished_thread_gcpages != NULL) {
        local_gcpages_t *old = finished_thread_gcpages;
        finished_thread_gcpages = old->gcp_next;

        for (i = 1; i < GC_SMALL_REQUESTS; i++) {
            gpage = old->pages_for_size[i];
            sweep_pages(gcp, i, gpage);
        }
        stm_free(old, sizeof(local_gcpages_t));
    }
}


/***** Major collections: forcing minor collections *****/

void force_minor_collections(void)
{
    struct tx_descriptor *d;
    struct tx_descriptor *saved = thread_descriptor;
    revision_t saved_local_rev = stm_local_revision;
    assert(saved_local_rev == *saved->local_revision_ref);

    for (d = tx_head; d; d = d->tx_next) {
        /* Force a minor collection to run in the thread 'd'.
           Usually not needed, but it may be the case that this major
           collection was not preceeded by a minor collection if the
           thread is busy in a system call for example.
        */
        if (stmgc_minor_collect_anything_to_do(d)) {
            /* Hack: temporarily pretend that we "are" the other thread...
             */
            thread_descriptor = d;
            stm_local_revision = *d->local_revision_ref;
            assert(stmgc_nursery_hiding(d, 0));
            stmgc_minor_collect_no_abort();
            assert(stmgc_nursery_hiding(d, 1));
            thread_descriptor = saved;
            stm_local_revision = saved_local_rev;
        }
    }
}


/***** Major collections: main *****/

void update_next_threshold(void)
{
    uintptr_t free_space_in_pages, next;

    /* the limit will be reached when we have allocated 0.82 times mc_total */
    next = (uintptr_t)(mc_total_in_use * (GC_MAJOR_COLLECT-1.0));

    /* this limit should be at least GC_MIN */
    if (next < GC_MIN)
        next = GC_MIN;

    /* this difference gives the size allocated in pages but unused so far */
    assert(mc_total_in_use <= mc_total_reserved);
    free_space_in_pages = mc_total_reserved - mc_total_in_use;

    /* decrement 'next' by this much, because it will not be accounted for */
    if (next >= free_space_in_pages)
        next -= free_space_in_pages;
    else
        next = 0;

    /* allow for headroom: enforce the smallest allowed value */
    if (next < GC_EXPAND)
        next = GC_EXPAND;

    /* done */
    countdown_next_major_coll = next;
}

void stm_major_collect(void)
{
    stmgcpage_acquire_global_lock();
    fprintf(stderr, ",-----\n| running major collection...\n");

    force_minor_collections();

    assert(gcptrlist_size(&objects_to_trace) == 0);
    mark_prebuilt_roots();
    mark_all_stack_roots();
    visit_all_objects();
    gcptrlist_delete(&objects_to_trace);
    clean_up_lists_of_read_objects_and_fix_outdated_flags();

    mc_total_in_use = mc_total_reserved = 0;
    free_all_unused_local_pages();
    free_unused_global_pages();
    update_next_threshold();

    fprintf(stderr, "| %lu bytes alive, %lu not used, countdown %lu\n`-----\n",
            (unsigned long)mc_total_in_use,
            (unsigned long)(mc_total_reserved - mc_total_in_use),
            (unsigned long)countdown_next_major_coll);
    stmgcpage_release_global_lock();
}

void stmgcpage_possibly_major_collect(int force)
{
    if (force)
        stmgcpage_reduce_threshold((size_t)-1);

    /* If 'countdown_next_major_coll' reached 0, then run a major coll now. */
    if (ACCESS_ONCE(countdown_next_major_coll) > 0)
        return;

    stm_start_single_thread();

    /* If several threads were blocked on the previous line, the first
       one to proceed sees 0 in 'countdown_next_major_coll'.  It's the
       thread that will do the major collection.  Afterwards the other
       threads will also acquire the RW lock in exclusive mode, but won't
       do anything. */
    if (countdown_next_major_coll == 0)
        stm_major_collect();

    stm_stop_single_thread();

    AbortNowIfDelayed();
}


/***** Debugging *****/

static gcptr *_recdump_first;
static char _recdump_isptr[256];

void _recdump_visit(gcptr *pobj)
{
    long index = pobj - _recdump_first;
    if (0 <= index && index < sizeof _recdump_isptr)
        _recdump_isptr[index] = 1;
}

void recdump(gcptr obj)
{
    static char *gc_flag_names[] = GC_FLAG_NAMES;
    static int counter = 0;
    struct GcPtrList pending = {0};
    struct G2L seen = {0};
    unsigned long i, n;
    char **pp;
    gcptr *pobj;
    int count = 0;

    if (obj == NULL) {
        fprintf(stderr, "obj is NULL\n");
        return;
    }
    fprintf(stderr, "%d", counter++);

    assert((((revision_t)obj) & (sizeof(void*)-1)) == 0);   /* aligned */
    gcptrlist_insert(&pending, obj);
    g2l_insert(&seen, obj, obj);

    while (gcptrlist_size(&pending) > 0) {
        obj = gcptrlist_pop(&pending);
        fprintf(stderr, "\n%p:", obj);
        /* ^^^ write this line even if the following segfault */
        switch (stm_dbgmem_is_active(obj, 1)) {
        case 1:
            if (thread_descriptor &&
                    stmgc_is_young_in(thread_descriptor, obj)) {
                if (g2l_contains(
                       &thread_descriptor->young_objects_outside_nursery, obj))
                    fprintf(stderr, " (young but outside nursery)");
                else
                    fprintf(stderr, " (nursery)");
            }
            break;
        case -1: fprintf(stderr, " (unmanaged)"); break;
        default: fprintf(stderr, "\n   CANNOT ACCESS MEMORY!\n"); abort();
        }
        fprintf(stderr, "\n   tid\t%16lx", (unsigned long)obj->h_tid);
        count++;

        pp = gc_flag_names;
        for (i = STM_FIRST_GCFLAG; *pp != NULL; pp++, i <<= 1) {
            if (obj->h_tid & i) {
                fprintf(stderr, "  %s", *pp);
            }
        }
        fprintf(stderr, "\n   rev\t%16lx (%ld)\n",
                (unsigned long)obj->h_revision, (long)obj->h_revision);

        pobj = (gcptr *)(obj + 1);
        _recdump_first = pobj;
        memset(_recdump_isptr, 0, sizeof _recdump_isptr);

        if ((obj->h_tid & GCFLAG_STUB) == 0) {
            stmcb_trace(obj, &_recdump_visit);

            n = stmcb_size(obj) - sizeof(*obj);
            n = (n + sizeof(WORD) - 1) / sizeof(WORD);
        }
        else {
            n = 0;
        }
        for (i = 0; i < n; i++) {
            fprintf(stderr, "   [%lu]\t%16lx (%ld)\n", i,
                    (unsigned long)*pobj, (long)*pobj);
            if (_recdump_isptr[i] && *pobj &&
                !g2l_contains(&seen, *pobj)) {
                gcptrlist_insert(&pending, *pobj);
                g2l_insert(&seen, *pobj, *pobj);
            }
            pobj++;
        }

        if (obj->h_revision != 0 && (obj->h_revision & 1) == 0) {
            gcptr p = (gcptr)(obj->h_revision & ~2);
            if (!g2l_contains(&seen, p)) {
                g2l_insert(&seen, p, p);
                /* don't add 'p' if it's a young pointer belonging to some
                   other thread */
                if (!(obj->h_revision & 2) ||
                        (thread_descriptor &&
                         stmgc_is_young_in(thread_descriptor, p))) {
                    gcptrlist_insert(&pending, p);
                }
                else {
                    fprintf(stderr, "\n%p: (foreign young ptr)\n   ?\n", p);
                }
            }
        }
    }

    if (count > 1) {
        fprintf(stderr, "\n%d objects.\n", count);
    }
    gcptrlist_delete(&pending);
    g2l_delete(&seen);
}
