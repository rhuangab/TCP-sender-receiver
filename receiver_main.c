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

#define MAXBUFLEN 500

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
}receiver_window;

char* get_ack_msg(int ack_id){
	tcpack* tack = malloc(sizeof(tack));
	tack->ack_id = ack_id;
	return (char*)(tack);
}

int readPacket(FILE *fp, receiver_window* rwnd, char* msg, int data_size){
	tcpheader* theader = (tcpheader *)msg;
	char* data = (char *)(theader + 1);
	printf("received packet data size: %d with byte %d, contains %s\n", theader->size, data_size, data);
	if(rwnd->expected_id == theader->pkt_id){
		if(theader->final_pck == 1){
			rwnd->received_final_pkt = 1;
			rwnd->expected_id++;
		}
		else{
			fwrite((char *)(theader+1), sizeof(char),theader->size, fp);
			rwnd->expected_id++;
			if(theader->pkt_id % 10 == 0){fflush(fp);}
		}
		return rwnd->expected_id-1;
	}
	else if(rwnd->expected_id < theader->pkt_id){
		return rwnd->expected_id-1;
	}
	else{
		return -1;
	}
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

	printf("listener: waiting to recvfrom...\n");

	addr_len = sizeof their_addr;

	receiver_window* rwnd = malloc(sizeof(receiver_window));
	rwnd->expected_id = 1;
	rwnd->received_final_pkt = 0;
	FILE* fp = fopen(destinationFile, "ab");
	while(1){
		if ((numbytes = recvfrom(sockfd, buf, MAXBUFLEN-1 , 0,
			(struct sockaddr *)&their_addr, &addr_len)) == -1) {
			perror("recvfrom");
			exit(1);
		}
		//printf("listener: packet contains \"%s\"\n", buf);
		if(numbytes > 0){
			printf("received packets\n");
			int ack_id = readPacket(fp, rwnd, buf, numbytes);
			if(ack_id != -1){
				char* msg = get_ack_msg(ack_id);
				int numbytes2;
				if ((numbytes2 = sendto(sockfd, msg, sizeof(msg), 0, (struct sockaddr *)&their_addr, addr_len)) == -1) {
					perror("talker: sendto");
					exit(1);
				};
				free(msg);
				if(rwnd->received_final_pkt){
					printf("received all packets, exit.\n");
					fclose(fp);
					exit(0);
				}
			}
		}
		/*
		printf("listener: got packet from %s\n",
			inet_ntop(their_addr.ss_family,
				get_in_addr((struct sockaddr *)&their_addr),
				s, sizeof s));
		printf("listener: packet is %d bytes long\n", numbytes);
		*/
		//buf[numbytes] = '\0';
		//printf("listener: packet contains \"%s\"\n", buf);
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
