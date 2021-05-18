#include "xmm7360.h"
#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/ioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

int max_frame, max_packets_per_frame;

struct bounds {
	uint32_t offset;
	uint32_t length;
};

struct first_header {
	uint32_t tag;
	uint16_t unknown;
	uint16_t sequence;
	uint16_t length;
	uint16_t extra;
	uint32_t next;
};

struct next_header {
	uint32_t tag;
	uint16_t length;
	uint16_t extra;
	uint32_t next;
};

struct {
	int n_packets, n_bytes;
	uint16_t *last_tag_length;
	uint32_t *last_tag_next;
	struct timespec deadline;
	struct bounds *bounds;
	uint8_t *data;
} frame;

uint16_t sequence = 0;

void frame_alloc(void)
{
	frame.data = malloc(max_frame);
	frame.bounds = malloc(sizeof(struct bounds) * max_packets_per_frame);
}

void frame_init(void)
{
	frame.n_packets = 0;
	frame.n_bytes = 0;
	frame.last_tag_next = NULL;
	frame.last_tag_length = NULL;
}

int frame_add_tag(uint32_t tag, void *data, int data_len)
{
	int total_length;
	if (frame.n_bytes == 0)
		total_length = sizeof(struct first_header) + data_len;
	else
		total_length = sizeof(struct next_header) + data_len;

	while (frame.n_bytes & 3)
		frame.n_bytes++;

	if (frame.n_bytes + total_length > max_frame)
		return -1;

	if (frame.last_tag_next)
		*frame.last_tag_next = frame.n_bytes;

	if (frame.n_bytes == 0) {
		struct first_header *hdr = (struct first_header *)frame.data;
		memset(hdr, 0, sizeof(struct first_header));
		hdr->tag = htonl(tag);
		hdr->sequence = sequence++;
		hdr->length = total_length;
		frame.last_tag_length = &hdr->length;
		frame.last_tag_next = &hdr->next;
		frame.n_bytes += sizeof(struct first_header);
	} else {
		struct next_header *hdr =
			(struct next_header *)(&frame.data[frame.n_bytes]);
		memset(hdr, 0, sizeof(struct next_header));
		hdr->tag = htonl(tag);
		hdr->length = total_length;
		frame.last_tag_length = &hdr->length;
		frame.last_tag_next = &hdr->next;
		frame.n_bytes += sizeof(struct next_header);
	}

	if (data_len) {
		memcpy(&frame.data[frame.n_bytes], data, data_len);
		frame.n_bytes += data_len;
	}

	return 0;
}

static int frame_append_data(void *data, int data_len)
{
	if (frame.n_bytes + data_len > max_frame)
		return -1;

	assert(frame.last_tag_length != NULL);

	memcpy(&frame.data[frame.n_bytes], data, data_len);
	*frame.last_tag_length += data_len;
	frame.n_bytes += data_len;

	return 0;
}

int frame_append_packet(void *data, int data_len)
{
	if (frame.n_packets >= max_packets_per_frame)
		return -1;

	int expected_adth_size = sizeof(struct next_header) + 4 +
				 (frame.n_packets + 1) * sizeof(struct bounds);

	if (frame.n_bytes + data_len + 16 + expected_adth_size > max_frame)
		return -1;

	assert(frame.last_tag_length != NULL);

	if (frame.n_packets == 0) {
		int ret = clock_gettime(CLOCK_MONOTONIC, &frame.deadline);
		if (ret) {
			perror("clock_gettime");
			exit(1);
		}

		// 100 µs coalesce time
		frame.deadline.tv_nsec += 100000;
		if (frame.deadline.tv_nsec > 1000000000LL) {
			frame.deadline.tv_nsec -= 1000000000LL;
			frame.deadline.tv_sec += 1;
		}
	}

	frame.bounds[frame.n_packets].offset = frame.n_bytes;
	frame.bounds[frame.n_packets].length = data_len + 16;
	frame.n_packets++;

	int ret;

	uint8_t pad[16];
	memset(pad, 0, sizeof(pad));
	ret = frame_append_data(pad, 16);
	if (ret)
		return ret;

	ret = frame_append_data(data, data_len);
	return ret;
}

int frame_append_adth(void)
{
	uint32_t unknown = 0;
	int ret;
	ret = frame_add_tag('ADTH', &unknown, sizeof(uint32_t));
	if (ret)
		return ret;
	ret = frame_append_data(&frame.bounds[0],
				sizeof(struct bounds) * frame.n_packets);
	return ret;
}

void frame_complete(void)
{
	// first tag gets the total length
	struct first_header *hdr = (void *)&frame.data[0];
	hdr->length = frame.n_bytes;
}

void frame_push(int mux_fd)
{
	frame_complete();
	int ret = write(mux_fd, frame.data, frame.n_bytes);
	if (ret < frame.n_bytes) {
		perror("mux write");
	}

	frame_init();
}

static int tun_alloc(void)
{
	struct ifreq ifr;
	int fd, ret;

	if ((fd = open("/dev/net/tun", O_RDWR)) < 0) {
		perror("tun open");
		exit(1);
	}

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

	ret = ioctl(fd, TUNSETIFF, (void *)&ifr);
	if (ret < 0) {
		close(fd);
		return ret;
	}
	return fd;
}

uint8_t *inbuf;

static void handle_mux_frame(int mux, int tun)
{
	int count = read(mux, inbuf, max_frame);
	if (count < 0) {
		perror("mux read");
		exit(1);
	}

	struct first_header *first = (void *)inbuf;
	if (ntohl(first->tag) != 'ADBH') {
		printf("Unexpected tag %x\n", first->tag);
		return;
	}

	struct next_header *adth = (void *)(&inbuf[first->next]);
	if (ntohl(adth->tag) != 'ADTH') {
		printf("Unexpected ADTH tag %x\n", adth->tag);
		return;
	}

	int n_packets = (adth->length - sizeof(struct next_header) - 4) /
			sizeof(struct bounds);

	struct bounds *bounds =
		(void *)&inbuf[first->next + sizeof(struct next_header) + 4];

	for (int i = 0; i < n_packets; i++)
		write(tun, &inbuf[bounds[i].offset], bounds[i].length);
}

static void handle_tun_frame(int tun, int mux)
{
	int count = read(tun, inbuf, max_frame);
	if (count < 0) {
		perror("tun read");
		exit(1);
	}

	int ret;

	ret = frame_append_packet(inbuf, count);

	if (ret || frame.n_packets >= max_packets_per_frame) {
		frame_append_adth();
		frame_push(mux);
		frame_add_tag('ADBH', NULL, 0);
	}

	// maybe frame was too full; try again
	if (ret)
		ret = frame_append_packet(inbuf, count);

	if (ret) {
		printf("FATAL: could not write packet of %d bytes\n", count);
		exit(1);
	}
}

int main(int argc, char **argv)
{
	int mux = open("/dev/xmm0/mux", O_RDWR);
	if (mux < 0) {
		perror("mux open");
		exit(1);
	}
	uint32_t val;
	int ret = ioctl(mux, XMM7360_IOCTL_GET_PAGE_SIZE, &val);
	if (ret < 0) {
		perror("mux ioctl");
		exit(1);
	}
	max_frame = val;
	max_packets_per_frame = max_frame / 1024;

	frame_alloc();
	inbuf = malloc(max_frame);

	int tun = tun_alloc();

	uint32_t cmdh_args[] = { 1, 0, 0, 0 };
	frame_init();
	frame_add_tag('ACBH', NULL, 0);
	frame_add_tag('CMDH', cmdh_args, sizeof(cmdh_args));
	frame_push(mux);

	frame_add_tag('ADBH', NULL, 0);

	int fd_max = mux > tun ? mux : tun;
	fd_set fds;
	FD_ZERO(&fds);
	struct timeval tv;
	while (1) {
		FD_SET(mux, &fds);
		FD_SET(tun, &fds);

		struct timeval *ptv;
		if (frame.n_packets) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			if (now.tv_sec >= frame.deadline.tv_sec &&
			    now.tv_nsec >= frame.deadline.tv_nsec)
				goto frame_out;

			tv.tv_sec = frame.deadline.tv_sec - now.tv_sec;
			if (now.tv_nsec > frame.deadline.tv_nsec) {
				tv.tv_sec -= 1;
				tv.tv_usec = (frame.deadline.tv_nsec / 1000) +
					     1000000 - (now.tv_nsec / 1000);
			} else {
				tv.tv_usec = (frame.deadline.tv_nsec / 1000) -
					     (now.tv_nsec / 1000);
			}
			ptv = &tv;
		} else {
			ptv = NULL;
		}

		int ret = select(fd_max + 1, &fds, NULL, NULL, ptv);
		if (ret < 0) {
			perror("select");
			exit(1);
		}

		if (FD_ISSET(mux, &fds))
			handle_mux_frame(mux, tun);

		if (FD_ISSET(tun, &fds))
			handle_tun_frame(tun, mux);

		if (ptv && !tv.tv_usec && frame.n_packets) {
		frame_out:
			frame_append_adth();
			frame_push(mux);
			frame_add_tag('ADBH', NULL, 0);
		}
	}
}
