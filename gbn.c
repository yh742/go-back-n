#include "gbn.h"
#include "helper.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

state_t s;

/* serialize from header format to buffer format */
static void serialize_gbnhdr(char* buffer, gbnhdr* hdr, int len){
    char* ptr;
    /* idx keeps track of buffer index */
    int i = 0, idx = 0, num = 0;
    buffer[idx++] = hdr->type;
    buffer[idx++] = hdr->seqnum;

    /* not always guaranteed to be using same arch, convert to network byte order */
    num = htons(hdr->checksum);
    ptr = (char *)&num;
    buffer[idx++] = *ptr;
    buffer[idx++] = *(ptr+1);

    /* copy payload to the buffer */
    for (; idx < len; i++, idx++){
        buffer[idx] = hdr->data[i];
    }
}

/* modified checksum previous one was no good, too many collisions */
uint16_t checksum2(gbnhdr *hdr)
{
    int nwords = (sizeof(hdr->type) + sizeof(hdr->seqnum) + sizeof(hdr->data))/sizeof(uint16_t);
    uint16_t buf_array[nwords];
    buf_array[0] = (uint16_t)hdr->seqnum + ((uint16_t)hdr->type << 8);
    int bIndex = 1;
    for (; bIndex <= sizeof(hdr->data); bIndex++){
        int wIndex = (bIndex + 1) / 2;
        if (bIndex % 2 == 1){
            buf_array[wIndex] = hdr->data[bIndex-1];
        } else {
            buf_array[wIndex] = buf_array[wIndex] << 8;
            buf_array[wIndex] += hdr -> data[bIndex - 1];
        }
    }
    
    uint16_t *buf = buf_array;
    uint32_t sum;
    for (sum = 0; nwords > 0; nwords--)
        sum += *buf++;
    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    return ~sum;
}

/* deserialize from buffer format to header format */
static void deserialize_gbnhdr(char* buffer, gbnhdr* hdr, int data_len){
    char* ptr;
    int i = 0, idx = 0;
    hdr->type = buffer[idx++];
    hdr->seqnum = buffer[idx++];

    /* not always guaranteed to be using same arch, convert to host byte order */
    ptr = (char*)&(hdr->checksum);
    *ptr = buffer[idx++];
    *(ptr+1) = buffer[idx++];
    hdr->checksum = ntohs(hdr->checksum);
    for (i = 0; i < data_len; i++){
        hdr->data[i] = buffer[idx++];
    }
}

/* original checksum algo */
uint16_t checksum(uint16_t *buf, int nwords)
{
	uint32_t sum;
    int count = nwords;
	for (sum = 0; count > 0; count--)
		sum += *buf++;
    int left = (nwords % 16);
    sum += (*buf & (0xffff >> left));

	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);
	return ~sum;
}

/* test checksum to see if header checksum is correct */
static int test_checksum(gbnhdr* hdr){
    int ret_checksum = hdr->checksum;
    hdr->checksum = 0;
    int cal_checksum = checksum2(hdr);
    DBG_PRINT("Checksum: Original %d, Calculated %d", ret_checksum, cal_checksum);
    if (ret_checksum != cal_checksum){
        DBG_ERROR("Checksum mismatch! %d, %d", ret_checksum, cal_checksum);
        return -1;
    }
    return 0;
}

/* set checksum in the header packet */
static void set_checksum(gbnhdr* hdr){
    int ret_checksum;
    /* checksum field is set to 0 when calculating */
    hdr->checksum = 0;
    ret_checksum = checksum2(hdr);
    hdr->checksum = ret_checksum;
}

/* ignore alarm signal and re-register handler */
static void ARLMHNDR(int sig){
    signal(SIGALRM, SIG_IGN);          /* ignore this signal       */
    signal(SIGALRM, ARLMHNDR);
}

/* initialize header packets using this function  */
static void init_header(gbnhdr* hdr, int type, int seq, const char* buf, int len){
    memset(hdr, 0, sizeof(gbnhdr));
    hdr->type = type;
    hdr->seqnum = seq;
    hdr->checksum = 0;
    if (buf == NULL){
        memset(hdr->data, 0, DATALEN);
    }
    else{
        memcpy(hdr->data, buf, len);
    }
    set_checksum(hdr);
}

/* sends header over to the server using the original sendto() function */
static int sendto_hdr(int sockfd, gbnhdr* hdr, int hdr_len){
    int count = 0;
    char buffer[hdr_len];
    memset(buffer, 0, hdr_len);
    serialize_gbnhdr(buffer, hdr, hdr_len);
    if ((count = sendto(sockfd, buffer, hdr_len, 0, &s.addr, s.len)) != hdr_len){
        DBG_ERROR("Size of sent %d is different than expected %d.", count, hdr_len);
        return -1;
    }
    return count;
}

/* sends header over to the server using the fake sendto() function for packet losses*/
static int sendto_maybe_hdr(int sockfd, gbnhdr* hdr, int hdr_len){
    int count = 0;
    char buffer[hdr_len];
    memset(buffer, 0, hdr_len);
    serialize_gbnhdr(buffer, hdr, hdr_len);
    if ((count = maybe_sendto(sockfd, buffer, hdr_len, 0, &s.addr, s.len)) != hdr_len){
        DBG_ERROR("Size of sent %d is different than expected %d.", count, hdr_len);
        return -1;
    }
    return count;
}

/* receives header using the recfrom() function */
static int recvfrom_hdr(int sockfd, gbnhdr* hdr, int type, int seq,
                        struct sockaddr* addr, socklen_t* len, int timed){
    int count = 0;
    char buffer[sizeof(gbnhdr)];
    memset(hdr, 0, sizeof(gbnhdr));
    /* if the given addr is NULL don't receive a struct */
    if (addr == NULL){
        count = recvfrom(sockfd, buffer, sizeof(gbnhdr), 0, NULL, NULL);
    }
    else{
        count = recvfrom(sockfd, buffer, sizeof(gbnhdr), 0, addr, len);
    }

    if (count < 1){
        /* alarm clock was fired */
        if (count == -1){
            DBG_ERROR("Operation timed out.");
            return -1;
        }
        else{
            DBG_ERROR("Size of received packet is zero.");
            return 0;
        }
    }
    /* deserialize and check checksum first, type second and sequence third */
    /* different return codes will signify different failure symptoms for callee */
    deserialize_gbnhdr(buffer, hdr, count - 4);
    if (test_checksum(hdr) != 0){
        return -4;
    }
    if (hdr->type != type){
        DBG_ERROR("The returned value type %d is differet than expected %d.", hdr->type, type);
        return -2;
    }
    if (hdr->seqnum != seq){
        DBG_ERROR("Return the wrong seqnum %d than expected %d.", hdr->seqnum, seq);
        return -3;
    }
    return count;
}

/* use packet to represent one 1024 bytes of data */
struct packet {
    char* start_addr;
    int length;
};

ssize_t gbn_send(int sockfd, const void *buf, size_t len, int flags){

    if (s.state != ESTABLISHED){
        DBG_ERROR("ESTABLISHED state");
        return -1;
    }

    char* buffer = (char*)buf;
    /* split input buffer into a an array of packets */
    int i = 0;
    int send_len = 0;
    int read_len = len;
    uint32_t array_len = len % DATALEN == 0? len/DATALEN : len/DATALEN + 1;
    /*DBG_PRINT("Array Length %d", array_len);*/
    struct packet* packs = malloc(sizeof(struct packet) * array_len);
    for (; i < array_len; i++, buffer += DATALEN, read_len -= DATALEN){
        send_len = read_len < DATALEN ? read_len : DATALEN;
        packs[i].start_addr = buffer;
        packs[i].length = send_len;
    }

    /*start sending it using window size 1 */
    int res = 0;
    /* on successful sends reset attempts to 0, else on fails increment attempts */
    int attempts = 0;
    int packs_sent = 0;
    s.winsize = 1;
    gbnhdr hdr = {0};
    /* have a sliding window keeping track of index along with cursor */
    uint8_t window[2] = {0, 1};
    uint8_t seq_cur = s.ex_seqnum;
    /* set window slots */
    window[0] = s.ex_seqnum;

    /* setup timer */
    signal(SIGALRM, ARLMHNDR);
    struct itimerval timer;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = 250000;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 0;

    do {
        /* send the packets dpending on the window size */
        int i;
        int ack_exp = 0;
        for (i = 0; i < s.winsize; i++){
            if (seq_cur == window[i]) {
                init_header(&hdr, DATA, seq_cur, packs[seq_cur].start_addr, packs[seq_cur].length);
                if (sendto_maybe_hdr(sockfd, &hdr, packs[seq_cur].length + 4) < 1) {
                    DBG_ERROR("Error occured while sending");
                    seq_cur--;
                    continue;
                }
                seq_cur++;
                ack_exp++;
            }
        }

        /* for receiving packets, make sure that the packets are within bounds of window */
        for (i = 0; i < ack_exp; i++){
            setitimer(ITIMER_REAL, &timer, NULL);
            res = recvfrom_hdr(sockfd, &hdr, DATAACK, window[0], NULL, NULL, 0);
            /* split between windows size cases */
            if (s.winsize == 1){
                if (res > 0){ /* packets received are correct */
                    window[0]++;
                    packs_sent++;
                    s.winsize = 2;
                    attempts = 0;
                }
                else{   /* packets received are not correct, reset cursor */
                    seq_cur = window[0];
                    s.winsize = 1;
                    attempts++;
                }
            }else{ /* window size 2 */
                if (res > 0){   /* packets received are correct */
                    window[0]++;
                    packs_sent++;
                    s.winsize = 2;
                    attempts = 0;
                }
                else if(res == -3 && hdr.seqnum == window[1]){
                    /* packets received came in out of order but within bounds */
                    /* use cumulative ACK */
                    packs_sent += 2;
                    window[0] = window[1] + 1;
                    s.winsize = 2;
                    attempts = 0;
                    break;
                }
                else{
                    /* something failed reset cursor to first un-ACK'd packet */
                    seq_cur = window[0];
                    s.winsize = 1;
                    attempts++;
                }
            }
        }
        DBG_PRINT("DATAACK: packet %d, res %d", hdr.seqnum, res);
        /* make sure the window indices don't go past array size */
        window[0] = window[0] >= array_len - 1 ? array_len - 1 : window[0];
        window[1] = window[0] >= array_len - 1 ? array_len - 1: window[0] + s.winsize - 1;
    } while(packs_sent != array_len && attempts != 10);
    DBG_PRINT("Exiting out of gbn_send");
    return 0;
}

/* the receiver essentially acts like it has window size 1 */
/* if any packet received out of order, reject and request last ACKed packet */
ssize_t gbn_recv(int sockfd, void *buf, size_t len, int flags){
    int count  = 0;
    gbnhdr hdr = {0};
    int cflag = 0;
    do
    {
        switch(s.state){
            case ESTABLISHED:
                count = recvfrom_hdr(sockfd, &hdr, DATA, s.ex_seqnum, NULL, NULL, 0);
                DBG_PRINT("Got packet length %d, seq %d from socket", count, hdr.seqnum);
                /* the type is wrong */
                if (count == -2) {
                    if (hdr.type == SYN) {
                        /* client is still waiting for SYNACK */
                        init_header(&hdr, SYNACK, 0, NULL, 0);
                    } else if (hdr.type == FIN) {
                        /* client sent FIN, have gbn_close deal with it */
                        s.state = FIN_RCVD;
                        return 0;
                    } else {
                        /* something is horribly wrong */
                        return -1;
                    }
                }
                else if (count < 0){
                    if (count == -3 && hdr.seqnum < s.ex_seqnum){ /* lower packet sequence arrived ACK number back */
                        init_header(&hdr, DATAACK, hdr.seqnum, NULL, 0);
                    }
                    else { /* something else went wrong, ack with last sequence (packet larger than sequence) */
                        init_header(&hdr, DATAACK, s.ex_seqnum - 1, NULL, 0);
                    }
                }
                else { /* we got the right packet */
                    /* we got the right packet, write to file */
                    memcpy(buf, hdr.data, count - 4);
                    init_header(&hdr, DATAACK, s.ex_seqnum, NULL, 0);
                    DBG_PRINT("Writing packet %d to file", hdr.seqnum);
                    s.ex_seqnum++;
                    cflag = 1;
                }
                if (sendto_maybe_hdr(sockfd, &hdr, sizeof(gbnhdr)) < 1){
                    /* critical error occured, bail */
                    DBG_ERROR("Can't send to client.");
                    return -1;
                }
                DBG_PRINT("Sending packet %d", hdr.seqnum);
                break;
            default:
                return -1;
        }
    }while(!cflag);
    DBG_PRINT("gbn_recv EXITING");
    /* -4 for the type, seqnum, check bytes */
    return count - 4;
}

/* Send FIN, Recv FIN, Send FINACK, Recv FINACK */
/* implemented with two way hand shake */
int gbn_close(int sockfd){
    int count = 0;
    int attempt = 0;
    gbnhdr hdr = {0};
    /* setup timer scaffolding */
    signal(SIGALRM, ARLMHNDR);
    struct itimerval timer;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = 250000;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 0;
    while (s.state != CLOSED){
        if (attempt == 10) break;
        switch(s.state){
            case ESTABLISHED:   /* this must be client, send first FIN */
                init_header(&hdr, FIN, 0, NULL, 0);
                if ((count = sendto_maybe_hdr(sockfd, &hdr, sizeof(gbnhdr))) < 1){
                    DBG_ERROR("Error occured while sending");
                    attempt++;
                    continue;
                }
                s.state = FIN_SENT;
                break;
            case FIN_SENT:      /* client waits for FINACK to respond */
                setitimer(ITIMER_REAL, &timer, NULL);
                if ((count = recvfrom_hdr(sockfd, &hdr, FINACK, 0, NULL, NULL, 0)) < 1){
                    DBG_ERROR("Error occured while waiting for recvfrom");
                    s.state = ESTABLISHED;
                    attempt++;
                    continue;
                }
                s.state = CLOSED;
                break;
            case FIN_RCVD: /* server comes here to send FINACK to client */
                init_header(&hdr, FINACK, 0, NULL, 0);
                if ((count = sendto_maybe_hdr(sockfd, &hdr, sizeof(gbnhdr))) < 1){
                    DBG_ERROR("Error occured while sending");
                    attempt++;
                    continue;
                }
                s.state = CLOSED;
                break;
            case CLOSED:
                break;
        }
    }
    if (attempt == 10){     /* max amount of attempts reached, hang up */
        DBG_ERROR("Attempts limit reached. State: %d.", s.state);
        return -2;
    }
    return 0;
}

/* SYN, SYNACK packets will only compose of type and checksum field, no seqnum and data */
int gbn_connect(int sockfd, const struct sockaddr *server, socklen_t socklen){
    int count;
    int attempts = 0;
    gbnhdr hdr = {0};
    /* save server address */
    memcpy(&s.addr, server, socklen);
    s.len = socklen;
    signal(SIGALRM, ARLMHNDR);

    /* FSM starts here, try 10 times */
    while (s.state != ESTABLISHED) {
        if (attempts == 10) break;
        switch (s.state){
            case CLOSED:
                /* setup SYN packet */
                /* use a full buffer for syn packets*/
                init_header(&hdr, SYN, 0, NULL, 0);
                DBG_PRINT("Checksum: %d", hdr.checksum);
                if (sendto_maybe_hdr(sockfd, &hdr, sizeof(gbnhdr)) < 1){
                    DBG_ERROR("An error occured sending SYN");
                    attempts++;
                    continue;
                }
                DBG_PRINT("SYN_SENT Checkpoint");
                /* update state variables */
                s.state = SYN_SENT;
                break;
            case SYN_SENT:
                alarm(TIMEOUT);
                if ((count = recvfrom_hdr(sockfd, &hdr, SYNACK, 0, NULL, NULL, 1)) < 1){
                    DBG_ERROR("Did not receive FINACK");
                    attempts++;
                    /* reset set to CLOSED and resend */
                    s.state = CLOSED;
                    continue;
                }
                DBG_PRINT("ESTABLISHED Checkpoint");
                s.state = ESTABLISHED;
                s.ex_seqnum = 0;
                break;
            case ESTABLISHED:
                break;
        }
    }
    if (attempts == 10) {
        DBG_ERROR("Server hung up first!");
        return -2;
    }
    return 0;
}

int gbn_listen(int sockfd, int backlog){
	return 0;
}

int gbn_bind(int sockfd, const struct sockaddr *server, socklen_t socklen){
    int ret = -1;
    if ((ret = bind(sockfd, server, socklen)) < 0){
        /* bind returns -1 if binding to port fails */
        DBG_ERROR("Unable to bind to socket");
    }
	return ret;
}	

int gbn_socket(int domain, int type, int protocol){
		
	/*----- Randomizing the seed. This is used by the rand() function -----*/
	srand((unsigned)time(0));
    /* state at socket creation is always close (not connected) */
    s.state = CLOSED;
    /* return file descriptor for the socket */
    int fd = 0;
    if ((fd = socket(domain, type, protocol)) < 0){
        /* file descriptor can't be negative */
        DBG_ERROR("Unable to create socket");
    }
	return fd;
}

int gbn_accept(int sockfd, struct sockaddr *client, socklen_t *socklen){
    int count = 0;
    gbnhdr hdr = {0};

    /* FSM starts here */
    while (s.state != ESTABLISHED){
        switch(s.state){
            case CLOSED:
                if ((count = recvfrom_hdr(sockfd, &hdr, SYN, 0, client, socklen, 0)) < 1){
                    DBG_ERROR("Did not receive the SYN packet");
                    continue;
                }
                memcpy(&s.addr, client, *socklen);
                s.len = *socklen;
                s.state = SYN_RCVD;
                DBG_PRINT("SYN_RCVD checkpoint");
                break;
            case SYN_RCVD:
                init_header(&hdr, SYNACK, 0, NULL, 0);
                if (sendto_hdr(sockfd, &hdr, sizeof(gbnhdr)) < 1){
                    DBG_ERROR("Counld not send SYNACK");
                    continue;
                }
                s.state = ESTABLISHED;
                s.ex_seqnum = 0;
                DBG_PRINT("ESTABLISHED checkpoint");
                break;
            case ESTABLISHED:
                break;
        }
    }
    return 0;
}

ssize_t maybe_sendto(int s, const void *buf, size_t len, int flags, \
                     const struct sockaddr *to, socklen_t tolen){

	char *buffer = malloc(len);
	memcpy(buffer, buf, len);
	
	
	/*----- Packet not lost -----*/
	if (rand() > LOSS_PROB*RAND_MAX){
		/*----- Packet corrupted -----*/
		if (rand() < CORR_PROB*RAND_MAX){
			
			/*----- Selecting a random byte inside the packet -----*/
			int index = (int)((len-1)*rand()/(RAND_MAX + 1.0));

			/*----- Inverting a bit -----*/
			char c = buffer[index];
			if (c & 0x01)
				c &= 0xFE;
			else
				c |= 0x01;
			buffer[index] = c;
		}

		/*----- Sending the packet -----*/
		int retval = sendto(s, buffer, len, flags, to, tolen);
		free(buffer);
		return retval;
	}
	/*----- Packet lost -----*/
	else
		return(len);  /* Simulate a success */
}
