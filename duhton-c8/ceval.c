#include "duhton.h"


DuObject *Du_Eval(DuObject *ob, DuObject *locals)
{
    eval_fn fn = Du_TYPE(ob)->dt_eval;
    if (fn) {
        return fn(ob, locals);
    }
    else {
        return ob;
    }
}
