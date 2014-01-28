#include "duhton.h"
#include <sys/select.h>

pthread_t *all_threads;
int all_threads_count;

static void _du_getargs1(const char *name, DuObject *cons, DuObject *locals,
                         DuObject **a)
{
    DuObject *expr1, *obj1;

    if (cons == Du_None) goto error;

    /* _du_read1(cons); IMMUTABLE */
    expr1 = _DuCons_CAR(cons);
    cons = _DuCons_NEXT(cons);
    if (cons != Du_None) goto error;

    obj1 = Du_Eval(expr1, locals);
    *a = obj1;
    return;

 error:
    Du_FatalError("%s: expected one argument", name);
}

static void _du_getargs2(const char *name, DuObject *cons, DuObject *locals,
                         DuObject **a, DuObject **b)
{
    DuObject *expr1, *expr2, *obj1, *obj2;

    if (cons == Du_None) goto error;

    /* _du_read1(cons); IMMUTABLE */
    expr1 = _DuCons_CAR(cons);
    cons = _DuCons_NEXT(cons);
    if (cons == Du_None) goto error;

    /* _du_read1(cons); IMMUTABLE */
    expr2 = _DuCons_CAR(cons);
    cons = _DuCons_NEXT(cons);
    if (cons != Du_None) goto error;

    _du_save2(expr2, locals);
    obj1 = Du_Eval(expr1, locals);
    _du_restore2(expr2, locals);

    _du_save1(obj1);
    obj2 = Du_Eval(expr2, locals);
    _du_restore1(obj1);

    *a = obj1;
    *b = obj2;
    return;

 error:
    Du_FatalError("%s: expected two arguments", name);
}

/************************************************************/


DuObject *Du_Progn(DuObject *cons, DuObject *locals)
{
    DuObject *result = Du_None;
    while (cons != Du_None) {
        /* _du_read1(cons); IMMUTABLE */
        DuObject *expr = _DuCons_CAR(cons);
        DuObject *next = _DuCons_NEXT(cons);
        _du_save2(next, locals);
        result = Du_Eval(expr, locals);
        _du_restore2(next, locals);
        cons = next;
    }
    return result;
}

DuObject *du_setq(DuObject *cons, DuObject *locals)
{
    DuObject *result = Du_None;
    while (cons != Du_None) {
        /* _du_read1(cons); IMMUTABLE */
        DuObject *symbol = _DuCons_CAR(cons);
        cons = _DuCons_NEXT(cons);
        if (cons == Du_None)
            Du_FatalError("setq: number of arguments is odd");
        /* _du_read1(cons); IMMUTABLE */
        DuObject *expr = _DuCons_CAR(cons);
        DuObject *next = _DuCons_NEXT(cons);

        _du_save3(symbol, next, locals);
        DuObject *obj = Du_Eval(expr, locals);
        _du_restore3(symbol, next, locals);

        _du_save3(next, locals, obj);
        DuFrame_SetSymbol(locals, symbol, obj);
        _du_restore3(next, locals, obj);

        result = obj;
        cons = next;
    }
    return result;
}

DuObject *du_print(DuObject *cons, DuObject *locals)
{
    _du_save2(cons, locals);
    DuObject *lst = DuList_New();
    _du_restore2(cons, locals);

    while (cons != Du_None) {
        /* _du_read1(cons); IMMUTABLE */
        DuObject *expr = _DuCons_CAR(cons);
        DuObject *next = _DuCons_NEXT(cons);

        _du_save3(lst, next, locals);
        DuObject *obj = Du_Eval(expr, locals);
        _du_restore3(lst, next, locals);

        _du_save3(lst, next, locals);
        DuList_Append(lst, obj);
        _du_restore3(lst, next, locals);

        cons = next;
    }

    _du_save1(lst);
    stm_become_inevitable("print");
    _du_restore1(lst);

    int i;
    for (i=0; i<DuList_Size(lst); i++) {
        if (i > 0) printf(" ");
        _du_save1(lst);
        Du_Print(DuList_GetItem(lst, i), 0);
        _du_restore1(lst);
    }

    printf("\n");
    return Du_None;
}

DuObject *du_xor(DuObject *cons, DuObject *locals)
{
    int result = 0;
    /* _du_read1(cons); IMMUTABLE */
    DuObject *expr = _DuCons_CAR(cons);
    DuObject *next = _DuCons_NEXT(cons);
        
    _du_save2(next, locals);
    DuObject *obj = Du_Eval(expr, locals);
    result = DuInt_AsInt(obj);
    _du_restore2(next, locals);
    
    cons = next;

    while (cons != Du_None) {
        /* _du_read1(cons); IMMUTABLE */
        expr = _DuCons_CAR(cons);
        next = _DuCons_NEXT(cons);

        _du_save2(next, locals);
        obj = Du_Eval(expr, locals);
        result ^= DuInt_AsInt(obj);
        _du_restore2(next, locals);

        cons = next;
    }
    
    return DuInt_FromInt(result);
}

DuObject *du_lshift(DuObject *cons, DuObject *locals)
{
    int result = 0;
    /* _du_read1(cons); IMMUTABLE */
    DuObject *expr = _DuCons_CAR(cons);
    DuObject *next = _DuCons_NEXT(cons);
        
    _du_save2(next, locals);
    DuObject *obj = Du_Eval(expr, locals);
    result = DuInt_AsInt(obj);
    _du_restore2(next, locals);
    
    cons = next;

    while (cons != Du_None) {
        /* _du_read1(cons); IMMUTABLE */
        expr = _DuCons_CAR(cons);
        next = _DuCons_NEXT(cons);

        _du_save2(next, locals);
        obj = Du_Eval(expr, locals);
        result <<= DuInt_AsInt(obj);
        _du_restore2(next, locals);

        cons = next;
    }
    
    return DuInt_FromInt(result);
}

DuObject *du_rshift(DuObject *cons, DuObject *locals)
{
    int result = 0;
    /* _du_read1(cons); IMMUTABLE */
    DuObject *expr = _DuCons_CAR(cons);
    DuObject *next = _DuCons_NEXT(cons);
        
    _du_save2(next, locals);
    DuObject *obj = Du_Eval(expr, locals);
    result = DuInt_AsInt(obj);
    _du_restore2(next, locals);
    
    cons = next;

    while (cons != Du_None) {
        /* _du_read1(cons); IMMUTABLE */
        expr = _DuCons_CAR(cons);
        next = _DuCons_NEXT(cons);

        _du_save2(next, locals);
        obj = Du_Eval(expr, locals);
        result >>= DuInt_AsInt(obj);
        _du_restore2(next, locals);

        cons = next;
    }
    
    return DuInt_FromInt(result);
}

DuObject *du_add(DuObject *cons, DuObject *locals)
{
    int result = 0;
    while (cons != Du_None) {
        /* _du_read1(cons); IMMUTABLE */
        DuObject *expr = _DuCons_CAR(cons);
        DuObject *next = _DuCons_NEXT(cons);

        _du_save2(next, locals);
        DuObject *obj = Du_Eval(expr, locals);
        result += DuInt_AsInt(obj);
        _du_restore2(next, locals);

        cons = next;
    }
    return DuInt_FromInt(result);
}

DuObject *du_sub(DuObject *cons, DuObject *locals)
{
    int result = 0;
    int sign = 1;
    while (cons != Du_None) {
        /* _du_read1(cons); IMMUTABLE */
        DuObject *expr = _DuCons_CAR(cons);
        DuObject *next = _DuCons_NEXT(cons);

        _du_save2(next, locals);
        DuObject *obj = Du_Eval(expr, locals);
        result += sign * DuInt_AsInt(obj);
        _du_restore2(next, locals);

        sign = -1;
        cons = next;
    }
    return DuInt_FromInt(result);
}

DuObject *du_mul(DuObject *cons, DuObject *locals)
{
    int result = 1;
    while (cons != Du_None) {
        /* _du_read1(cons); IMMUTABLE */
        DuObject *expr = _DuCons_CAR(cons);
        DuObject *next = _DuCons_NEXT(cons);

        _du_save2(next, locals);
        DuObject *obj = Du_Eval(expr, locals);
        result *= DuInt_AsInt(obj);
        _du_restore2(next, locals);

        cons = next;
    }
    return DuInt_FromInt(result);
}

DuObject *du_div(DuObject *cons, DuObject *locals)
{
    int result = 0;
    int first = 1;

    while (cons != Du_None) {
        /* _du_read1(cons); IMMUTABLE */
        DuObject *expr = _DuCons_CAR(cons);
        DuObject *next = _DuCons_NEXT(cons);

        _du_save2(next, locals);
        DuObject *obj = Du_Eval(expr, locals);
        if (first) {
            result = DuInt_AsInt(obj);
            first = 0;
        } else {
            result /= DuInt_AsInt(obj);
        }
        _du_restore2(next, locals);

        cons = next;
    }
    return DuInt_FromInt(result);
}

DuObject *du_mod(DuObject *cons, DuObject *locals)
{
    int result = 0;
    int first = 1;

    while (cons != Du_None) {
        /* _du_read1(cons); IMMUTABLE */
        DuObject *expr = _DuCons_CAR(cons);
        DuObject *next = _DuCons_NEXT(cons);

        _du_save2(next, locals);
        DuObject *obj = Du_Eval(expr, locals);
        if (first) {
            result = DuInt_AsInt(obj);
            first = 0;
        } else {
            result %= DuInt_AsInt(obj);
        }
        _du_restore2(next, locals);

        cons = next;
    }
    return DuInt_FromInt(result);
}


static DuObject *_du_intcmp(DuObject *cons, DuObject *locals, int mode)
{
    DuObject *obj_a, *obj_b;
    _du_getargs2("comparison", cons, locals, &obj_a, &obj_b);

    int a = DuInt_AsInt(obj_a);
    int b = DuInt_AsInt(obj_b);
    int r = 0;
    switch (mode) {
    case 0: r = a < b; break;
    case 1: r = a <= b; break;
    case 2: r = a == b; break;
    case 3: r = a != b; break;
    case 4: r = a > b; break;
    case 5: r = a >= b; break;
    case 6: r = a && b; break;
    case 7: r = a || b; break;
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
DuObject *du_and(DuObject *cons, DuObject *locals)
{ return _du_intcmp(cons, locals, 6); }
DuObject *du_or(DuObject *cons, DuObject *locals)
{ return _du_intcmp(cons, locals, 7); }



DuObject *du_type(DuObject *cons, DuObject *locals)
{
    DuObject *obj;
    _du_getargs1("type", cons, locals, &obj);

    return DuSymbol_FromString(Du_TYPE(obj)->dt_name);
}

DuObject *du_quote(DuObject *cons, DuObject *locals)
{
    /* _du_read1(cons); IMMUTABLE */
    if (cons == Du_None || _DuCons_NEXT(cons) != Du_None)
        Du_FatalError("quote: expected one argument");
    return _DuCons_CAR(cons);
}

DuObject *du_list(DuObject *cons, DuObject *locals)
{
    _du_save2(cons, locals);
    DuObject *list = DuList_New();
    _du_restore2(cons, locals);
    while (cons != Du_None) {
        /* _du_read1(cons); IMMUTABLE */
        DuObject *expr = _DuCons_CAR(cons);
        DuObject *next = _DuCons_NEXT(cons);

        _du_save3(list, next, locals);
        DuObject *obj = Du_Eval(expr, locals);
        _du_restore3(list, next, locals);

        _du_save3(list, next, locals);
        DuList_Append(list, obj);
        _du_restore3(list, next, locals);

        cons = next;
    }
    return list;
}

DuObject *du_container(DuObject *cons, DuObject *locals)
{
    DuObject *obj;

    if (cons == Du_None)
        obj = Du_None;
    else
        _du_getargs1("container", cons, locals, &obj);

    DuObject *container = DuContainer_New(obj);
    return container;
}

DuObject *du_get(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None)
        Du_FatalError("get: expected at least one argument");

    /* _du_read1(cons); IMMUTABLE */
    DuObject *expr = _DuCons_CAR(cons);
    DuObject *next = _DuCons_NEXT(cons);

    _du_save2(next, locals);
    DuObject *obj = Du_Eval(expr, locals);
    _du_restore2(next, locals);

    if (DuList_Check(obj)) {
        /* _du_read1(next); IMMUTABLE */
        if (next == Du_None || _DuCons_NEXT(next) != Du_None)
            Du_FatalError("get with a list: expected two arguments");

        _du_save1(obj);
        DuObject *index = Du_Eval(_DuCons_CAR(next), locals);
        _du_restore1(obj);

        return DuList_GetItem(obj, DuInt_AsInt(index));
    }
    else if (DuContainer_Check(obj)) {
        if (next != Du_None)
            Du_FatalError("get with a container: expected one argument");

        return DuContainer_GetRef(obj);
    }
    else
        Du_FatalError("get: bad argument type '%s'", Du_TYPE(obj)->dt_name);
}

DuObject *du_set(DuObject *cons, DuObject *locals)
{
    /* _du_read1(cons); IMMUTABLE */
    if (cons == Du_None || _DuCons_NEXT(cons) == Du_None)
        Du_FatalError("set: expected at least two arguments");

    DuObject *expr = _DuCons_CAR(cons);
    DuObject *next = _DuCons_NEXT(cons);

    _du_save2(next, locals);
    DuObject *obj = Du_Eval(expr, locals);
    _du_restore2(next, locals);

    /* _du_read1(next); IMMUTABLE */
    DuObject *expr2 = _DuCons_CAR(next);
    DuObject *next2 = _DuCons_NEXT(next);

    if (DuList_Check(obj)) {
        /* _du_read1(next2); IMMUTABLE */
        if (next2 == Du_None || _DuCons_NEXT(next2) != Du_None)
            Du_FatalError("set with a list: expected three arguments");

        _du_save3(obj, next2, locals);
        DuObject *index = Du_Eval(expr2, locals);
        _du_restore3(obj, next2, locals);

        _du_save2(obj, index);
        DuObject *newobj = Du_Eval(_DuCons_CAR(next2), locals);
        _du_restore2(obj, index);

        DuList_SetItem(obj, DuInt_AsInt(index), newobj);
    }
    else if (DuContainer_Check(obj)) {
        if (next2 != Du_None)
            Du_FatalError("set with a container: expected two arguments");

        _du_save1(obj);
        DuObject *newobj = Du_Eval(expr2, locals);
        _du_restore1(obj);

        DuContainer_SetRef(obj, newobj);
    }
    else
        Du_FatalError("set: bad argument type '%s'", Du_TYPE(obj)->dt_name);

    return Du_None;
}

DuObject *du_append(DuObject *cons, DuObject *locals)
{
    DuObject *lst, *newobj;
    _du_getargs2("append", cons, locals, &lst, &newobj);

    _du_save1(lst);
    DuList_Append(lst, newobj);
    _du_restore1(lst);
    return lst;
}

DuObject *du_pop(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None)
        Du_FatalError("pop: expected at least one argument");

    /* _du_read1(cons); IMMUTABLE */
    DuObject *expr = _DuCons_CAR(cons);
    DuObject *next = _DuCons_NEXT(cons);

    _du_save2(next, locals);
    DuObject *lst = Du_Eval(expr, locals);
    _du_restore2(next, locals);

    int index;
    if (next == Du_None) {
        index = DuList_Size(lst) - 1;
        if (index < 0)
            Du_FatalError("pop: empty list");
    }
    else {
        /* _du_read1(next); IMMUTABLE */
        DuObject *expr2 = _DuCons_CAR(next);
        DuObject *next2 = _DuCons_NEXT(next);

        if (next2 != Du_None)
            Du_FatalError("pop: expected at most two arguments");

        _du_save1(lst);
        DuObject *indexobj = Du_Eval(expr2, locals);
        _du_restore1(lst);

        index = DuInt_AsInt(indexobj);
    }

    return DuList_Pop(lst, index);
}

DuObject *du_len(DuObject *cons, DuObject *locals)
{
    DuObject *obj;
    _du_getargs1("len", cons, locals, &obj);

    int length = DuObject_Length(obj);
    return DuInt_FromInt(length);
}

DuObject *du_if(DuObject *cons, DuObject *locals)
{
    /* _du_read1(cons); IMMUTABLE */
    if (cons == Du_None || _DuCons_NEXT(cons) == Du_None)
        Du_FatalError("if: expected at least two arguments");

    DuObject *expr = _DuCons_CAR(cons);
    DuObject *next = _DuCons_NEXT(cons);

    _du_save2(next, locals);
    DuObject *cond = Du_Eval(expr, locals);
    _du_restore2(next, locals);

    /* _du_read1(next); IMMUTABLE */
    if (DuObject_IsTrue(cond) != 0) {
        /* true path */
        return Du_Eval(_DuCons_CAR(next), locals);
    }
    else {
        /* false path */
        return Du_Progn(_DuCons_NEXT(next), locals);
    }
}

DuObject *du_while(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None)
        Du_FatalError("while: expected at least one argument");

    /* _du_read1(cons); IMMUTABLE */
    DuObject *expr = _DuCons_CAR(cons);
    DuObject *next = _DuCons_NEXT(cons);

    while (1) {
        _du_save3(expr, next, locals);
        DuObject *cond = Du_Eval(expr, locals);
        _du_restore3(expr, next, locals);

        if (!DuObject_IsTrue(cond))
            break;

        _du_save3(expr, next, locals);
        Du_Progn(next, locals);
        _du_restore3(expr, next, locals);
    }
    return Du_None;
}

DuObject *du_defun(DuObject *cons, DuObject *locals)
{
    /* _du_read1(cons); IMMUTABLE */
    if (cons == Du_None || _DuCons_NEXT(cons) == Du_None)
        Du_FatalError("defun: expected at least two arguments");

    DuObject *name = _DuCons_CAR(cons);
    DuObject *next = _DuCons_NEXT(cons);

    /* _du_read1(next); IMMUTABLE */
    DuObject *arglist = _DuCons_CAR(next);
    DuObject *progn = _DuCons_NEXT(next);

    DuFrame_SetUserFunction(locals, name, arglist, progn);

    return Du_None;
}

DuObject *du_car(DuObject *cons, DuObject *locals)
{
    DuObject *obj;
    _du_getargs1("car", cons, locals, &obj);

    return DuCons_Car(obj);
}

DuObject *du_cdr(DuObject *cons, DuObject *locals)
{
    DuObject *obj;
    _du_getargs1("cdr", cons, locals, &obj);

    return DuCons_Cdr(obj);
}

DuObject *du_cons(DuObject *cons, DuObject *locals)
{
    DuObject *obj1, *obj2;
    _du_getargs2("cons", cons, locals, &obj1, &obj2);

    return DuCons_New(obj1, obj2);
}

DuObject *du_not(DuObject *cons, DuObject *locals)
{
    DuObject *obj;
    _du_getargs1("not", cons, locals, &obj);

    int res = !DuObject_IsTrue(obj);
    return DuInt_FromInt(res);
}

DuObject *du_transaction(DuObject *cons, DuObject *locals)
{
    if (cons == Du_None)
        Du_FatalError("transaction: expected at least one argument");

    /* _du_read1(cons); IMMUTABLE */
    DuObject *sym = _DuCons_CAR(cons);
    DuObject *rest = _DuCons_NEXT(cons);
    _DuFrame_EvalCall(locals, sym, rest, 0);
    return Du_None;
}

DuObject *du_run_transactions(DuObject *cons, DuObject *locals)
{
    if (cons != Du_None)
        Du_FatalError("run-transactions: expected no argument");

    _du_save1(stm_thread_local_obj);
    stm_stop_transaction();
    _du_restore1(stm_thread_local_obj);
    
    Du_TransactionRun();
    
    stm_start_inevitable_transaction();
    return Du_None;
}

DuObject *du_sleepms(DuObject *cons, DuObject *locals)
{
    DuObject *obj;
    _du_getargs1("sleepms", cons, locals, &obj);

    int ms = DuInt_AsInt(obj);

    struct timeval t;
    t.tv_sec = ms / 1000;
    t.tv_usec = (ms % 1000) * 1000;
    select(0, (fd_set *)0, (fd_set *)0, (fd_set *)0, &t);

    return Du_None;
}

DuObject *du_defined(DuObject *cons, DuObject *locals)
{
    /* _du_read1(cons); IMMUTABLE */
    if (cons == Du_None || _DuCons_NEXT(cons) != Du_None)
        Du_FatalError("defined?: expected one argument");
    DuObject *ob = _DuCons_CAR(cons);

    _du_save1(ob);
    DuObject *res = DuFrame_GetSymbol(locals, ob);
    _du_restore1(ob);

    if (res == NULL)
        res = DuFrame_GetSymbol(Du_Globals, ob);

    return DuInt_FromInt(res != NULL);
}

DuObject *du_pair(DuObject *cons, DuObject *locals)
{
    DuObject *obj;
    _du_getargs1("pair?", cons, locals, &obj);
    return DuInt_FromInt(DuCons_Check(obj));
}

DuObject *du_assert(DuObject *cons, DuObject *locals)
{
    DuObject *obj;
    _du_getargs1("assert", cons, locals, &obj);

    if (!DuInt_AsInt(obj)) {
        printf("assert failed: ");
        /* _du_read1(cons); IMMUTABLE */
        Du_Print(_DuCons_CAR(cons), 1);
        Du_FatalError("assert failed");
    }
    return Du_None;
}

extern void init_prebuilt_frame_objects(void);
extern void init_prebuilt_list_objects(void);
extern void init_prebuilt_object_objects(void);
extern void init_prebuilt_symbol_objects(void);
extern void init_prebuilt_transaction_objects(void);

void Du_Initialize(int num_threads)
{
    assert(num_threads == 2);

    stm_setup();
    stm_setup_thread();
    stm_setup_thread();
    _stm_restore_local_state(0);

    init_prebuilt_object_objects();
    init_prebuilt_symbol_objects();
    init_prebuilt_list_objects();
    init_prebuilt_frame_objects();
    init_prebuilt_transaction_objects();

    all_threads_count = num_threads;
    all_threads = (pthread_t*)malloc(sizeof(pthread_t) * num_threads);

    stm_start_transaction(NULL);
    DuFrame_SetBuiltinMacro(Du_Globals, "progn", Du_Progn);
    DuFrame_SetBuiltinMacro(Du_Globals, "setq", du_setq);
    DuFrame_SetBuiltinMacro(Du_Globals, "print", du_print);
    DuFrame_SetBuiltinMacro(Du_Globals, "+", du_add);
    DuFrame_SetBuiltinMacro(Du_Globals, "^", du_xor);
    DuFrame_SetBuiltinMacro(Du_Globals, "<<", du_lshift);
    DuFrame_SetBuiltinMacro(Du_Globals, ">>", du_rshift);
    DuFrame_SetBuiltinMacro(Du_Globals, "%", du_mod);
    DuFrame_SetBuiltinMacro(Du_Globals, "-", du_sub);
    DuFrame_SetBuiltinMacro(Du_Globals, "*", du_mul);
    DuFrame_SetBuiltinMacro(Du_Globals, "/", du_div);
    DuFrame_SetBuiltinMacro(Du_Globals, "||", du_or);
    DuFrame_SetBuiltinMacro(Du_Globals, "&&", du_and);
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
    DuFrame_SetBuiltinMacro(Du_Globals, "cons", du_cons);
    DuFrame_SetBuiltinMacro(Du_Globals, "not", du_not);
    DuFrame_SetBuiltinMacro(Du_Globals, "transaction", du_transaction);
    DuFrame_SetBuiltinMacro(Du_Globals, "run-transactions", du_run_transactions);
    DuFrame_SetBuiltinMacro(Du_Globals, "sleepms", du_sleepms);
    DuFrame_SetBuiltinMacro(Du_Globals, "defined?", du_defined);
    DuFrame_SetBuiltinMacro(Du_Globals, "pair?", du_pair);
    DuFrame_SetBuiltinMacro(Du_Globals, "assert", du_assert);
    DuFrame_SetSymbolStr(Du_Globals, "None", Du_None);
    stm_stop_transaction();
}

void Du_Finalize(void)
{
    _stm_restore_local_state(1);
    _stm_teardown_thread();

    _stm_restore_local_state(0);
    _stm_teardown_thread();

    _stm_teardown();
}
