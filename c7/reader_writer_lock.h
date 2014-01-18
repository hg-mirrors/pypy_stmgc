
typedef union rwticket rwticket;
union rwticket
{
	unsigned u;
	unsigned short us;
	struct
	{
		unsigned char write;
		unsigned char read;
		unsigned char users;
	} s;
};

void rwticket_wrlock(rwticket *l);
int rwticket_wrunlock(rwticket *l);
int rwticket_wrtrylock(rwticket *l);
void rwticket_rdlock(rwticket *l);
void rwticket_rdunlock(rwticket *l);
int rwticket_rdtrylock(rwticket *l);


