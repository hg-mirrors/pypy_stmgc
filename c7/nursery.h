


object_t *stm_allocate_prebuilt(size_t size);
object_t *_stm_allocate_old(size_t size);
object_t *stm_allocate(size_t size);

void _stm_minor_collect();
bool _stm_is_young(object_t *o);

void nursery_on_abort();
void nursery_on_commit();
void nursery_on_start();



extern uintptr_t index_page_never_used;
    

