#include <stdarg.h>
#include "duhton.h"


DuType *Du_Types[_DUTYPE_TOTAL] = {
    NULL,                  // DUTYPE_INVALID       0
    &DuNone_Type,          // DUTYPE_NONE          1
    &DuInt_Type,           // DUTYPE_INT           2
    &DuSymbol_Type,        // DUTYPE_SYMBOL        3
    &DuCons_Type,          // DUTYPE_CONS          4
    &DuList_Type,          // DUTYPE_LIST          5
    &DuTuple_Type,         // DUTYPE_TUPLE         6
    &DuFrame_Type,         // DUTYPE_FRAME         7
    &DuFrameNode_Type,     // DUTYPE_FRAMENODE     8
    &DuContainer_Type,     // DUTYPE_CONTAINER     9
};


/* callback: get the size of an object */
size_t stmcb_size(struct object_s *obj)
{
    DuType *tp = Du_Types[((struct DuObject_s *)obj)->type_id];
    size_t result = tp->dt_size;
    if (result == 0)
        result = tp->dt_bytesize((struct DuObject_s *)obj);
    return result;
}

/* callback: trace the content of an object */
void stmcb_trace(struct object_s *obj, void visit(object_t **))
{
    DuType *tp = Du_Types[((struct DuObject_s *)obj)->type_id];
    trace_fn trace = tp->dt_trace;
    if (trace)
        trace((struct DuObject_s *)obj, visit);
}


DuObject *DuObject_New(DuType *tp)
{
    assert(tp->dt_size >= sizeof(DuObject));
    DuObject *ob = (DuObject *)stm_allocate(tp->dt_size);
    assert(ob);
    ob->type_id = tp->dt_typeindex;
    return ob;
}

void none_print(DuObject *ob)
{
    printf("None");
}

int none_is_true(DuObject *ob)
{
    return 0;
}

DuType DuNone_Type = {
    "NoneType",
    DUTYPE_NONE,
    sizeof(DuObject),
    (trace_fn)NULL,
    none_print,
    (eval_fn)NULL,
    none_is_true,
};

DuObject *Du_None;

void init_prebuilt_object_objects(void)
{
    Du_None = (DuObject *)stm_allocate_prebuilt(sizeof(DuObject));
    Du_None->type_id = DUTYPE_NONE;
}

void Du_FatalError(char *msg, ...)
{
    va_list ap;
    va_start(ap, msg);
    fprintf(stderr, "Error: ");
    vfprintf(stderr, msg, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    abort();
}

void Du_Print(DuObject *ob, int newline)
{
    Du_TYPE(ob)->dt_print(ob);
    if (newline)
        printf("\n");
}

int DuObject_IsTrue(DuObject *ob)
{
    len_fn fn = Du_TYPE(ob)->dt_is_true;
    if (!fn) fn = Du_TYPE(ob)->dt_length;
    if (!fn) return 1;
    return fn(ob) != 0;
}

int DuObject_Length(DuObject *ob)
{
    if (!Du_TYPE(ob)->dt_length)
        Du_FatalError("object of type '%s' has no length",
                      Du_TYPE(ob)->dt_name);
    return Du_TYPE(ob)->dt_length(ob);
}
