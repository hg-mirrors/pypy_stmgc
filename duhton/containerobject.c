#include "duhton.h"

typedef struct {
    DuOBJECT_HEAD
    DuObject *ob_reference;
} DuContainerObject;

void container_free(DuContainerObject *ob)
{
    DuObject *x = ob->ob_reference;
#ifdef Du_DEBUG
    ob->ob_reference = (DuObject *)0xDD;
#endif
    free(ob);
    Du_DECREF(x);
}

void container_print(DuContainerObject *ob)
{
    printf("<container ");
    Du_Print(ob->ob_reference, 0);
    printf(">");
}

DuObject *DuContainer_GetRef(DuObject *ob)
{
    DuObject *result;
    DuContainer_Ensure("DuContainer_GetRef", ob);

    Du_AME_READ(ob, (result = ((DuContainerObject *)ob)->ob_reference));

    Du_INCREF(result);
    return result;
}

void DuContainer_SetRef(DuObject *ob, DuObject *x)
{
    DuContainer_Ensure("DuContainer_SetRef", ob);

    Du_AME_WRITE(ob);
    DuObject *prev = ((DuContainerObject *)ob)->ob_reference;
    Du_INCREF(x);
    ((DuContainerObject *)ob)->ob_reference = x;
    Du_DECREF(prev);
}

DuTypeObject DuContainer_Type = {
    DuOBJECT_HEAD_INIT(&DuType_Type),
    "container",
    sizeof(DuContainerObject),
    (destructor_fn)container_free,
    (print_fn)container_print,
};

DuObject *DuContainer_New()
{
    DuContainerObject *ob =                                     \
        (DuContainerObject *)DuObject_New(&DuContainer_Type);
    Du_INCREF(Du_None);
    ob->ob_reference = Du_None;
    return (DuObject *)ob;
}

void DuContainer_Ensure(char *where, DuObject *ob)
{
    if (!DuContainer_Check(ob))
        Du_FatalError("%s: expected 'container' argument, got '%s'",
                      where, ob->ob_type->dt_name);
}
