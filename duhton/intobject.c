#include "duhton.h"

typedef struct {
    DuOBJECT_HEAD
    int ob_intval;
} DuIntObject;

void int_print(DuIntObject *ob)
{
    printf("%d", ob->ob_intval);
}

int int_is_true(DuIntObject *ob)
{
    return ob->ob_intval;
}

DuTypeObject DuInt_Type = {
    DuOBJECT_HEAD_INIT(&DuType_Type),
    "int",
    sizeof(DuIntObject),
    (destructor_fn)free,
    (print_fn)int_print,
    (eval_fn)NULL,
    (len_fn)int_is_true,
    (len_fn)NULL,
};

DuObject *DuInt_FromInt(int value)
{
    DuIntObject *ob = (DuIntObject *)DuObject_New(&DuInt_Type);
    ob->ob_intval = value;
    return (DuObject *)ob;
}

int DuInt_AsInt(DuObject *ob)
{
    DuInt_Ensure("DuInt_AsInt", ob);
    return ((DuIntObject *)ob)->ob_intval;
}

void DuInt_Ensure(char *where, DuObject *ob)
{
    if (!DuInt_Check(ob))
        Du_FatalError("%s: expected 'int' argument, got '%s'",
                      where, ob->ob_type->dt_name);
}
