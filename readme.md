# go-back-n
## How to use this
```
./sender <hostname> <port> <filename>
./receiver <port> <filename>
```

## Client Side Finite State Machine
![alt text](https://firebasestorage.googleapis.com/v0/b/test-840a6.appspot.com/o/client_fsm.png?alt=media&token=0542f68b-798d-496e-be8e-94957244dfc0)

* CLOSED: This is where our TCP protocol will always be initiated as upon startup and end. After starting the program, the client will issue packet to the server with the SYN type set. Right after sending it, it will move the SYN_SENT state.
* SYN_SENT:
  * If the client is unable to receive a SYN_ACK packet type back, it will time out within 1
seconds and retrieve back to the CLOSED state.
  * If the client is able to receive a SYN_ACK packet type, it will proceed to become
ESTABLISH. In my protocol, I only use a two-way handshake due to the fact that the sender is the only one sending messages and the server is the only one that sends ACK messages.
* ESTABLISH: The client side implementation is done following the book, “Computer Network, A Top-Down Approach.” The client starts in slow mode to send one packet over to the server. If it is in fast mode, it will send two DATA type packets at once. After sending the packets, the client will set a single timer to wait for DATAACK results to come back. This timer is reset whenever a packet is received. The following logic is how these packets are processed when recvfrom() returns:
  * If it was interrupted by an alarm, the client will resend the DATA packets over to the server again. The protocol will retreat back to slow mode and send the first non- ACK’ed packet over.  
  * If the packets received are outside of the window range it is discarded o If packets are in windows range, they are assumed to be a cumulatively
acknowledged.
  * If all of the expected packets are not received in time, the window is updated to reflect the last ACK’ed packets.
* FIN_SENT: Once the client is finished reading the file, it will send a FIN type packet over to the server. After this, it will wait for the FIN_ACK package and return to the initial CLOSED state.

## Server Side Finite State Machine
![alt text](https://firebasestorage.googleapis.com/v0/b/test-840a6.appspot.com/o/server_fsm.png?alt=media&token=b82c7d59-7a40-4930-b09d-a7ab3a10be2c)

* CLOSED: In this state, the server is essentially waiting for any SYN packets from any client in order to transition to SYN_RCVD.
* SYN_RCVD: When the server transitions to this stage, it sends a SYN_ACK packet back to the server and goes to the ESTABLISHED state.
* ESTABLISHED: In the established state, the server is essentially processing any DATA packets it receives. The logic is as follows:
  * If the received packet is smaller or equal to the expected sequence number, it will ACK the sequence number back.
  * If the received packet is greater than the expected sequence number, it will ACK the number of the last received DATA packet.
  * If the received packet is a FIN packet, it will transition to FIN_RCVD.
* FIN_RCVD: In this state, the server simply sends a FIN_ACK packet and transitions to the
CLOSED state.

## How to test this

Put Tests folder in the root directory and run:
```
./test_files.sh
```
## Some issues in implementation

1. The connection and teardown mechanism as the program skeleton did not have an ACKing sequence number
2. When I sent my files to the regular sendto() function, it ran smoothly but when packet started dropping there was some situations when the sender and receiver became out of sync. It turned out that if my receiver needed to re-ACKed previous packets. 
3. I had some issue as the timing out mechanism was taking too long and the file transfers took too much time. I switch alarm for seitimer() to get a better resolution.
