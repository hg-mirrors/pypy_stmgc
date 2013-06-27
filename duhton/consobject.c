#include "duhton.h"


void cons_trace(DuConsObject *ob, void visit(gcptr *))
{
    visit(&ob->car);
    visit(&ob->cdr);
}

void cons_print(DuConsObject *ob)
{
    DuObject *p;
    printf("( ");
    while (1) {
        _du_read1(ob);
        _du_save1(ob);
        Du_Print(ob->car, 0);
        _du_restore1(ob);
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
    _du_read1(ob);
    return _DuFrame_EvalCall(locals, ob->car, ob->cdr, 1);
}

DuType DuCons_Type = {
    "cons",
    DUTYPE_CONS,
    sizeof(DuConsObject),
    (trace_fn)cons_trace,
    (print_fn)cons_print,
    (eval_fn)cons_eval,
};

DuObject *DuCons_New(DuObject *car, DuObject *cdr)
{
    _du_save2(car, cdr);
    DuConsObject *ob = (DuConsObject *)DuObject_New(&DuCons_Type);
    _du_restore2(car, cdr);
    ob->car = car;
    ob->cdr = cdr;
    return (DuObject *)ob;
}

DuObject *DuCons_Car(DuObject *cons)
{
    DuCons_Ensure("DuCons_Car", cons);
    _du_read1(cons);
    return ((DuConsObject *)cons)->car;
}

DuObject *DuCons_Cdr(DuObject *cons)
{
    DuCons_Ensure("DuCons_Cdr", cons);
    _du_read1(cons);
    return ((DuConsObject *)cons)->cdr;
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
                      where, Du_TYPE(ob)->dt_name);
}
