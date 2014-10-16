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

#define MAXBUFLEN 10100
#define BUFFER_TABLE_SIZE 300
#define TIMEOUT_TO_QUIT 5

typedef struct buffered_pkt{
	int pkt_id;
	char* data;
	int length;
}bpkt;

typedef struct tcpheader{
	int pkt_id;
	int size;
	int final_pck;
}tcpheader;

typedef struct tcpack{
	int ack_id;
}tcpack;

typedef struct receiver_window{
	int expected_id;
	int received_final_pkt;
	unsigned int total_received;
}receiver_window;

char* get_ack_msg(int ack_id){
	tcpack* tack = malloc(sizeof(tack));
	tack->ack_id = ack_id;
	return (char*)(tack);
}

void buffer_pkt(bpkt** oooBuffer, int pkt_id, char* data, int p_len){
	int key = pkt_id%BUFFER_TABLE_SIZE;
	if(oooBuffer[key] == NULL){
		oooBuffer[key] = malloc(sizeof(bpkt));
		oooBuffer[key]->pkt_id = pkt_id;
		oooBuffer[key]->data = (char*)malloc(sizeof(char) *p_len);
		memcpy(oooBuffer[key]->data, data, p_len);
		oooBuffer[key]->length = p_len;
	}
}

void remove_pkt(bpkt** oooBuffer, int pkt_id){
	int key= pkt_id % BUFFER_TABLE_SIZE;
	if(oooBuffer[key] != NULL && pkt_id == oooBuffer[key]->pkt_id){
		free(oooBuffer[key]->data);
		free(oooBuffer[key]);
		oooBuffer[key] = NULL;
	}
}

void check_buffer(bpkt** oooBuffer, receiver_window* rwnd, FILE* fp){
	while(1){
		int key = rwnd->expected_id % BUFFER_TABLE_SIZE;
		if(oooBuffer[key] != NULL && oooBuffer[key]->pkt_id == rwnd->expected_id){
			fwrite(oooBuffer[key]->data, sizeof(char),oooBuffer[key]->length, fp);
			rwnd->expected_id++;
			remove_pkt(oooBuffer, oooBuffer[key]->pkt_id);
			if((rwnd->expected_id-1) % 10 == 0){fflush(fp);}
		}
		else break;
	}
	return;
}

int readPacket(FILE *fp, receiver_window* rwnd, char* msg, int data_size, bpkt** oooBuffer){
	tcpheader* theader = (tcpheader *)msg;
	char* data = (char *)(theader + 1);
	//printf("###received packet data size: %d with byte %d, pkt_id: %d\n", theader->size, data_size, theader->pkt_id);
	////printf("------Content:------\n%s\n---------------\n", data);
	if(rwnd->expected_id == theader->pkt_id){
		if(theader->final_pck == 1){
			rwnd->received_final_pkt = 1;
			rwnd->expected_id++;
		}
		else{
			fwrite((char *)(theader+1), sizeof(char),theader->size, fp);
			rwnd->expected_id++;
			if(theader->pkt_id % 10 == 0){fflush(fp);}
			remove_pkt(oooBuffer, rwnd->expected_id-1);
			check_buffer(oooBuffer, rwnd, fp);
		}
		
	}
	else if(rwnd->expected_id < theader->pkt_id){
		buffer_pkt(oooBuffer, theader->pkt_id, (char *)(theader+1), theader->size);
		//return rwnd->expected_id-1;
	}
	else{
		//return -1;
	}
	return rwnd->expected_id-1;
}


// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

void reliablyReceive(unsigned short int myUDPport, char* destinationFile){
	int sockfd;
	struct addrinfo hints, *servinfo, *p;
	int rv;
	int numbytes;
	struct sockaddr_storage their_addr;
	char buf[MAXBUFLEN];
	socklen_t addr_len;
	char s[INET6_ADDRSTRLEN];
	bpkt *oooBuffer[BUFFER_TABLE_SIZE];
	int i = 0;
	for(; i < BUFFER_TABLE_SIZE; i++){
		oooBuffer[i] = NULL;
	}

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC; // set to AF_INET to force IPv4
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_PASSIVE; // use my IP
	char host_port_str[6];
	sprintf( host_port_str, "%d", myUDPport );
	if ((rv = getaddrinfo(NULL, host_port_str, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return;
	}

	// loop through all the results and bind to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			perror("listener: socket");
			continue;
		}

		if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
			perror("listener: bind");
			continue;
		}

		break;
	}

	if (p == NULL) {
		fprintf(stderr, "listener: failed to bind socket\n");
		return;
	}

	freeaddrinfo(servinfo);

	//printf("listener: waiting to recvfrom...\n");

	addr_len = sizeof their_addr;

	receiver_window* rwnd = malloc(sizeof(receiver_window));
	rwnd->expected_id = 1;
	rwnd->received_final_pkt = 0;
	rwnd->total_received = 0;
	FILE* fp = fopen(destinationFile, "wb");
	int sendDup = 3;
	int flag = 0;
	while(1){
		//set timeout
		struct timeval tv;
		tv.tv_sec = TIMEOUT_TO_QUIT;
		tv.tv_usec = 0;
		if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0) {
		    perror("Set timeout Error");
		}

		if ((numbytes = recvfrom(sockfd, buf, MAXBUFLEN-1 , 0,
			(struct sockaddr *)&their_addr, &addr_len)) == -1) {
			perror("recvfrom timeout");
			fclose(fp);
			close(sockfd);
			exit(1);
		}
		////printf("listener: packet contains \"%s\"\n", buf);
		if(numbytes > 0){
			rwnd->total_received++;
			////printf("received packets\n");
			int ack_id = readPacket(fp, rwnd, buf, numbytes, oooBuffer);
			if(ack_id != -1){
				//if(ack_id % sendDup == 0){ack_id = ack_id - 1;}
				//if(ack_id >= 5 && flag <= 5) {flag++; continue;}
				char* msg = get_ack_msg(ack_id);
				int numbytes2;
				if ((numbytes2 = sendto(sockfd, msg, sizeof(msg), 0, (struct sockaddr *)&their_addr, addr_len)) == -1) {
					perror("talker: sendto");
					exit(1); 
				};
				//printf("send out ack_id: %d with bytes: %d\n", ack_id, numbytes2);
				free(msg);
				if(rwnd->received_final_pkt){
					//printf("received all packets, exit.\n");
					printf("Total received: %d\n", rwnd->total_received);
					fclose(fp);
					exit(0);
				}
			}
		}
		/*
		//printf("listener: got packet from %s\n",
			inet_ntop(their_addr.ss_family,
				get_in_addr((struct sockaddr *)&their_addr),
				s, sizeof s));
		//printf("listener: packet is %d bytes long\n", numbytes);
		*/
		//buf[numbytes] = '\0';
		////printf("listener: packet contains \"%s\"\n", buf);
	}
	fclose(fp);
	close(sockfd);

	return;
}


int main(int argc, char** argv)
{
	unsigned short int udpPort;
	
	if(argc != 3)
	{
		fprintf(stderr, "usage: %s UDP_port filename_to_write\n\n", argv[0]);
		exit(1);
	}
	
	udpPort = (unsigned short int)atoi(argv[1]);
	
	reliablyReceive(udpPort, argv[2]);
}
