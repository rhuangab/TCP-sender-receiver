all:
	gcc -o reliable_sender sender_main.c
	g++ receiver_main.c -pthread -o reliable_receiver -std=c++11
	
clean:
	rm reliable_sender reliable_receiver