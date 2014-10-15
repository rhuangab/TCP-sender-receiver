#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#define DATA_BYTES 500
#define THRESHOLD 50
#define TIMEOUT 50 * CLOCKS_PER_SEC /1000
#define MAXBUFLEN 500

typedef struct congest_win{
	int size;
	int threshold;
	int unack_pkt_id; //The oldest pkt id that is waiting for acknowledgement
	int next_pkt_id;
	int slowstart;
	clock_t timer;
	int timeout;
	int ca_rev_count; //count the number of ack received after congestion avoidance.
}congest_win;

typedef struct tcp{
	struct sockaddr *to;
	size_t tolen;
	struct congest_win* cwd;
	int dup_ack_num;
	int final_pid;
}tcp;

typedef struct tcpheader{
	int pkt_id;
	int size;
	int final_pck;
}tcpheader;

typedef struct tcpack{
	int ack_id;
}tcpack;

int next(struct congest_win* cwd){
	if(cwd->next_pkt_id - cwd->unack_pkt_id >= cwd->size){
		return -1;
	}
	return cwd->next_pkt_id;
}

int iss(struct congest_win* cwd){
	return cwd->size < cwd->threshold;
}

void sent_pkt(struct congest_win* cwd){
	++(cwd->next_pkt_id);
}

void ack_pkt(struct congest_win* cwd, int pkt_id){
	cwd->unack_pkt_id = pkt_id + 1;
	if(iss(cwd)){
		cwd->size += (pkt_id-cwd->unack_pkt_id + 1);
	}
}

char* writePacket(FILE *fp, tcpheader* th)
{
	if(th->final_pck){
		th->size = 0;
	}
	tcpheader* msg;
	size_t msgsize = th->size + sizeof(tcpheader);
	msg = malloc(msgsize); //allocate size for message
	memcpy(msg, th, sizeof(tcpheader));

	printf("pkt size:%zu, packet id: %d\n", msgsize, msg->pkt_id);
	if(!th->final_pck){
		char* data_head_pointer = (char*)(msg+1);
		fseek ( fp , DATA_BYTES * (th->pkt_id-1) , SEEK_SET );
		msg->size = fread(data_head_pointer, sizeof(char), sizeof(char) * msg->size, fp);
		th->size = msg->size;
		printf("packet contains %s, data size: %d\n", data_head_pointer, msg->size);
	}
	else{
		printf("final packet");
	}

	return (char*)msg;
}

void tcp_init(struct tcp* t, struct sockaddr *to, size_t tolen, unsigned long long int bytesToTransfer){
	t->to = to;
	t->tolen = tolen;
	t->dup_ack_num = 0;
	t->cwd = malloc(sizeof(struct congest_win));
	t->final_pid = bytesToTransfer%DATA_BYTES == 0? 
		bytesToTransfer/DATA_BYTES + 1
		:bytesToTransfer/DATA_BYTES + 2;
		//including the final signal packet.

	//initialize cwd
	t->cwd->size = 1;
	t->cwd->threshold = THRESHOLD;
	t->cwd->unack_pkt_id = 1;
	t->cwd->next_pkt_id = 1;
	t->cwd->timer = clock();
	t->cwd->timeout = TIMEOUT;
	t->cwd->ca_rev_count = 0;

}

void tcp_send(int sockfd, FILE* fp, struct tcp* t, int from_pid, int to_pid, int total_bytes){
	int i;
	// The packet id cannot exceed the final id number;
	if(to_pid > t->final_pid){
		to_pid = t->final_pid;
	}
	if(t->cwd->next_pkt_id > t->final_pid)
	{
		return;
	}

	for(i=from_pid; i <= to_pid; ++i){
		int data_size = DATA_BYTES;
		printf("sending packet %d\n", i);
		if(total_bytes - i * DATA_BYTES <0)
		{
			data_size = total_bytes % DATA_BYTES;
		}
		tcpheader th;
		th.pkt_id = i;
		th.size = data_size;
		th.final_pck = (t->final_pid == i);
		
		char* msg = writePacket(fp, &th);
		if(th.size == 0){
			//reach the end of file
			printf("Final packet id: %d\n", i);
			tcpheader* thead = (tcpheader*)msg;
			thead->final_pck = 1;
			t->final_pid = th.pkt_id;
		}
		int pkt_size = th.size + sizeof(tcpheader);

		int numbytes;
		printf("Sent packet with size %d", pkt_size);
		if ((numbytes = sendto(sockfd, (void *)msg, pkt_size, 0,
				 t->to, t->tolen)) == -1) {
			perror("talker: sendto");
			exit(1);
		}
		t->cwd->next_pkt_id++;
		printf("talker: sent %d bytes to %s\n", numbytes, t->to->sa_data);
	}
}

int check_timeout(struct congest_win* cwd){
	clock_t cur_time;
	cur_time = clock();
	printf("%lu,%lu, %d\n", cur_time, cwd->timer, cwd->timeout);
	if(cur_time - cwd->timer >= cwd->timeout){
		printf("timeout!...\n");
		return 1;
	}
	return 0;
}

void cut_congest_win(struct tcp* t, int choice){
	if(choice == 0){
		//triple dup ack
		t->cwd->threshold = t->cwd->size/2;
		t->cwd->size = t->cwd->threshold;
		t->cwd->next_pkt_id = t->cwd->unack_pkt_id;
		if(t->cwd->threshold < 1){
			t->cwd->threshold = 1;
		}
	}
	else{
		//timeout
		t->cwd->threshold = t->cwd->size/2;
		t->cwd->size = 1;
		t->cwd->next_pkt_id = t->cwd->unack_pkt_id;
		if(t->cwd->threshold < 1)
			t->cwd->threshold = 1;
	}
	t->cwd->timer = clock();
}

void read_ack(struct tcp* t, char* msg, int numbytes){
	if(numbytes < sizeof(tcpack)) return ;

	tcpack* data = (tcpack* )msg;
	printf("ack_id : %d, unack_pkt_id: %d\nwindow size:%d\n", data->ack_id, t->cwd->unack_pkt_id, t->cwd->size);
	if(data->ack_id == t->cwd->unack_pkt_id){
		t->cwd->unack_pkt_id++;
		if(iss(t->cwd)){
			t->cwd->size++;
		}
		else{
			t->cwd->ca_rev_count++;
			if(t->cwd->ca_rev_count >= t->cwd->size){
				t->cwd->size++;
				t->cwd->ca_rev_count = 0;
			}
		}
	}
	else if(data->ack_id == t->cwd->unack_pkt_id - 1){
		t->dup_ack_num++;
		if(t->dup_ack_num == 3){
			cut_congest_win(t, 0);
			t->dup_ack_num = 0;
		}
	}

}


void reliablyTransfer(char* hostname, unsigned short int hostUDPport, char* filename, unsigned long long int bytesToTransfer)
{
	int sockfd;
	struct addrinfo hints, *servinfo, *p;
	int rv;
	int numbytes;
	struct sockaddr_storage their_addr;
	char buf[MAXBUFLEN];
	socklen_t addr_len;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	char host_port_str[6];
	sprintf( host_port_str, "%d", hostUDPport );

	if ((rv = getaddrinfo(hostname, host_port_str, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return;
	}

	// loop through all the results and make a socket
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			perror("talker: socket");
			continue;
		}

		break;
	}

	if (p == NULL) {
		fprintf(stderr, "talker: failed to bind socket\n");
		return;
	}

	FILE* fp = fopen(filename,"rb");
	struct tcp* t = malloc(sizeof(tcp));
	tcp_init(t, p->ai_addr, p->ai_addrlen, bytesToTransfer);
	struct congest_win* cwd = t->cwd;
	while(1){
		
		if(cwd->unack_pkt_id - 1 == t->final_pid){
			printf("Finish transfer");
			exit(0);
		}
		if(cwd->next_pkt_id - cwd->unack_pkt_id <= cwd->size){
			tcp_send(sockfd, fp, t, cwd->next_pkt_id, cwd->unack_pkt_id + cwd->size - 1, bytesToTransfer);
		}

		//receive ack
		int numbytes = 0;
		printf("waiting receive\n");
		//set timeout
		struct timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = TIMEOUT;
		if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0) {
		    perror("Set timeout Error");
		}
		if ((numbytes = recvfrom(sockfd, buf, MAXBUFLEN-1 , 0,
			(struct sockaddr *)&their_addr, &addr_len)) == -1) {
			printf("Timeout\n");
			cut_congest_win(t, 1);
			continue;
			//perror("recvfrom");
		}
		printf("after receive\n");
		//printf("listener: packet contains \"%s\"\n", buf);
		if(numbytes > 0){
			printf("received packets\n");
			read_ack(t, buf, numbytes);
		}


	}
	fclose(fp);

	freeaddrinfo(servinfo);
	close(sockfd);

	return;
}

int main(int argc, char** argv)
{
	unsigned short int udpPort;
	unsigned long long int numBytes;
	
	if(argc != 5)
	{
		fprintf(stderr, "usage: %s receiver_hostname receiver_port filename_to_xfer bytes_to_xfer\n\n", argv[0]);
		exit(1);
	}
	udpPort = (unsigned short int)atoi(argv[2]);
	numBytes = atoll(argv[4]);
	
	reliablyTransfer(argv[1], udpPort, argv[3], numBytes);
} 
