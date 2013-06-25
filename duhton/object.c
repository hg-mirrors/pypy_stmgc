#include <stdarg.h>
#include "duhton.h"


DuType *Du_Types[_DUTYPE_TOTAL] = {
    NULL,
    &DuNone_Type,
    &DuInt_Type,
    &DuSymbol_Type,
    &DuCons_Type,
    &DuList_Type,
    &DuFrame_Type,
    &DuContainer_Type,
};


/* callback: get the size of an object */
size_t stmcb_size(gcptr obj)
{
    if (_DuObject_TypeNum(obj) == DUTYPE_TUPLE)
        return _DuTuple_ByteSize(obj);
    else
        return Du_TYPE(obj)->dt_size;
}

/* callback: trace the content of an object */
void stmcb_trace(gcptr obj, void visit(gcptr *))
{
    trace_fn trace = Du_TYPE(obj)->dt_trace;
    if (trace)
        trace(obj, visit);
}


DuObject *DuObject_New(DuType *tp)
{
    assert(tp->dt_size >= sizeof(DuObject));
    DuObject *ob = stm_allocate(tp->dt_size, tp->dt_typeindex);
    assert(ob);
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

DuObject _Du_NoneStruct =
    DuOBJECT_HEAD_INIT(DUTYPE_NONE);

void Du_FatalError(char *msg, ...)
{
    va_list ap;
    va_start(ap, msg);
    fprintf(stderr, "Error: ");
    vfprintf(stderr, msg, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
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
