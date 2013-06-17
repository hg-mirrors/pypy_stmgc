#include "stmimpl.h"


/* In this file, we use size_t to measure various sizes that can poten-
 * tially be large.  Be careful that it's an unsigned type --- but it
 * is needed to represent more than 2GB on 32-bit machines (up to 4GB).
 */

/* This maps each small request size to the number of blocks of this size
   that fit in a page. */
static int nblocks_for_size[GC_SMALL_REQUESTS];

/* A mutex for major collections and other global operations */
static pthread_mutex_t mutex_gc_lock = PTHREAD_MUTEX_INITIALIZER;

/* A count-down: when it reaches 0, run the next major collection */
static revision_t countdown_next_major_coll = GC_MIN;

/* For statistics */
static size_t count_global_pages;

/* Only computed during a major collection */
static size_t mc_total_in_use, mc_total_reserved;

/* For tests */
long stmgcpage_count(int quantity)
{
    switch (quantity) {
    case 0: return count_global_pages;
    case 1: return LOCAL_GCPAGES()->count_pages;
    case 2: count_global_pages = 0; return 0;
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
    if (nblocks_for_size[1] == 0)
        init_global_data();

    /* Take back ownership of the pages currently assigned to
       LOCAL_GCPAGES that might come from a previous thread. */
}

void stmgcpage_done_tls(void)
{
    /* Send to the shared area all my pages.  For now we don't extract
       the information about which locations are free or not; we just
       leave it to the next major GC to figure them out. */
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
    struct tx_public_descriptor *gcp = LOCAL_GCPAGES();
    gcp->count_pages++;
    count_global_pages++;

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
        //stm_dbgmem_not_used(current, nsize, 0);
        current = next;
    }
    current->h_tid = DEBUG_WORD(0xDD);
    current->h_revision = (revision_t)gcp->free_loc_for_size[size_class];
    //stm_dbgmem_not_used(current, nsize, 0);
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
        struct tx_public_descriptor *gcp = LOCAL_GCPAGES();
        int size_class = (size + WORD - 1) / WORD;
        assert(0 < size_class && size_class < GC_SMALL_REQUESTS);

        /* The result is simply 'free_loc_for_size[size_class]' */
        result = gcp->free_loc_for_size[size_class];
        if (!result) {
            result = allocate_new_page(size_class);
        }
        gcp->free_loc_for_size[size_class] = (gcptr)result->h_revision;
        //stm_dbgmem_used_again(result, size_class * WORD, 0);
        return result;
    }
    else {
        fprintf(stderr, "XXX stmgcpage_malloc: too big!\n");
        abort();
    }
}

static unsigned char random_char = 42;

void stmgcpage_free(gcptr obj)
{
    size_t size = stmcb_size(obj);
    if (size <= GC_SMALL_REQUEST_THRESHOLD) {
        struct tx_public_descriptor *gcp = LOCAL_GCPAGES();
        int size_class = (size + WORD - 1) / WORD;
        assert(0 < size_class && size_class < GC_SMALL_REQUESTS);

        /* We simply re-add the object to the right chained list */
        assert(obj->h_tid = DEBUG_WORD(random_char++));
        obj->h_revision = (revision_t)gcp->free_loc_for_size[size_class];
        gcp->free_loc_for_size[size_class] = obj;
        //stm_dbgmem_not_used(obj, size_class * WORD, 0);
    }
    else {
        fprintf(stderr, "XXX stmgcpage_free: too big!\n");
        abort();
    }
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
    for (d = stm_tx_head; d; d = d->tx_next) {

        /* the roots pushed on the shadowstack */
        mark_roots(d->shadowstack, *d->shadowstack_end_ref);

#if 0
        /* the thread-local object */
        visit(d->thread_local_obj_ref);
#endif

        /* the current transaction's private copies of public objects */
        wlog_t *item;
        G2L_LOOP_FORWARD(d->public_to_private, item) {

            /* note that 'item->addr' is also in the read set, so if it was
               outdated, it will be found at that time */
            visit(&item->addr);
            visit(&item->val);

        } G2L_LOOP_END;

        /* make sure that the other lists are empty */
        assert(gcptrlist_size(&d->public_with_young_copy) == 0);
        assert(gcptrlist_size(&d->public_descriptor->stolen_objects) == 0);
        assert(gcptrlist_size(&d->public_descriptor->stolen_young_stubs) == 0);
        /* NOT NECESSARILY EMPTY:
           - list_of_read_objects
           - private_from_protected
           - public_to_private
           - old_objects_to_trace
        */
        assert(gcptrlist_size(&d->list_of_read_objects) ==
               d->num_read_objects_known_old);
        assert(gcptrlist_size(&d->private_from_protected) ==
               d->num_private_from_protected_known_old);
    }
}


/***** Major collections: sweeping *****/

static void sweep_pages(struct tx_public_descriptor *gcp, int size_class,
                        page_header_t *lpage)
{
    int objs_per_page = nblocks_for_size[size_class];
    revision_t obj_size = size_class * WORD;
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
                    //stm_dbgmem_not_used(p, size_class * WORD, 0);
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
            assert(gcp->count_pages > 0);
            assert(count_global_pages > 0);
            gcp->count_pages--;
            count_global_pages--;
        }
    }
    gcp->free_loc_for_size[size_class] = freelist;
}

static void free_unused_local_pages(struct tx_public_descriptor *gcp)
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
    for (d = stm_tx_head; d; d = d->tx_next) {
        free_unused_local_pages(d->public_descriptor);
    }
}

static void free_closed_thread_descriptors(void)
{
    int i;
    page_header_t *gpage;
    struct tx_public_descriptor *gcp;
    revision_t index = -1;

    while ((gcp = stm_get_free_public_descriptor(&index)) != NULL) {
        if (gcp->collection_lock == -1)
            continue;

        for (i = 1; i < GC_SMALL_REQUESTS; i++) {
            gpage = gcp->pages_for_size[i];
            sweep_pages(gcp, i, gpage);
        }
        assert(gcp->collection_lock == 0);
        gcp->collection_lock = -1;
        /* XXX ...stub_blocks... */
        assert(gcp->stolen_objects.size == 0);
        assert(gcp->stolen_young_stubs.size == 0);
        gcptrlist_delete(&gcp->stolen_objects);
        gcptrlist_delete(&gcp->stolen_young_stubs);
    }
}


/***** Major collections: main *****/

void stm_major_collect(void)
{
    stmgcpage_acquire_global_lock();
    fprintf(stderr, ",-----\n| running major collection...\n");

#if 0
    force_minor_collections();

    assert(gcptrlist_size(&objects_to_trace) == 0);
    mark_prebuilt_roots();
#endif
    mark_all_stack_roots();
    visit_all_objects();
#if 0
    gcptrlist_delete(&objects_to_trace);
    clean_up_lists_of_read_objects_and_fix_outdated_flags();
#endif

    mc_total_in_use = mc_total_reserved = 0;
    free_all_unused_local_pages();
#if 0
    free_unused_global_pages();
#endif
    free_closed_thread_descriptors();
#if 0
    update_next_threshold();
#endif

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
