#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>


#define MAXLINE 128
#define CORB_BUFF_SIZE 12
#define REGION_ZERO 0
#define BAR_REGION_OFFSET 40
#define BAR_ADDRESS_OFFSET 44

void get_corb_buffer_addr(char *array, char *corb_buffer_addr, unsigned int tlo)
{
	unsigned int i;
	printf("CORB buffer Address:");
	for(i = 0; array[i + (tlo + 48)] != ','; i++) {
		corb_buffer_addr[i] = array[i + (tlo + 48)];
		printf("%c", corb_buffer_addr[i]);
	}
	putchar('\n');

	return;
}

unsigned int regionCheck(char *array, unsigned int tlo)
{
	return array[tlo + 40];

}

/*
 * Get the character offset from the PID and the time of the trace.
 */
int traceLineOffset(char *array)
{
	int i, j;
	char match_chars[] = { '@', '.', ':', 0 };
	i = j = 0;
	do {
	  for(;array[i] && array[i] != match_chars[j]; i++);
	  if (array[i] != match_chars[j])
	    return -1;
	  ++j;
	} while(match_chars[j] != 0);
	return i;
}

void dumpMem(char *array, unsigned short framenumber, int fd, int is_final)
{
	int i;
	char cmdbuf[80];

        if (array[0] == '\0')
	{
	  printf("dumpMem entered... but the address is not set, skipping\n");
	  return;
        } else
	  printf("dumpMem entered...\n");

	if (is_final)
		sprintf(cmdbuf, "pmemsave %.10s 0x1000 exit_dump\n", array);
	else
		sprintf(cmdbuf, "pmemsave %.10s 0x1000 frame%02d\n", array, framenumber);

	for (i = 0; cmdbuf[i]; i++) {
		ioctl(fd, TIOCSTI, &cmdbuf[i]);
	}
}

int main(int argc, char *argv[])
{
	char corb_buffer_location[16];
	size_t trace_line_size = MAXLINE;
	char *trace_line = NULL;
	unsigned short framenumber = 0;
	int fd;
	unsigned int i = 0;
	unsigned int total_verbs = 0;
//	char cont[] = "cont\n";

	fd = open("/dev/tty", O_RDWR);

        if (fd < 0) {
		perror("opening terminal");
		return 2;
	}

/*	for(i = 0; cont[i]; i++) {
		ioctl(fd, TIOCSTI, cont+i);
	}
*/
        memset(corb_buffer_location, 0, sizeof(corb_buffer_location));

	while(1) {
		int tlo = 0;  // trace line offset, due to PID
		unsigned short switch_check = 0;

		if (trace_line)
			memset(trace_line, 0, trace_line_size);
		fflush(stdout);
		if (getline(&trace_line, &trace_line_size, stdin) == -1)
		  break;
		tlo = traceLineOffset(trace_line);
		if (tlo < 0)
		  	// ignore non-trace lines
			continue;

		switch_check = regionCheck(trace_line, tlo);

		/* Check which PCI BAR region it is */
		switch(switch_check) {
		/* this is the HDA register region */
		case '0':
			switch(trace_line[tlo + 44]) {
			case '2':
				if(trace_line[tlo + 45] == '0') {
					if(trace_line[tlo + 50] == '4' && total_verbs > 20)
						dumpMem(corb_buffer_location, framenumber, fd, 1);
				}
				break;
			case '4':
				switch(trace_line[tlo + 45]) {
				case '0':
        				memset(corb_buffer_location, 0, sizeof(corb_buffer_location));
					get_corb_buffer_addr(trace_line, corb_buffer_location, tlo);
					break;
				case '8':
					total_verbs += 4;
					printf("0x%04x \n", total_verbs);
					if(trace_line[tlo + 50] == 'f' && trace_line[tlo + 51] == 'f') {
						dumpMem(corb_buffer_location, framenumber, fd, 0);
						framenumber++;
					}
					break;
				}
				break;
			case '8':
				if(trace_line[tlo + 45] != ',') {
					printf("Current verb 0x%04x Region0+", total_verbs);
					for(i = 0; trace_line[i + (tlo + 42)] != ')'; i++) {
						printf("%c", trace_line[i + (tlo + 42)]);
					}
				putchar('\n');
				}
				break;
			case '1':
				printf("Current verb 0x%04x Region0+", total_verbs);
				for(i = 0; trace_line[i + (tlo + 42)] != ')'; i++) {
					printf("%c", trace_line[i + (tlo + 42)]);
				}
				putchar('\n');
				break;
			}

			break;
		default:
			printf("Current verb 0x%04x tlo = %i Region%c+", total_verbs, tlo, switch_check);
			i = 0;
			while(trace_line[i + (tlo + 42)] != ')') {
				printf("%c", trace_line[i + (tlo + 42)]);
				i++;
			}
			putchar('\n');

			break;
		}
		if (trace_line)
			memset(trace_line, 0, trace_line_size);
		fflush(stdout);
	}
	if (trace_line)
	  free(trace_line);
	return 0;
}



