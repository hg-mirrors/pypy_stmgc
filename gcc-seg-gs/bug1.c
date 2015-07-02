typedef struct {
    int a[20];
} foo_t;


int sum1(__seg_gs foo_t *p)
{
    int i, total=0;
    for (i=0; i<20; i++)
        total += p->a[i];
    return total;
}

int sum2(void)
{
    __seg_gs foo_t *p = (__seg_gs foo_t *)0x1234;
    int i, total=0;
    for (i=0; i<20; i++)
        total += p->a[i];     // <= this memory read is missing %gs:
    return total;
}
