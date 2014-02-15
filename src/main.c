/* -*- mode: c; c-file-style: "linux"; compile-command: "gcc -Wall -std=gnu99 main.c `pkg-config --libs --cflags openssl` `pkg-config --cflags --libs glib-2.0`" -*-
 *  vi: set shiftwidth=8 tabstop=8 noexpandtab:
 */

#include <poll.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h> 
#include <err.h>
#include <stdbool.h>
#include <openssl/sha.h>
#include <glib.h>

guint8 join_message[] = {
	0xF9, 0xBE, 0xB4, 0xD9, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6F,
	0x6E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00,
	0x35, 0x8D, 0x49, 0x32, 0x62, 0xEA, 0x00, 0x00, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0xB2, 0xD0, 0x50,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3B, 0x2E, 0xB3, 0x5D,
	0x8C, 0xE6, 0x17, 0x65, 0x0F, 0x2F, 0x53, 0x61, 0x74, 0x6F,
	0x73, 0x68, 0x69, 0x3A, 0x30, 0x2E, 0x37, 0x2E, 0x32, 0x2F,
	0xC0, 0x3E, 0x03, 0x00
};

const int join_message_len = sizeof(join_message);

struct __attribute__ ((__packed__)) msg {
	guint32 magic;
	char command[12];
	guint32 length_le; // Little-endian!
	guint32 checksum;
	guint8 payload[];
};

// Prototypes
guint64 var_int(const guint8 *const buf);
gint var_int_len(const guint8 *const buf);
guint32 checksum(const struct msg *const m);
unsigned char *dhash(const guchar *const d, const gulong n);
char *hex256(const guchar *const buf);
void log_msg(const struct msg *const m);
void process(const int fd);
void serial(const int fd);

int main(int argc, char *argv[])
{
	if(argc != 3) {
		errx(1,"Usage: %s <ip of server> <serial_port>",argv[0]);
	} 

	// Prepare serial port
	
	// TODO baud rate

	int dev_fd = open(argv[2],O_WRONLY|O_NOCTTY);
	if (dev_fd == -1) {
		err(2,"Unable to open serial port %s",argv[2]);
	}

	// Prepare socket

	int node_fd;
	struct sockaddr_in serv_addr;

	if((node_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		err(2,"Could not create socket");
	}

	memset(&serv_addr, '0', sizeof(serv_addr)); 

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(8333); 

	if(inet_pton(AF_INET, argv[1], &serv_addr.sin_addr) <= 0) {
		errx(1,"IP address conversion failed");
	} 

	if(connect(node_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
		err(2,"Connect failed");
	}

	// Send greeting
	if (write(node_fd, join_message, join_message_len) != join_message_len) {
		err(2,"Sending of welcome message has failed");
	}

	printf("Connected.\n");

	struct pollfd fds[] = {{dev_fd,POLLOUT,0},{node_fd,POLLIN,0}};

	// Process messages forever
	while (true) {
		const int ret = poll(fds,2,-1);
		if (ret < 1) err(5,"Error while polling");

		// Always serve slow serial first
		if (fds[0].revents & POLLOUT) serial(dev_fd);
		if (fds[1].revents & POLLIN) process(node_fd);
	}
}

// Processes messages having this format:
// https://en.bitcoin.it/wiki/Protocol_specification#Message_structure
void process(const int fd)
{
	static struct msg *buf = NULL; // For storing the message payload
	static int buf_allocated = 0;
	static int buf_pos = 0;
	static int buf_left = sizeof(struct msg);

	// Reallocate buffer only if it is too small.
	if (buf_allocated < buf_pos+buf_left) {
		buf = realloc(buf,buf_pos+buf_left);
		if (buf == NULL) errx(5,"Memory allocation failed");
	}

	const int got = read(fd,(void*)buf+buf_pos,buf_left);
	if (got == 0) {
		errx(3,"Unexpected end of bitcoind stream");
	} else if (got == -1) {
		err(3,"Error reading bitcoind stream");
	}
	buf_pos += got;
	buf_left -= got;

	// If not everything is received yet, come back later
	if (buf_left > 0) return;

	// Swapping byte ordering of length
	const guint32 payload_len = GUINT32_FROM_LE(buf->length_le);

	if (buf_pos == sizeof(struct msg) && payload_len != 0) {
		// Header received. To continue reading in next pass,
		// update the message length.
		buf_left = payload_len;
	} else {
		// All payload received. Process content.
		if (checksum(buf) != buf->checksum) {
			errx(3,"Checksum error. Probably we got out of sync.");
		}

		// Store to flat files
		log_msg(buf);

		// Message valid, parsing it
		if (strcmp(buf->command,"inv") == 0) {
			// Requesting transactions and blocks from a peer.

			// Payload in getdata request is identical to
			// inv and checksum is not affected because it
			// is calculated from payload only. We just
			// need to alter command name and send the
			// whole blob back to sender.

			// Because the command name is shorter in inv
			// than in getdata, we may just write over "inv"
			// and trailing null bytes will be fine.
			strcpy(buf->command,"getdata");

			// INFO: buf_pos has the total length (header+payload)
			// TODO: write() may block and become a bottleneck.
			if (write(fd,buf,buf_pos) != buf_pos) {
				err(2,"Sending of getdata has failed");
			}
		}

		// Start reading from top
		buf_pos = 0;
		buf_left = sizeof(struct msg);
	}
}

void serial(const int devfd) {
	// Just dummy writer for generating much traffic to serial
	static guint8 i=0;
	char instanssi[43];
	snprintf(instanssi,sizeof(instanssi),
		 "Kissa kissa kissa kissa... Instanssi! %3hhu\n",
		 i++);

	// Write the output. Do not care if some output is ignored.
	const int ret = write(devfd,instanssi,sizeof(instanssi)-1);
	if (ret < 1) err(4,"Unable to write to serial port");
	printf("sent %d bytes to serial port\n",ret);
}

guint64 var_int(const guint8 *const buf)
{
	if (*buf == 0xfd) return GUINT16_FROM_LE(*(guint16*)(buf+1));
	if (*buf == 0xfe) return GUINT32_FROM_LE(*(guint32*)(buf+1));
	if (*buf == 0xff) return GUINT64_FROM_LE(*(guint64*)(buf+1));
	return *buf;
}

int var_int_len(const guint8 *const buf)
{
	if (*buf == 0xfd) return 3;
	if (*buf == 0xfe) return 5;
	if (*buf == 0xff) return 9;
	return 1;
}

guint32 checksum(const struct msg *const m)
{
	return *(guint32*)dhash(m->payload,GUINT32_FROM_LE(m->length_le));
}

guchar *dhash(const guchar *const d, const gulong n)
{
	return SHA256(SHA256(d,n,NULL),32,NULL);
}

gchar *hex256(const guchar *const in)
{
	static char out[65];

	for (int i=0; i<32; i++) {
		snprintf(out+2*i,3,"%02hhx",in[31-i]);
	}

	return out;
}

void log_msg(const struct msg *const m)
{
	const int payload_len = GUINT32_FROM_LE(m->length_le);
	int hash_end = payload_len;

	if (strcmp(m->command,"inv") == 0) {
		// Inventory messages may be dropped from logs
		return;
	} else if (strcmp(m->command,"block") == 0) {
		// Block hash is calculated only from 6 first fields
		hash_end = 4+32+32+4+4+4;
	}
	
	// Prepare path name. FIXME this has security issue if command
	// string contains slashes. Not a problem if you connect to a
	// trusted peer, though.
	char pathname[4+11+1];
	snprintf(pathname,sizeof(pathname),"log/%s",m->command);

	// Create directory. If already exists, ignore error.
	const int ret = mkdir(pathname,0777);
	if (ret != 0 && errno != EEXIST) err(5,"Unable to create directory %s",pathname);

	// Prepare file name
	char filename[4+11+1+64+1];
	snprintf(filename,sizeof(filename),
		 "log/%s/%s",m->command,hex256(dhash(m->payload,hash_end)));

	// Open file
	FILE *const f = fopen(filename,"wb");
	if (f == NULL) err(5,"Unable to open file for writing %s",filename);
	
	// Store data to file
	if (payload_len > 0 && fwrite(m->payload,payload_len,1,f) != 1) {
		errx(5,"Storing message to log file has failed");
	}

	// Closing log file
	if (fclose(f) != 0) err(5,"Closing log file has failed");

	// Report what's done
	printf("Storing %s\n",filename);

	// Make sure debugging strings are printed
	fflush(stdout);
}
