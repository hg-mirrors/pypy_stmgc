#include "duhton.h"
#include <sys/select.h>


DuObject *Du_Progn(DuObject *cons, DuObject *locals)
{
    DuObject *result = Du_None;
    Du_INCREF(Du_None);
    while (cons != Du_None) {
        DuObject *obj = Du_Eval(_DuCons_CAR(cons), locals);
        Du_DECREF(result);
        result = obj;
        cons = _DuCons_NEXT(cons);
    }
    return result;
}

DuObject *du_setq(DuObject *cons, DuObject *locals)
{
    DuObject *result = Du_None;
    Du_INCREF(Du_None);
    while (cons != Du_None) {
        DuObject *symbol = _DuCons_CAR(cons);
        cons = _DuCons_NEXT(cons);
        if (cons == Du_None)
            Du_FatalError("setq: number of arguments is odd");
        DuObject *expr = _DuCons_CAR(cons);
        DuObject *obj = Du_Eval(expr, locals);
        DuFrame_SetSymbol(locals, symbol, obj);
        Du_DECREF(result);
        result = obj;
        cons = _DuCons_NEXT(cons);
    }
    return result;
}

DuObject *du_print(DuObject *cons, DuObject *locals)
{
    DuObject *lst = DuList_New();

    while (cons != Du_None) {
        DuObject *obj = Du_Eval(_DuCons_CAR(cons), locals);
        DuList_Append(lst, obj);
        Du_DECREF(obj);
        cons = _DuCons_NEXT(cons);
    }

    Du_AME_TryInevitable();

    int i;
    for (i=0; i<DuList_Size(lst); i++) {
        if (i > 0) printf(" ");
        DuObject *obj = DuList_GetItem(lst, i);
        Du_Print(obj, 0);
        Du_DECREF(obj);
    }
    Du_DECREF(lst);

    printf("\n");
    Du_INCREF(Du_None);
    return Du_None;
}

DuObject *du_add(DuObject *cons, DuObject *locals)
{
    int result = 0;
    while (cons != Du_None) {
        DuObject *obj = Du_Eval(_DuCons_CAR(cons), locals);
        result += DuInt_AsInt(obj);
        Du_DECREF(obj);
        cons = _DuCons_NEXT(cons);
    }
    return DuInt_FromInt(result);
}

DuObject *du_sub(DuObject *cons, DuObject *locals)
{
    int result = 0;
    int sign = 1;
    while (cons != Du_None) {
        DuObject *obj = Du_Eval(_DuCons_CAR(cons), locals);
        result += sign * DuInt_AsInt(obj);
        sign = -1;
        Du_DECREF(obj);
        cons = _DuCons_NEXT(cons);
    }
    return DuInt_FromInt(result);
}

DuObject *du_mul(DuObject *cons, DuObject *locals)
{
    int result = 1;
    while (cons != Du_None) {
        DuObject *obj = Du_Eval(_DuCons_CAR(cons), locals);
        result *= DuInt_AsInt(obj);
        Du_DECREF(obj);
        cons = _DuCons_NEXT(cons);
    }
    return DuInt_FromInt(result);
}

static DuObject *_du_intcmp(DuObject *cons, DuObject *locals, int mode)
{
    if (cons == Du_None || _DuCons_NEXT(cons) == Du_None ||
        _DuCons_NEXT(_DuCons_NEXT(cons)) != Du_None)
        Du_FatalError("get: expected two arguments");
    DuObject *obj_a = Du_Eval(_DuCons_CAR(cons), locals);
    DuObject *obj_b = Du_Eval(_DuCons_CAR(_DuCons_NEXT(cons)), locals);
    int a = DuInt_AsInt(obj_a);
    int b = DuInt_AsInt(obj_b);
    Du_DECREF(obj_a);
    Du_DECREF(obj_b);
    int r = 0;
    switch (mode) {
    case 0: r = a < b; break;
    case 1: r = a <= b; break;
    case 2: r = a == b; break;
    case 3: r = a != b; break;
    case 4: r = a > b; break;
    case 5: r = a >= b; break;
    }
    return DuInt_FromInt(r);
}

DuObject *du_lt(DuObject *cons, DuObject *locals)
{ return _du_intcmp(cons, locals, 0); }
DuObject *du_le(DuObject *cons, DuObject *locals)
{ return _du_intcmp(cons, locals, 1); }
DuObject *du_eq(DuObject *cons, DuObject *locals)
{ return _du_intcmp(cons, locals, 2); }
DuObject *du_ne(DuObject *cons, DuObject *locals)
{ return _du_intcmp(cons, locals, 3); }
DuObject *du_gt(DuObject *cons, DuObject *locals)
{ return _du_intcmp(cons, locals, 4); }
DuObject *du_ge(DuObject *cons, DuObject *locals)
{ return _du_intcmp(cons, locals, 5); }

DuObject *du_type(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None || _DuCons_NEXT(cons) != Du_None)
        Du_FatalError("type: expected one argument");
    DuObject *obj = Du_Eval(_DuCons_CAR(cons), locals);
    DuObject *res = (DuObject *)(obj->ob_type);
    Du_INCREF(res);
    Du_DECREF(obj);
    return res;
}

DuObject *du_quote(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None || _DuCons_NEXT(cons) != Du_None)
        Du_FatalError("quote: expected one argument");
    DuObject *obj = _DuCons_CAR(cons);
    Du_INCREF(obj);
    return obj;
}

DuObject *du_list(DuObject *cons, DuObject *locals)
{
    DuObject *list = DuList_New();
    while (cons != Du_None) {
        DuObject *obj = Du_Eval(_DuCons_CAR(cons), locals);
        DuList_Append(list, obj);
        Du_DECREF(obj);
        cons = _DuCons_NEXT(cons);
    }
    return list;
}

DuObject *du_container(DuObject *cons, DuObject *locals)
{
    if (cons != Du_None && _DuCons_NEXT(cons) != Du_None)
        Du_FatalError("container: expected at most one argument");
    DuObject *container = DuContainer_New();
    if (cons != Du_None) {
        DuObject *obj = Du_Eval(_DuCons_CAR(cons), locals);
        DuContainer_SetRef(container, obj);
        Du_DECREF(obj);
    }
    return container;
}

DuObject *du_get(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None)
        Du_FatalError("get: expected at least one argument");
    DuObject *res;
    DuObject *obj = Du_Eval(_DuCons_CAR(cons), locals);

    if (DuList_Check(obj)) {
        if (_DuCons_NEXT(cons) == Du_None ||
            _DuCons_NEXT(_DuCons_NEXT(cons)) != Du_None)
            Du_FatalError("get with a list: expected two arguments");
        DuObject *index = Du_Eval(_DuCons_CAR(_DuCons_NEXT(cons)), locals);
        res = DuList_GetItem(obj, DuInt_AsInt(index));
        Du_DECREF(index);
    }
    else if (DuContainer_Check(obj)) {
        if (_DuCons_NEXT(cons) != Du_None)
            Du_FatalError("get with a container: expected one argument");
        res = DuContainer_GetRef(obj);
    }
    else
        Du_FatalError("get: bad argument type '%s'", obj->ob_type->dt_name);

    Du_DECREF(obj);
    return res;
}

DuObject *du_set(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None || _DuCons_NEXT(cons) == Du_None)
        Du_FatalError("set: expected at least two arguments");
    DuObject *obj = Du_Eval(_DuCons_CAR(cons), locals);

    if (DuList_Check(obj)) {
        if (_DuCons_NEXT(_DuCons_NEXT(cons)) == Du_None ||
            _DuCons_NEXT(_DuCons_NEXT(_DuCons_NEXT(cons))) != Du_None)
            Du_FatalError("set with a list: expected three arguments");
        DuObject *index = Du_Eval(_DuCons_CAR(_DuCons_NEXT(cons)), locals);
        DuObject *newobj = Du_Eval(
                    _DuCons_CAR(_DuCons_NEXT(_DuCons_NEXT(cons))), locals);
        DuList_SetItem(obj, DuInt_AsInt(index), newobj);
        Du_DECREF(index);
        Du_DECREF(newobj);
    }
    else if (DuContainer_Check(obj)) {
        if (_DuCons_NEXT(_DuCons_NEXT(cons)) != Du_None)
            Du_FatalError("set with a container: expected two arguments");
        DuObject *newobj = Du_Eval(_DuCons_CAR(_DuCons_NEXT(cons)), locals);
        DuContainer_SetRef(obj, newobj);
        Du_DECREF(newobj);
    }
    else
        Du_FatalError("set: bad argument type '%s'", obj->ob_type->dt_name);

    Du_DECREF(obj);
    Du_INCREF(Du_None);
    return Du_None;
}

DuObject *du_append(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None || _DuCons_NEXT(cons) == Du_None ||
        _DuCons_NEXT(_DuCons_NEXT(cons)) != Du_None)
        Du_FatalError("append: expected two arguments");
    DuObject *lst = Du_Eval(_DuCons_CAR(cons), locals);
    DuObject *newobj = Du_Eval(_DuCons_CAR(_DuCons_NEXT(cons)), locals);
    DuList_Append(lst, newobj);
    Du_DECREF(lst);
    return newobj;
}

DuObject *du_pop(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None)
        Du_FatalError("pop: expected at least one argument");
    DuObject *lst = Du_Eval(_DuCons_CAR(cons), locals);
    int index;
    if (_DuCons_NEXT(cons) == Du_None) {
        index = DuList_Size(lst) - 1;
        if (index < 0)
            Du_FatalError("pop: empty list");
    }
    else if (_DuCons_NEXT(_DuCons_NEXT(cons)) == Du_None) {
        DuObject *indexobj = Du_Eval(_DuCons_CAR(_DuCons_NEXT(cons)), locals);
        index = DuInt_AsInt(indexobj);
        Du_DECREF(indexobj);
    }
    else
        Du_FatalError("pop: expected at most two arguments");

    DuObject *res = DuList_Pop(lst, index);
    Du_DECREF(lst);
    return res;
}

DuObject *du_len(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None || _DuCons_NEXT(cons) != Du_None)
        Du_FatalError("len: expected one argument");
    DuObject *obj = Du_Eval(_DuCons_CAR(cons), locals);
    int length = DuObject_Length(obj);
    Du_DECREF(obj);
    return DuInt_FromInt(length);
}

DuObject *du_if(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None || _DuCons_NEXT(cons) == Du_None)
        Du_FatalError("if: expected at least two arguments");
    DuObject *cond = Du_Eval(_DuCons_CAR(cons), locals);
    int cond_int = DuObject_IsTrue(cond);
    Du_DECREF(cond);
    if (cond_int != 0) {
        /* true path */
        return Du_Eval(_DuCons_CAR(_DuCons_NEXT(cons)), locals);
    }
    else {
        /* false path */
        return Du_Progn(_DuCons_NEXT(_DuCons_NEXT(cons)), locals);
    }
}

DuObject *du_while(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None)
        Du_FatalError("while: expected at least one argument");
    while (1) {
        DuObject *cond = Du_Eval(_DuCons_CAR(cons), locals);
        int cond_int = DuObject_IsTrue(cond);
        Du_DECREF(cond);
        if (cond_int == 0)
            break;
        DuObject *res = Du_Progn(_DuCons_NEXT(cons), locals);
        Du_DECREF(res);
    }
    Du_INCREF(Du_None);
    return Du_None;
}

DuObject *du_defun(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None || _DuCons_NEXT(cons) == Du_None)
        Du_FatalError("defun: expected at least two arguments");
    DuObject *name = _DuCons_CAR(cons);
    DuObject *arglist = _DuCons_CAR(_DuCons_NEXT(cons));
    DuObject *progn = _DuCons_NEXT(_DuCons_NEXT(cons));
    DuFrame_SetUserFunction(locals, name, arglist, progn);
    Du_INCREF(Du_None);
    return Du_None;
}

DuObject *du_car(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None || _DuCons_NEXT(cons) != Du_None)
        Du_FatalError("car: expected one argument");
    DuObject *obj = Du_Eval(_DuCons_CAR(cons), locals);
    DuObject *res = DuCons_Car(obj);
    Du_DECREF(obj);
    return res;
}

DuObject *du_cdr(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None || _DuCons_NEXT(cons) != Du_None)
        Du_FatalError("car: expected one argument");
    DuObject *obj = Du_Eval(_DuCons_CAR(cons), locals);
    DuObject *res = DuCons_Cdr(obj);
    Du_DECREF(obj);
    return res;
}

DuObject *du_not(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None || _DuCons_NEXT(cons) != Du_None)
        Du_FatalError("not: expected one argument");
    DuObject *obj = Du_Eval(_DuCons_CAR(cons), locals);
    int res = !DuObject_IsTrue(obj);
    Du_DECREF(obj);
    return DuInt_FromInt(res);
}

DuObject *du_transaction(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None)
        Du_FatalError("transaction: expected at least one argument");
    DuObject *sym = _DuCons_CAR(cons);
    DuObject *rest = _DuCons_NEXT(cons);
    _DuFrame_EvalCall(locals, sym, rest, 0);
    Du_INCREF(Du_None);
    return Du_None;
}

DuObject *du_sleepms(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None || _DuCons_NEXT(cons) != Du_None)
        Du_FatalError("sleepms: expected one argument");
    DuObject *obj = Du_Eval(_DuCons_CAR(cons), locals);
    int ms = DuInt_AsInt(obj);
    Du_DECREF(obj);

    struct timeval t;
    t.tv_sec = ms / 1000;
    t.tv_usec = (ms % 1000) * 1000;
    select(0, (fd_set *)0, (fd_set *)0, (fd_set *)0, &t);

    Du_INCREF(Du_None);
    return Du_None;
}

DuObject *du_defined(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None || _DuCons_NEXT(cons) != Du_None)
        Du_FatalError("defined?: expected one argument");
    DuObject *ob = _DuCons_CAR(cons);
    DuObject *res = DuFrame_GetSymbol(locals, ob);
    if (res == NULL)
        DuFrame_GetSymbol(Du_Globals, ob);
    if (res != NULL)
        Du_DECREF(res);
    return DuInt_FromInt(res != NULL);
}

DuObject *du_assert(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None || _DuCons_NEXT(cons) != Du_None)
        Du_FatalError("defined?: expected one argument");
    DuObject *obj = Du_Eval(_DuCons_CAR(cons), locals);
    if (!DuInt_AsInt(obj)) {
        printf("assert failed: ");
        Du_Print(_DuCons_CAR(cons), 1);
        Du_FatalError("assert failed");
    }
    Du_DECREF(obj);
    Du_INCREF(Du_None);
    return Du_None;
}

DuObject *Du_Globals;

void Du_Initialize(void)
{
    _Du_AME_InitThreadDescriptor();
    _Du_InitializeObjects();

    Du_Globals = DuFrame_New();
    DuFrame_SetBuiltinMacro(Du_Globals, "progn", Du_Progn);
    DuFrame_SetBuiltinMacro(Du_Globals, "setq", du_setq);
    DuFrame_SetBuiltinMacro(Du_Globals, "print", du_print);
    DuFrame_SetBuiltinMacro(Du_Globals, "+", du_add);
    DuFrame_SetBuiltinMacro(Du_Globals, "-", du_sub);
    DuFrame_SetBuiltinMacro(Du_Globals, "*", du_mul);
    DuFrame_SetBuiltinMacro(Du_Globals, "<", du_lt);
    DuFrame_SetBuiltinMacro(Du_Globals, "<=", du_le);
    DuFrame_SetBuiltinMacro(Du_Globals, "==", du_eq);
    DuFrame_SetBuiltinMacro(Du_Globals, "!=", du_ne);
    DuFrame_SetBuiltinMacro(Du_Globals, ">", du_gt);
    DuFrame_SetBuiltinMacro(Du_Globals, ">=", du_ge);
    DuFrame_SetBuiltinMacro(Du_Globals, "type", du_type);
    DuFrame_SetBuiltinMacro(Du_Globals, "quote", du_quote);
    DuFrame_SetBuiltinMacro(Du_Globals, "list", du_list);
    DuFrame_SetBuiltinMacro(Du_Globals, "container", du_container);
    DuFrame_SetBuiltinMacro(Du_Globals, "get", du_get);
    DuFrame_SetBuiltinMacro(Du_Globals, "set", du_set);
    DuFrame_SetBuiltinMacro(Du_Globals, "append", du_append);
    DuFrame_SetBuiltinMacro(Du_Globals, "pop", du_pop);
    DuFrame_SetBuiltinMacro(Du_Globals, "len", du_len);
    DuFrame_SetBuiltinMacro(Du_Globals, "if", du_if);
    DuFrame_SetBuiltinMacro(Du_Globals, "while", du_while);
    DuFrame_SetBuiltinMacro(Du_Globals, "defun", du_defun);
    DuFrame_SetBuiltinMacro(Du_Globals, "car", du_car);
    DuFrame_SetBuiltinMacro(Du_Globals, "cdr", du_cdr);
    DuFrame_SetBuiltinMacro(Du_Globals, "not", du_not);
    DuFrame_SetBuiltinMacro(Du_Globals, "transaction", du_transaction);
    DuFrame_SetBuiltinMacro(Du_Globals, "sleepms", du_sleepms);
    DuFrame_SetBuiltinMacro(Du_Globals, "defined?", du_defined);
    DuFrame_SetBuiltinMacro(Du_Globals, "assert", du_assert);
    DuFrame_SetSymbolStr(Du_Globals, "None", Du_None);
}

void Du_Finalize(void)
{
    Du_DECREF(Du_Globals);
    Du_Globals = NULL;

    _Du_FinalizeObjects();
    _Du_AME_FiniThreadDescriptor();
}
