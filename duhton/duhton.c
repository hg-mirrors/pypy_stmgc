#include "duhton.h"

#define DEFAULT_NUM_THREADS 4

int main(int argc, char **argv)
{
    char *filename = NULL;
    int interactive = 1;
	int i;
	int num_threads = DEFAULT_NUM_THREADS;

	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--help") == 0) {
			printf("Duhton: a simple lisp-like language with STM support\n\n");
			printf("Usage: duhton [--help] [--num-threads no] [filename]\n");
			printf("  --help: this help\n");
			printf("  --num-threads <number>: number of threads (default 4)\n\n");
			exit(0);
		} else if (strcmp(argv[i], "--num-threads") == 0) {
			if (i == argc - 1) {
				printf("ERROR: --num-threads requires a parameter\n");
				exit(1);
			}
			num_threads = atoi(argv[i + 1]);
			i++;
		} else if (strncmp(argv[i], "--", 2) == 0) {
			printf("ERROR: unrecognized parameter %s\n", argv[i]);
		} else {
			filename = argv[i];
			interactive = 0;
		}
	}
    if (!filename) {
        filename = "-";   /* stdin */
	}

    Du_Initialize(num_threads);

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
        Du_TransactionRun();
        if (!interactive)
            break;
    }

    Du_Finalize();
    return 0;
}
