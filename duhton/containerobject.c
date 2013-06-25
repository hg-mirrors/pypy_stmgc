#include "duhton.h"

typedef struct {
    DuOBJECT_HEAD
    DuObject *ob_reference;
} DuContainerObject;


void container_trace(DuContainerObject *ob, void visit(gcptr *))
{
    visit(&ob->ob_reference);
}

void container_print(DuContainerObject *ob)
{
    printf("<container ");
    Du_Print(ob->ob_reference, 0);
    printf(">");
}

DuObject *DuContainer_GetRef(DuObject *ob)
{
    DuContainer_Ensure("DuContainer_GetRef", ob);

    _du_read1(ob);
    return ((DuContainerObject *)ob)->ob_reference;
}

void DuContainer_SetRef(DuObject *ob, DuObject *x)
{
    DuContainer_Ensure("DuContainer_SetRef", ob);

    _du_write1(ob);
    ((DuContainerObject *)ob)->ob_reference = x;
}

DuType DuContainer_Type = {
    "container",
    DUTYPE_CONTAINER,
    sizeof(DuContainerObject),
    (trace_fn)container_trace,
    (print_fn)container_print,
};

DuObject *DuContainer_New(DuObject *x)
{
    _du_save1(x);
    DuContainerObject *ob =                                     \
        (DuContainerObject *)DuObject_New(&DuContainer_Type);
    _du_restore1(x);

    ob->ob_reference = x;
    return (DuObject *)ob;
}

void DuContainer_Ensure(char *where, DuObject *ob)
{
    if (!DuContainer_Check(ob))
        Du_FatalError("%s: expected 'container' argument, got '%s'",
                      where, Du_TYPE(ob)->dt_name);
}
