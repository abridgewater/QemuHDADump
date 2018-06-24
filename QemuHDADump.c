#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>


#define MAXLINE 128
#define CORB_BUFF_SIZE 12
#define BAR_REGION_OFFSET 40
#define BAR_ADDRESS_OFFSET 44

struct trace_event {
	int pci_region;
	uint64_t offset;
	uint32_t data;
	int width;
};

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

int parse_trace_event(struct trace_event *event, char *input)
{
	int name_offset;
	char terminus;

	name_offset = traceLineOffset(input);

	if (name_offset < 0) return name_offset;

	/* Apparently, sscanf() will silently ignore anything past the
	 * last match found, so we match for %c after all of our
	 * parameters and then check to make sure that it is the
	 * expected close-paren. */
	if ((sscanf(&input[name_offset+1], "vfio_region_write "
		    "(%*4x:%*2x:%*2x.%*1d:region%d+%li, %i, %i%c",
		    &event->pci_region, &event->offset,
		    &event->data, &event->width, &terminus) == 5)
	    && (terminus == ')'))
		return name_offset;

	return -1;
}

void stuff_tty_input(int tty_fd, char *input)
{
	for (int i = 0; input[i]; i++) {
		ioctl(tty_fd, TIOCSTI, &input[i]);
	}
}

void dumpMem(char *array, unsigned short framenumber, int fd, int is_final)
{
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

	stuff_tty_input(fd, cmdbuf);
}

int main(int argc, char *argv[])
{
	char corb_buffer_location[16];
	size_t trace_line_size = MAXLINE;
	char *trace_line = NULL;
	unsigned short framenumber = 0;
	int fd;
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
		struct trace_event event;

		if (trace_line)
			memset(trace_line, 0, trace_line_size);
		fflush(stdout);
		if (getline(&trace_line, &trace_line_size, stdin) == -1)
		  break;
		tlo = parse_trace_event(&event, trace_line);
		if (tlo < 0)
		  	// ignore non-trace lines
			continue;

		/* Check which PCI BAR region it is */
		switch(event.pci_region) {
		/* this is the HDA register region */
		case 0:
			if ((event.offset == 0x20)
			    && (((event.data & 0xf0000000) == 0x40000000)
				|| ((event.data & 0xff000000) == 0x4000000)
				|| ((event.data & 0xfff00000) == 0x400000)
				|| ((event.data & 0xffff0000) == 0x40000)
				|| ((event.data & 0xfffff000) == 0x4000)
				|| ((event.data & 0xffffff00) == 0x400)
				|| ((event.data & 0xfffffff0) == 0x40)
				|| ((event.data & 0xffffffff) == 0x4))
			    && (total_verbs > 20)) {
				dumpMem(corb_buffer_location, framenumber, fd, 1);
			} else if (trace_line[tlo + 44] == '4') {
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
			} else if (trace_line[tlo + 44] == '8') {
				if(trace_line[tlo + 45] != ',') {
					printf("Current verb 0x%04x Region%d+0x%lx, 0x%x, %d\n",
					       total_verbs, event.pci_region, event.offset,
					       event.data, event.width);
				}
			} else if (trace_line[tlo + 44] == '1') {
				printf("Current verb 0x%04x Region%d+0x%lx, 0x%x, %d\n",
				       total_verbs, event.pci_region, event.offset,
				       event.data, event.width);
				break;
			}

			break;
		default:
			printf("Current verb 0x%04x tlo = %i Region%d+0x%lx, 0x%x, %d\n",
			       total_verbs, tlo, event.pci_region, event.offset,
			       event.data, event.width);
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



