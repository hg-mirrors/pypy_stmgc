#include "duhton.h"

typedef struct {
    DuOBJECT_HEAD
    DuObject *car, *cdr;
} DuConsObject;

void cons_free(DuConsObject *ob)
{
    Du_DECREF(ob->car);
    Du_DECREF(ob->cdr);
#ifdef Du_DEBUG
    ob->car = ob->cdr = (DuObject *)0xDD;
#endif
    free(ob);
}

void cons_print(DuConsObject *ob)
{
    DuObject *p;
    printf("( ");
    while (1) {
        Du_Print(ob->car, 0);
        p = ob->cdr;
        if (!DuCons_Check(p))
            break;
        ob = (DuConsObject *)p;
        printf(" ");
    }
    if (p != Du_None) {
        printf(" . ");
        Du_Print(p, 0);
    }
    printf(" )");
}



DuObject *cons_eval(DuConsObject *ob, DuObject *locals)
{
    return _DuFrame_EvalCall(locals, ob->car, ob->cdr, 1);
}

DuTypeObject DuCons_Type = {
    DuOBJECT_HEAD_INIT(&DuType_Type),
    "cons",
    sizeof(DuConsObject),
    (destructor_fn)cons_free,
    (print_fn)cons_print,
    (eval_fn)cons_eval,
};

DuObject *DuCons_New(DuObject *car, DuObject *cdr)
{
    DuConsObject *ob = (DuConsObject *)DuObject_New(&DuCons_Type);
    ob->car = car; Du_INCREF(car);
    ob->cdr = cdr; Du_INCREF(cdr);
    return (DuObject *)ob;
}

DuObject *DuCons_Car(DuObject *cons)
{
    DuCons_Ensure("DuCons_Car", cons);
    DuObject *res = ((DuConsObject *)cons)->car;
    Du_INCREF(res);
    return res;
}

DuObject *DuCons_Cdr(DuObject *cons)
{
    DuCons_Ensure("DuCons_Cdr", cons);
    DuObject *res = ((DuConsObject *)cons)->cdr;
    Du_INCREF(res);
    return res;
}

DuObject *_DuCons_CAR(DuObject *cons)
{
    assert(DuCons_Check(cons));
    return ((DuConsObject *)cons)->car;
}

DuObject *_DuCons_NEXT(DuObject *cons)
{
    assert(DuCons_Check(cons));
    DuObject *result = ((DuConsObject *)cons)->cdr;
    if (result != Du_None && !DuCons_Check(cons))
        Du_FatalError("_DuCons_NEXT: not a well-formed cons list");
    return result;
}

void DuCons_Ensure(char *where, DuObject *ob)
{
    if (!DuCons_Check(ob))
        Du_FatalError("%s: expected 'cons' argument, got '%s'",
                      where, ob->ob_type->dt_name);
}
