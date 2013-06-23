#include "duhton.h"


#ifndef Du_AME

static DuObject *list_transactions = NULL;

void Du_TransactionAdd(DuObject *frame)
{
    if (list_transactions == NULL) {
        list_transactions = DuList_New();
        _Du_ForgetReference(list_transactions);
    }
    DuList_Append(list_transactions, frame);
}

void Du_TransactionRun(void)
{
    if (list_transactions == NULL)
        return;
    while (DuList_Size(list_transactions) > 0) {
        DuObject *frame = DuList_Pop(list_transactions, 0);
        DuObject *res = DuFrame_Execute(frame);
        Du_DECREF(res);
        Du_DECREF(frame);
    }
}

#endif
