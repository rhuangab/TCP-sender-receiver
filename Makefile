all:
	gcc -o reliable_sender sender_main.c
	gcc -o reliable_receiver receiver_main.c
	
clean:
	rm reliable_sender reliable_receiver