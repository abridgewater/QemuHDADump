#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/param.h>


struct trace_event {
	int pci_region;
	uint64_t offset;
	uint32_t data;
	int width;
};

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

char data_fifo_name[PATH_MAX];

void release_data_fifo(void)
{
	unlink(data_fifo_name);
}

void prepare_data_fifo(void)
{
	int i;

	for (i = 0; ; i++) {
		sprintf(data_fifo_name, "/tmp/QemuHDADumpFIFO-%d", i);
		if (!mkfifo(data_fifo_name, 0666)) { /* stupid octal mode flags */
			atexit(release_data_fifo);
			return;
		}
		if (errno != EEXIST) {
			perror("creating data fifo");
			exit(1);
		}
	}
}

void dumpMem(uint32_t reg_corblbase, unsigned short framenumber, int fd, int is_final)
{
	char cmdbuf[80];

	printf("dumpMem entered...\n");

	if (is_final)
		sprintf(cmdbuf, "pmemsave 0x%"PRIx32" 0x1000 exit_dump\n",
			reg_corblbase);
	else
		sprintf(cmdbuf, "pmemsave 0x%"PRIx32" 0x1000 frame%02d\n",
			reg_corblbase, framenumber);

	stuff_tty_input(fd, cmdbuf);
}

struct corb_dma_state {
	uint32_t reg_corblbase;
	uint8_t reg_corbwp;
	uint16_t reg_corbrp;
	int corbsize;
	int corbrun;
};

void corb_dma_update(struct corb_dma_state *corb_dma)
{
	if (!corb_dma->corbrun)
		/* DMA engine is stopped, nothing to do */
		return;

	if ((corb_dma->reg_corbrp & (corb_dma->corbsize - 1))
	    == (corb_dma->reg_corbwp & (corb_dma->corbsize - 1)))
		/* CORB is empty, nothing to do */
		return;

	corb_dma->reg_corbrp =
		(corb_dma->reg_corbrp & 0xff00)
		| (corb_dma->reg_corbwp & (corb_dma->corbsize - 1));
}

int main(int argc, char *argv[])
{
	struct corb_dma_state corb_dma = {
		.reg_corblbase = 0,
		.reg_corbwp = 0,
		.reg_corbrp = 0,
		.corbsize = 1, /* unknown CORB size */
		.corbrun = 0 /* DMA disabled */
	};
	size_t trace_line_size = 0;
	char *trace_line = NULL;
	unsigned short framenumber = 0;
	int fd;
	int data_fifo_fd;
	unsigned int total_verbs = 0;

	fd = open("/dev/tty", O_RDWR);

        if (fd < 0) {
		perror("opening terminal");
		return 2;
	}

	prepare_data_fifo();

	data_fifo_fd = open(data_fifo_name, O_RDWR);
	if (data_fifo_fd < 0) {
		perror("opening data fifo");
		return 1;
	}

	while (1) {
		struct trace_event event;

		if (getline(&trace_line, &trace_line_size, stdin) == -1)
			break;
		if (parse_trace_event(&event, trace_line) < 0) {
		  	/* ignore non-trace lines */
			continue;
		}

		/* Check which PCI BAR region it is */
		switch(event.pci_region) {
		case 0: /* HDA registers */
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
				dumpMem(corb_dma.reg_corblbase, framenumber, fd, 1);
			} else if ((event.offset & ~3) == 0x40) {
				/* CORBLBASE */
				corb_dma.reg_corblbase = event.data;
				/* FIXME: Correctly handle sub-dword writes */
				printf("CORB buffer Address: 0x%"PRIx32"\n",
				       corb_dma.reg_corblbase);
			} else if (event.offset == 0x48) {
				/* CORBWP */
				corb_dma.reg_corbwp = event.data & 0xff;
				corb_dma_update(&corb_dma);
				total_verbs += 4;
				printf("0x%04x \n", total_verbs);
				if (corb_dma.reg_corbwp == 0xff) {
					dumpMem(corb_dma.reg_corblbase, framenumber, fd, 0);
					framenumber++;
				}
			} else if ((event.offset == 0x4a)
				   && (event.width >= 2)) {
				/* CORBRP */
				if (!(event.data & 0x8000)
				    && (corb_dma.reg_corbrp & 0x8000)) {
					/* On the falling edge of
					 * CORBRPRST, the controller
					 * clears CORBRP */
					corb_dma.reg_corbrp = 0;
				} else {
					/* CORBRPRST is the only
					 * writable part of CORBRP */
					corb_dma.reg_corbrp =
						(corb_dma.reg_corbrp & 0xff)
						| (event.data & 0x8000);
				}
			} else if (event.offset == 0x4c) {
				/* CORBCTL */
				/* FIXME: DWORD writes here also hit CORBSIZE */
				corb_dma.corbrun = !!(event.data & 2);
				corb_dma_update(&corb_dma);
			} else if (event.offset == 0x4e) {
				/* CORBSIZE */
				const char *size_texts[4] = {"2 entries",
							     "16 entries",
							     "256 entries",
							     "reserved"};
				const int size_values[4] = {2, 16, 256, 1};
				/* We treat "reserved" as 1, and then
				 * 1 as meaning "unknown CORB size",
				 * disabling CORB tracking */
				printf("CORB size set: %s\n",
				       size_texts[event.data & 3]);
				corb_dma.corbsize = size_values[event.data & 3];
			} else if (((event.offset & 0xfffffff0) == 0x80)
				   || ((event.offset & 0xffffff00) == 0x800)
				   || ((event.offset & 0xfffff000) == 0x8000)
				   || ((event.offset & 0xffff0000) == 0x80000)
				   || ((event.offset & 0xfff00000) == 0x800000)
				   || ((event.offset & 0xff000000) == 0x8000000)
				   || ((event.offset & 0xf0000000) == 0x80000000)
				   || (event.offset == 0x1)
				   || ((event.offset & 0xfffffff0) == 0x10)
				   || ((event.offset & 0xffffff00) == 0x100)
				   || ((event.offset & 0xfffff000) == 0x1000)
				   || ((event.offset & 0xffff0000) == 0x10000)
				   || ((event.offset & 0xfff00000) == 0x100000)
				   || ((event.offset & 0xff000000) == 0x1000000)
				   || ((event.offset & 0xf0000000) == 0x10000000)) {
			    printf("Current verb 0x%04x Region%d+0x%lx, 0x%x, %d\n",
				   total_verbs, event.pci_region, event.offset,
				   event.data, event.width);
			}

			break;
		default:
			printf("Current verb 0x%04x Region%d+0x%lx, 0x%x, %d\n",
			       total_verbs, event.pci_region, event.offset,
			       event.data, event.width);
			break;
		}
		fflush(stdout);
	}
	if (trace_line)
		free(trace_line);
	return 0;
}
