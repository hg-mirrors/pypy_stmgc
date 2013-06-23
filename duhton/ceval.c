#include "duhton.h"


DuObject *Du_Eval(DuObject *ob, DuObject *locals)
{
    eval_fn fn = ob->ob_type->dt_eval;
    if (fn) {
        return fn(ob, locals);
    }
    else {
        Du_INCREF(ob);
        return ob;
    }
}
