#include "duhton.h"


int main(int argc, char **argv)
{
    char *filename;
    int interactive;
    if (argc <= 1) {
        filename = "-";   /* stdin */
        interactive = 1;
    }
    else {
        filename = argv[1];
        interactive = 0;
    }

    Du_Initialize();

    while (1) {
        if (interactive) {
            printf("))) ");
            fflush(stdout);
        }
        DuObject *code = Du_Compile(filename, interactive);
        if (code == NULL) {
            printf("\n");
            break;
        }
        /*Du_Print(code, 1);
          printf("\n");*/
        DuObject *res = Du_Eval(code, Du_Globals);
        if (interactive) {
            Du_Print(res, 1);
        }
        Du_DECREF(res);
        Du_DECREF(code);
        Du_TransactionRun();
        if (!interactive)
            break;
    }

    Du_Finalize();
    return 0;
}
