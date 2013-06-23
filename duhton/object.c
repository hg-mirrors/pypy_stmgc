#include <stdarg.h>
#include "duhton.h"


static __thread DuObject chainedlist;

DuObject *DuObject_New(DuTypeObject *tp)
{
    assert(tp->dt_size >= sizeof(DuObject));
    DuObject *ob = malloc(tp->dt_size);
    assert(ob);
    ob->ob_refcnt = 1;
    ob->ob_type = tp;
    _Du_NewReference(ob);
    return ob;
}

void _Du_Dealloc(DuObject *ob)
{
    assert(ob->ob_refcnt == 1);
    destructor_fn destructor = ob->ob_type->dt_destructor;
    assert(destructor != NULL);
    _Du_ForgetReference(ob);
    destructor(ob);
}

#ifdef Du_TRACK_REFS
void _Du_NewReference(DuObject *ob)
{
    if (chainedlist.ob_debug_prev == NULL)
        Du_FatalError("Du_Initialize() must be called first");
    ob->ob_debug_prev = &chainedlist;
    ob->ob_debug_next = chainedlist.ob_debug_next;
#ifdef Du_AME
    ob->ob_version = 0;
#endif
    chainedlist.ob_debug_next = ob;
    ob->ob_debug_next->ob_debug_prev = ob;
}

void _Du_ForgetReference(DuObject *ob)
{
    ob->ob_debug_prev->ob_debug_next = ob->ob_debug_next;
    ob->ob_debug_next->ob_debug_prev = ob->ob_debug_prev;
    ob->ob_debug_prev = NULL;
    ob->ob_debug_next = NULL;
}
#endif

void _Du_BecomeImmortal(DuObject *ob)
{
    _Du_ForgetReference(ob);
    ob->ob_refcnt = -1;
}

void type_print(DuTypeObject *ob)
{
    printf("<type '%s'>", ob->dt_name);
}

DuTypeObject DuType_Type = {
    DuOBJECT_HEAD_INIT(&DuType_Type),
    "type",
    0,
    NULL,
    (print_fn)type_print,
};

void none_print(DuObject *ob)
{
    printf("None");
}

int none_is_true(DuObject *ob)
{
    return 0;
}

DuTypeObject DuNone_Type = {
    DuOBJECT_HEAD_INIT(&DuType_Type),
    "NoneType",
    sizeof(DuObject),
    NULL,
    none_print,
    (eval_fn)NULL,
    none_is_true,
};

DuObject _Du_NoneStruct =
    DuOBJECT_HEAD_INIT(&DuNone_Type);

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
    ob->ob_type->dt_print(ob);
    if (newline)
        printf("\n");
}

int DuObject_IsTrue(DuObject *ob)
{
    len_fn fn = ob->ob_type->dt_is_true;
    if (!fn) fn = ob->ob_type->dt_length;
    if (!fn) return 1;
    return fn(ob) != 0;
}

int DuObject_Length(DuObject *ob)
{
    if (!ob->ob_type->dt_length)
        Du_FatalError("object of type '%s' has no length",
                      ob->ob_type->dt_name);
    return ob->ob_type->dt_length(ob);
}

void _Du_InitializeObjects(void)
{
#ifdef Du_TRACK_REFS
    chainedlist.ob_debug_prev = &chainedlist;
    chainedlist.ob_debug_next = &chainedlist;
#endif
}

void _Du_FinalizeObjects(void)
{
#ifdef Du_DEBUG
    DuObject *obj;
    for (obj = chainedlist.ob_debug_next;
         obj != &chainedlist;
         obj = obj->ob_debug_next) {
        printf("NOT FREED: ");
        Du_Print(obj, 1);
    }
#endif
#ifdef Du_TRACK_REFS
    chainedlist.ob_debug_prev = NULL;
    chainedlist.ob_debug_next = NULL;
#endif
}

#ifdef Du_TRACK_REFS
void _Du_MakeImmortal(void)
{
    DuObject *end_marker = &chainedlist;
    DuObject *obj = end_marker;
    do {
        DuObject *next = obj->ob_debug_next;
        obj->ob_refcnt = -1;
        obj->ob_debug_prev = NULL;
        obj->ob_debug_next = NULL;
        obj = next;
    } while (obj != end_marker);
}
#endif
