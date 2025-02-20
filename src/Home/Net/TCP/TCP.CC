
U0 TCPGlobalsInit()
{
	tcp_globals.bound_socket_tree = NULL;
	tcp_globals.next_source_port  = 0;
}

Bool IsTCPStateSync(CTCPSocket *tcp_socket)
{ 
	switch (tcp_socket->state)
	{
		case TCP_STATE_ESTABLISHED:
		case TCP_STATE_FIN_WAIT1:
		case TCP_STATE_FIN_WAIT2:
		case TCP_STATE_CLOSE_WAIT:
		case TCP_STATE_CLOSING:
		case TCP_STATE_LAST_ACK:
		case TCP_STATE_TIME_WAIT:
			return TRUE;
		default:
			return FALSE;
	}
}

// TODO: Merge IPV4Checksum, TCPChecksumPartial, and TCPChecksumFinal into 1.
//		 All use same alg.
U16 TCPChecksumPartial(U32 sum, U8 *header, I64 length)
{ // same algorithm as IPV4Checksum, except sum is arg. First half.
  //todo. make names clearer, and better comments.
	I64  nleft = length;
	U16 *w = header;

	while (nleft > 1)
	{
		sum += *w++;
		nleft -= 2;
	}

	return sum;
}

U16 TCPChecksumFinal(U32 sum, U8 *header, I64 length)
{ // same algorithm as IPV4Checksum, except sum is arg. Both halves.
  //todo. make names clearer, and better comments.
	I64  nleft = length;
	U16 *w = header;

	while (nleft > 1)
	{
		sum += *w++;
		nleft -= 2;
	}

	// "mop up an odd byte, if necessary"
	if (nleft == 1)
	{
		sum += *w & 0x00FF;
	}

	// "add back carry outs from top 16 bits to low 16 bits"
	sum = sum >> 16 + sum & 0xFFFF; // "add hi 16 to low 16"
	sum += sum >> 16; // add carry
	return ~sum & 0xFFFF;

}

I64 TCPPacketAllocate(U8 **frame_out,
					  U32  source_ip,
					  U16  source_port,
					  U32  destination_ip,
					  U16  destination_port,
					  U32  seq_num,
					  U32  ack_num,
					  U8   flags,
					  I64  length)
{
	U8			*tcp_frame;
	I64			 de_index;
	CTCPHeader	*header;

	de_index = IPV4PacketAllocate(&tcp_frame,
								  IP_PROTOCOL_TCP,
								  source_ip,
								  destination_ip,
								  sizeof(CTCPHeader) + length);
	if (de_index < 0)
	{
		NetLog("TCP PACKET ALLOCATE: Ethernet Frame Allocate failed.");
		return de_index;
	}

	header = tcp_frame;

	header->source_port			= EndianU16(source_port);
	header->destination_port	= EndianU16(destination_port);

	NetDebug("TCP PACKET ALLOCATE: Outputting header port src/dest 0x%0X/0x%0X:",
				header->source_port, header->destination_port);

	header->seq_num				= EndianU32(seq_num);
	header->ack_num				= EndianU32(ack_num);
	header->data_offset			= (sizeof(CTCPHeader) / 4) << 4; // ???
	header->flags				= flags;
	header->window_size			= EndianU16(TCP_WINDOW_SIZE / 2);// shrine has FIXME ... what should window size be...
	header->checksum			= 0;
	header->urgent_pointer		= 0;

	*frame_out = tcp_frame + sizeof(CTCPHeader);

	return de_index;
}

U0 TCPPacketFinish(I64			  de_index,
				   U32			  source_ip,
				   U32			  destination_ip,
				   U8			 *frame,
				   I64			  length,
				   CTCPAckQueue **send_buffer_out=NULL)
{
	CTCPHeader		 *header		= frame - sizeof(CTCPHeader);
	CTCPPseudoHeader *pseudoheader	= CAlloc(sizeof(CTCPPseudoHeader));
	U32				  checksum;
	CTCPAckQueue	 *send_buffer;

	pseudoheader->source_address		= EndianU32(source_ip);
	pseudoheader->destination_address	= EndianU32(destination_ip);
	pseudoheader->zeros					= 0;
	pseudoheader->protocol				= IP_PROTOCOL_TCP;
	pseudoheader->tcp_length			= EndianU16(sizeof(CTCPHeader) + length);

	checksum = TCPChecksumPartial(0, pseudoheader, sizeof(CTCPPseudoHeader));
	header->checksum = TCPChecksumFinal(checksum, header, sizeof(CTCPHeader) + length);

	if (send_buffer_out)
	{
		send_buffer = CAlloc(sizeof(CTCPAckQueue));

		send_buffer->time_sent			= tS;
		send_buffer->retries			= 0;
		send_buffer->start_seq_num		= EndianU32(header->seq_num);
		send_buffer->end_seq_num		= 0; // must set this upstream
		send_buffer->tcp_packet_size	= length;
		send_buffer->tcp_packet			= CAlloc(sizeof(CTCPHeader) + length);

		MemCopy(send_buffer->tcp_packet, frame, sizeof(CTCPHeader) + length);

		*send_buffer_out = send_buffer; // set output param as new send buffer
	}

	IPV4PacketFinish(de_index);
}

I64 TCPSend(U32 source_ip,
			U16 source_port,
			U32 destination_ip,
			U16 destination_port,
			U32 seq_num,
			U32 ack_num,
			U8  flags)
{ // send raw unsocketed TCP packet, flags and/or data. Does not store into ACK Queue (Send Buffer).

	U8 *payload_frame;
	I64 de_index = TCPPacketAllocate(&payload_frame,
									 source_ip,
									 source_port,
									 destination_ip,
									 destination_port,
									 seq_num,
									 ack_num,
									 flags,
									 0);
	if (de_index < 0)
	{
		NetErr("TCP SEND: TCP Packet Allocate failed.");
		return de_index;
	}

	TCPPacketFinish(de_index, source_ip, destination_ip, payload_frame, 0, NULL);

	return 0;
}

I64 TCPSendFlags(CTCPSocket *tcp_socket, U8 flags)
{ // send TCP packet, flags only. Stores into ACK Queue (Send Buffer) if flags SYN or FIN.

	U8						*payload_frame;
	CSocketAddressStorage	*dest	= &tcp_socket->destination_address;
	CSocketAddressStorage	*source	= &tcp_socket->source_address;
	CSocketAddressIPV4		*dest_ipv4;
	CSocketAddressIPV6		*dest_ipv6;
	CSocketAddressIPV4		*source_ipv4;
	CSocketAddressIPV6		*source_ipv6;
	I64						 de_index;
	CTCPAckQueue			*send_buffer	 = NULL;
	CTCPAckQueue		   **send_buffer_ptr = NULL;

	NetDebug("TCP SEND FLAGS: Attempting Send using param TCP socket");

	switch (dest->family)
	{
		case AF_INET:
			dest_ipv4 = dest;

			switch (source->family)
			{
				case AF_INET:
					source_ipv4 = source;

					NetDebug("TCP SEND FLAGS: Trying TCP Packet Allocate... port src/dest 0x%0X/0x%0X",
								source_ipv4->port, dest_ipv4->port);

					de_index = TCPPacketAllocate(&payload_frame,
												 source_ipv4->address.address,
												 EndianU16(source_ipv4->port),
												 dest_ipv4->address.address,
												 EndianU16(dest_ipv4->port),
												 tcp_socket->next_send_seq_num,
												 tcp_socket->next_recv_seq_num,
												 flags,
												 0);
					if (de_index < 0)
					{
						NetErr("TCP SEND FLAGS: TCP Packet Allocate failed.");
						return de_index;
					}

					break;
				case AF_INET6:
					source_ipv6 = source;
					Debug("TODO: IPV6 Not implemented yet");
					return -1;
				case AF_UNSPEC:
					Debug("TODO: Error TCP Send from AF_UNSPEC\n");
					return -1;
			}

			break;
		case AF_INET6:
			dest_ipv6 = dest;
			Debug("TODO: IPV6 Not implemented yet");
			return -1;
		case AF_UNSPEC:
			Debug("TODO: Error TCP Send to AF_UNSPEC\n");
			return -1;
	}

	// at this point, a TCP packet has been allocated.
	// TODO: Is there a better way to manage links across different address types?

	if (Bt(&flags, TCPf_SYN))
		tcp_socket->next_send_seq_num++;

	if (Bt(&flags, TCPf_FIN))
		tcp_socket->next_send_seq_num++;

	if (Bt(&flags, TCPf_FIN) || Bt(&flags, TCPf_SYN))
	{
		send_buffer_ptr = &send_buffer;
	}


	switch (dest->family)
	{
		case AF_INET:
			dest_ipv4 = dest;

			switch (source->family)
			{
				case AF_INET:
					source_ipv4 = source;

					TCPPacketFinish(de_index,
									source_ipv4->address.address,
									dest_ipv4->address.address,
									payload_frame,
									0,
									send_buffer_ptr);

					if (send_buffer)
					{ // modifying ptr should affect our send_buffer variable
						send_buffer->end_seq_num = tcp_socket->next_send_seq_num;

						// append it to the socket, ACK Queue needs to be ready (head)...
						QueueInsertRev(send_buffer, tcp_socket->ack_queue);

					}

					break;
				case AF_INET6:
					source_ipv6 = source;
					Debug("TODO: IPV6 Not implemented yet");
					return -1;
				case AF_UNSPEC:
					Debug("TODO: Error TCP Send from AF_UNSPEC\n");
					return -1;
			}

			break;
		case AF_INET6:
			dest_ipv6 = dest;
			Debug("TODO: IPV6 Not implemented yet");
			return -1;
		case AF_UNSPEC:
			Debug("TODO: Error TCP Send to AF_UNSPEC\n");
			return -1;
	}
}

I64 TCPSendData(CTCPSocket *tcp_socket, U8 flags, U8 *data, I64 length)
{ // send TCP packet, flags and data. Stores into ACK Queue (Send Buffer).

	U8						*payload_frame;
	CSocketAddressStorage	*dest	= &tcp_socket->destination_address;
	CSocketAddressStorage	*source	= &tcp_socket->source_address;
	CSocketAddressIPV4		*dest_ipv4;
	CSocketAddressIPV6		*dest_ipv6;
	CSocketAddressIPV4		*source_ipv4;
	CSocketAddressIPV6		*source_ipv6;
	I64						 de_index;
	CTCPAckQueue			*send_buffer;

	NetDebug("TCP SEND DATA: Attempting Send using param TCP socket");

	switch (dest->family)
	{
		case AF_INET:
			dest_ipv4 = dest;

			switch (source->family)
			{
				case AF_INET:
					source_ipv4 = source;

					NetDebug("TCP SEND DATA: Trying TCP Packet Allocate... port src/dest 0x%0X/0x%0X",
								source_ipv4->port, dest_ipv4->port);

					de_index = TCPPacketAllocate(&payload_frame,
												 source_ipv4->address.address,
												 EndianU16(source_ipv4->port),
												 dest_ipv4->address.address,
												 EndianU16(dest_ipv4->port),
												 tcp_socket->next_send_seq_num,
												 tcp_socket->next_recv_seq_num,
												 flags,
												 length);
					if (de_index < 0)
					{
						NetLog("TCP SEND FLAGS: TCP Packet Allocate failed.");
						return de_index;
					}

					break;
				case AF_INET6:
					source_ipv6 = source;
					Debug("TODO: IPV6 Not implemented yet");
					return -1;
				case AF_UNSPEC:
					Debug("TODO: Error TCP Send from AF_UNSPEC\n");
					return -1;
			}

			break;
		case AF_INET6:
			dest_ipv6 = dest;
			Debug("TODO: IPV6 Not implemented yet");
			return -1;
		case AF_UNSPEC:
			Debug("TODO: Error TCP Send to AF_UNSPEC\n");
			return -1;
	}

	// at this point, a TCP packet has been allocated.
	// TODO: Is there a better way to manage links across different address types?

	if (length)
		MemCopy(payload_frame, data, length);

	if (Bt(&flags, TCPf_SYN))
		tcp_socket->next_send_seq_num++;

	tcp_socket->next_send_seq_num += length;

	if (Bt(&flags, TCPf_FIN))
		tcp_socket->next_send_seq_num++;

	switch (dest->family)
	{
		case AF_INET:
			dest_ipv4 = dest;

			switch (source->family)
			{
				case AF_INET:
					source_ipv4 = source;

					TCPPacketFinish(de_index,
									source_ipv4->address.address,
									dest_ipv4->address.address,
									payload_frame,
									length,
									&send_buffer);

					send_buffer->end_seq_num = tcp_socket->next_send_seq_num;

					// append it to the socket, ACK Queue needs to be ready (head)...
					QueueInsertRev(send_buffer, tcp_socket->ack_queue);

					break;
				case AF_INET6:
					source_ipv6 = source;
					Debug("TODO: IPV6 Not implemented yet");
					return -1;
				case AF_UNSPEC:
					Debug("TODO: Error TCP Send from AF_UNSPEC\n");
					return -1;
			}

			break;
		case AF_INET6:
			dest_ipv6 = dest;
			Debug("TODO: IPV6 Not implemented yet");
			return -1;
		case AF_UNSPEC:
			Debug("TODO: Error TCP Send to AF_UNSPEC\n");
			return -1;
	}
}

I64 TCPPacketParse(CTCPHeader **header_out,
				   U8		  **data_out,
				   I64		   *length_out,
				   CIPV4Packet *packet)
{
	CTCPHeader		 *header		= packet->data;
	I64				  header_length	= (header->data_offset >> 4) * 4; // ?? why
//	U32				  checksum, sum;
//	CTCPPseudoHeader *pseudoheader	= CAlloc(sizeof(CTCPPseudoHeader));

	// TODO: Shrine has FIXME, validate packet->length!

	// Verify Checksum:
/*	NetWarn("DEBUG: TCPPACKETPARSE:		0x%X = checksum from packet, verifying...", EndianU16(header->checksum));
	pseudoheader->source_address		= EndianU32(packet->source_ip_address);
	pseudoheader->destination_address	= EndianU32(packet->destination_ip_address);
	pseudoheader->zeros					= 0;
	pseudoheader->protocol				= IP_PROTOCOL_TCP;
	pseudoheader->tcp_length			= EndianU16(packet->length);

	checksum = TCPChecksumPartial(0, pseudoheader, sizeof(CTCPPseudoHeader));
	NetWarn("DEBUG: TCPPACKETPARSE:		0x%X = partial checksum", checksum);

	Free(pseudoheader); // No longer needed, discard

	sum = TCPChecksumFinal(checksum, header, packet->length);

	if (sum != EndianU16(header->checksum))
	{
		NetErr("DEBUG: TCPPACKETPARSE:		0x%X = NOT MATCH", sum);
		//return -1; // is checksum not being 0 in received header going to mess this up?..
	}
	// Checksum verified.
*/

	*header_out = header;
	*data_out	= packet->data   + header_length;
	*length_out	= packet->length - header_length;

	return 0;
}

U0 TCPAcknowledgePacket(CTCPSocket *tcp_socket, U32 segment_ack)
{
	F64				 time = tS;
	F64				 rtt;
	CTCPAckQueue	*send_buffer = tcp_socket->ack_queue->next;
	CTCPAckQueue	*send_buffer_temp;
	I64				 segment_ack_relative;
	I64				 send_next_relative;

	NetDebug("TCP ACKNOWLEDGE PACKET: Looking to see if there are any send buffers");

	while (send_buffer != tcp_socket->ack_queue)
	{
		segment_ack_relative	= (segment_ack - send_buffer->end_seq_num) & 0xFFFFFFFF;
		send_next_relative		= (tcp_socket->next_send_seq_num - send_buffer->end_seq_num) & 0xFFFFFFFF;
		// ???

		if (segment_ack_relative <= send_next_relative)
		{
			NetDebug("TCP ACKNOWLEDGE PACKET: Saw send buffer entry with ACK <= next relative send ACK, removing.");
			rtt = time - send_buffer->time_sent; // Round-Trip Time

			tcp_socket->srtt = (tcp_socket->srtt * TCP_SRTT_ALPHA) + ((1.0 - TCP_SRTT_ALPHA) * rtt);
			// Smoothed Round-Trip Time

			QueueRemove(send_buffer);
			Free(send_buffer->tcp_packet);

			send_buffer_temp = send_buffer->next; // QueueRemove doesn't change removed queue's links
			Free(send_buffer);

			send_buffer = send_buffer_temp;
			
		}
		else
			break;
	}
}

U0 TCPCheckACKQueue(CTCPSocket *tcp_socket)
{
	F64						 time			= tS;
	F64						 rto			= TCP_RTO_BETA * tcp_socket->srtt; // Retransmission Timeout, Smoothed Round-Trip Time
	CTCPAckQueue			*send_buffer	= tcp_socket->ack_queue->next;
	CTCPAckQueue			*first_buffer	= send_buffer;
	U8						*tcp_frame;
	I64						 de_index;
	CSocketAddressStorage	*dest			= &tcp_socket->destination_address;
	CSocketAddressStorage	*source			= &tcp_socket->source_address;
	CSocketAddressIPV4		*dest_ipv4;
	CSocketAddressIPV6		*dest_ipv6;
	CSocketAddressIPV4		*source_ipv4;
	CSocketAddressIPV6		*source_ipv6;


	if (send_buffer == tcp_socket->ack_queue)
	{
//		NetWarn("TCP CHECK ACK QUEUE: ACK Queue empty, bailing early.");
		return;
	}

	if (rto < TCP_RTO_MIN)
		rto = TCP_RTO_MIN;
	if (rto < TCP_RTO_MAX)
		rto = TCP_RTO_MAX;

	do
	{
		if (time > send_buffer->time_sent + rto)
		{
			NetWarn("TCP CHECK ACK QUEUE: Retransmitting TCP packet in ACK Queue.");

			switch (dest->family)
			{
				case AF_INET:
					dest_ipv4 = dest;

					switch (source->family)
					{
						case AF_INET:
							source_ipv4 = source;

							de_index = IPV4PacketAllocate(&tcp_frame,
														  IP_PROTOCOL_TCP,
														  source_ipv4->address.address,
														  dest_ipv4->address.address,
														  send_buffer->tcp_packet_size);

							if (de_index < 0)
							{
								NetErr("TCP CHECK ACK QUEUE: IPV4 Packet Allocate failed.");
								return;// de_index;
							}

							MemCopy(&tcp_frame, send_buffer->tcp_packet, send_buffer->tcp_packet_size);

							IPV4PacketFinish(de_index);

							break;

						case AF_INET6:
							source_ipv6 = source;
							Debug("TODO: IPV6 Not implemented yet");
							return;// -1;
						case AF_UNSPEC:
							Debug("TODO: Error TCP Send from AF_UNSPEC\n");
							return;// -1;
					}

					break;
				case AF_INET6:
					dest_ipv6 = dest;
					Debug("TODO: IPV6 Not implemented yet");
					return;// -1;
				case AF_UNSPEC:
					Debug("TODO: Error TCP Send to AF_UNSPEC\n");
					return;// -1;
			}

			send_buffer->time_sent = tS;

			// If it gets here, retransmit was successful.

			// Move to the END OF THE CHAIN (BACK OF THE QUEUE. SO, REQUEUE.)
			QueueRemove(send_buffer);
			QueueInsertRev(send_buffer, tcp_socket->ack_queue);

			send_buffer = tcp_socket->ack_queue->next;

		}
		else
			break;
	}
	while (send_buffer != first_buffer);
}

CTCPSocket TCPSocket(U16 domain=AF_UNSPEC)
{
	U16					 type			= SOCKET_STREAM;
	CTCPSocket			*tcp_socket		= CAlloc(sizeof(CTCPSocket));
	CTCPAckQueue		*ack_queue		= CAlloc(sizeof(CTCPAckQueue));
	CTCPAcceptQueue		*accept_queue	= CAlloc(sizeof(CTCPAcceptQueue));
	CSocketAddressIPV4	*ipv4_address;
	CSocketAddressIPV6	*ipv6_address;

	tcp_socket->socket	= Socket(domain, type);
	tcp_socket->state	= TCP_STATE_CLOSED;

	tcp_socket->source_address.family = domain; // INET, INET6, or unspecified

	switch (domain)
	{
		case AF_INET:
			ipv4_address = &tcp_socket->source_address;

			ipv4_address->address.address = IPV4AddressGet;
			// do we need to set local port? does that only make sense to do later?
			// CAlloc will leave it at 0

			break;

		case AF_INET6:
			ipv6_address = &tcp_socket->source_address;
			Debug("TODO: IPV6 Not implemented yet");
			return NULL;

		case AF_UNSPEC:
			Debug("TODO: Error TCP Socket to AF_UNSPEC");
			return NULL;
	}

	QueueInit(ack_queue);  // init send buffer queue head
	tcp_socket->ack_queue = ack_queue;

	QueueInit(accept_queue); // init pending connection queue
	tcp_socket->accept_queue = accept_queue;

	tcp_socket->receive_buffer_size = TCP_WINDOW_SIZE;
	tcp_socket->receive_buffer = CAlloc(TCP_WINDOW_SIZE);

	tcp_socket->max_segment_size = TCP_MSS;

	return tcp_socket;
}

I64 TCPSocketBind(CTCPSocket *tcp_socket, CSocketAddressStorage *address)
{ // Binds a TCP socket to address, which contains the local port and remote address to use.
	CTCPTreeNode	   *head = tcp_globals.bound_socket_tree;
	CTCPTreeNode	   *temp_node;
	CSocketAddressIPV4 *ipv4_address;
	CSocketAddressIPV4 *ipv4_destination;
	CSocketAddressIPV4 *ipv4_source;
	CSocketAddressIPV6 *ipv6_address;
	CSocketAddressIPV6 *ipv6_destination;
	U16					port;

	if (!SocketBind(tcp_socket->socket))
	{
		NetErr("TCP SOCKET BIND: Failed, Socket state-machine must be in READY state.");
		return -1;
	}

	if (tcp_socket->state != TCP_STATE_CLOSED)
	{
		NetErr("TCP SOCKET BIND: Failed, TCP Socket must be in CLOSED state.");
		return -1;
	}

	switch (address->family)
	{
		case AF_INET:

			if (tcp_socket->source_address.family == AF_INET6)
			{
				Debug("TODO: IPV6->IPV4 TCP BIND");
				return -1;
			}

			ipv4_address = address;
			ipv4_destination = &tcp_socket->destination_address;

			ipv4_destination->family			= AF_INET;
			ipv4_destination->address.address	= ipv4_address->address.address; // bind socket to address in parameter.
//			ipv4_destination->port				= ipv4_address->port; // ... consistency would say keep in Big Endian ...

			ipv4_source = &tcp_socket->source_address;

			ipv4_source->port = ipv4_address->port;

			port = EndianU16(ipv4_address->port); // port member should be Big Endian,  so now we're going L.E (?)

			break;

		case AF_INET6:

			if (tcp_socket->source_address.family == AF_INET)
			{
				NetErr("TCP SOCKET BIND: Incompatible Address type.");
				return -1;
			}

			ipv6_address = address;
			ipv6_destination = &tcp_socket->destination_address;
			// ...
			// ...

			port = EndianU16(ipv6_address->port); // port member should be Big Endian,  so now we're going L.E (?)

			Debug("TODO: IPV6 TCP BIND");

			break;

		case AF_UNSPEC:
			Debug("TODO: AF_UNSPEC TCP BIND -- param family");
			break;
	}

	if (head)
	{
		// look for our port.
		temp_node = TCPTreeNodeFind(port, head);

		if (temp_node)
		{ // if we find we have bound sockets at port, check address before adding to queue
			switch (address->family)
			{
				case AF_INET:
					if (TCPTreeNodeQueueIPV4Find(ipv4_destination->address.address, temp_node, TRUE))
					{
						NetErr("TCP SOCKET BIND: Address already in Bound Socket Tree !");
						return -1;
					}
					else
					{ // if no address match, free to add socket to the node queue
						NetDebug("TCP SOCKET BIND: No address match, adding socket to node queue");
						TCPTreeNodeQueueAdd(tcp_socket, temp_node);
					}

					break;

				case AF_INET6:
					Debug("TODO: IPV6 TCP BIND");
					break;

				case AF_UNSPEC:
					Debug("TODO: AF_UNSPEC TCP BIND -- found in bound tree");
					break;
			}
		}
		else
		{ // if we get no node back from port search, we didn't find it and are free to add a new node.
			temp_node = TCPTreeNodeParamAdd(port, head); // add new node with port, return its *.
			TCPTreeNodeQueueAdd(tcp_socket, temp_node);
		}
	}
	else // if no bound sockets, we init the tree as a new node
	{
		tcp_globals.bound_socket_tree = head = TCPTreeNodeParamInit(port); //... shouuuld be in L.E .. ?
		TCPTreeNodeQueueAdd(tcp_socket, head); // add the tcp socket to the port queue
		// maybe more checks to do before this, dunno rn.
	}


	// do we need to do anything else after bind add to tree? ...


	switch (tcp_socket->socket->state)
	{
		case SOCKET_STATE_BIND_REQ: //	if BIND request success, set BOUND.
			tcp_socket->socket->state = SOCKET_STATE_BOUND;
			break;

		default:
			NetErr("TCP SOCKET BIND: Failed, Misconfigured Socket state-machine.");
			return -1;
	}

	return 0;
}

I64 TCPSocketClose(CTCPSocket *tcp_socket)
{
	CTCPTreeNode		*head = tcp_globals.bound_socket_tree;
	CTCPTreeNode		*node;
	CTCPTreeQueue		*queue;
	CTCPAckQueue		*send_buffer;
	CTCPAcceptQueue		*pending;
	CSocketAddressIPV4	*address = &tcp_socket->source_address;
	I64					 timeout;

	SocketClose(tcp_socket->socket); // TODO: testing on closing a socket while another task is using it

	switch (tcp_socket->state)
	{
		case TCP_STATE_ESTABLISHED:
			while (TCPSendFlags(tcp_socket, TCPF_FIN | TCPF_ACK) < 0)
			{
				TCPCheckACKQueue(tcp_socket);
				Sleep(1);
			}

			tcp_socket->state = TCP_STATE_FIN_WAIT1;

			// Shrine has TODOs:	- What states are allowed here?
			//						- This can block forever if our recv buffer fills up
			//					 	  but remote insists on sending more.
			// Implementing timeout here to force close.
			timeout = counts.jiffies + tcp_socket->timeout * JIFFY_FREQ / 1000;

			while (tcp_socket->state == TCP_STATE_FIN_WAIT1 &&
				   tcp_socket->first_unacked_seq != tcp_socket->next_send_seq_num &&
				   counts.jiffies < timeout)
			{
				TCPCheckACKQueue(tcp_socket);
				Sleep(1);
			}

			if (tcp_socket->state == TCP_STATE_FIN_WAIT1)
				tcp_socket->state = TCP_STATE_FIN_WAIT2;

			timeout = counts.jiffies + tcp_socket->timeout * JIFFY_FREQ / 1000;
			while (tcp_socket->state == TCP_STATE_FIN_WAIT2 && counts.jiffies < timeout)
				Sleep(1);

			break;

		case TCP_STATE_CLOSE_WAIT:
			while (TCPSendFlags(tcp_socket, TCPF_FIN | TCPF_ACK) < 0)
			{
				TCPCheckACKQueue(tcp_socket);
				Sleep(1);
			}

			if (tcp_socket->state == TCP_STATE_CLOSE_WAIT)
				tcp_socket->state = TCP_STATE_LAST_ACK;

			timeout = counts.jiffies + tcp_socket->timeout * JIFFY_FREQ / 1000;
			while (tcp_socket->state == TCP_STATE_LAST_ACK &&
				   tcp_socket->first_unacked_seq != tcp_socket->next_send_seq_num &&
				   counts.jiffies < timeout)
			{
				TCPCheckACKQueue(tcp_socket);
				Sleep(1);
			}

			break;
	}

	if (IsTCPStateSync(tcp_socket))
		TCPSendFlags(tcp_socket, TCPF_RST);

	// remove from bound list, free associated allocs

	node = TCPTreeNodeFind(EndianU16(address->port), head);

	if (node)
		queue = TCPTreeNodeQueueSocketFind(tcp_socket, node);
	else
	{
		Debug("Didn't find node at socket during TCPSocketClose!\n");
		return -1;
	}

	if (queue)
	{
		TCPTreeNodeQueueSocketSinglePop(tcp_socket, node);

		if (node->queue == node->queue->next)
		{ // if we popped the only queue on the node, remove the node.
			if (node == head)
			{ // head is the global. if node is the global, change it and add branches.
				if (node->left)
				{
					tcp_globals.bound_socket_tree = head = node->left;
					if (node->right)
						TCPTreeNodeAdd(node->right, head);
				}
				else if (node->right)
					tcp_globals.bound_socket_tree = node->right;
				else
					tcp_globals.bound_socket_tree = NULL;
			}
			else // if node is not the global, just pop it from the tree
				TCPTreeNodeSinglePop(node->value, head);

			Free(node);
		}

		Free(tcp_socket->socket);

		NetWarn("TCP SOCKET CLOSE: Freeing socket, ACK queue, accept() queue, & receive buffer.");
		Free(tcp_socket->receive_buffer);

		send_buffer = tcp_socket->ack_queue->next;
		while (send_buffer != tcp_socket->ack_queue)
		{
			NetWarn("TCP SOCKET CLOSE: Freeing send buffer @ 0x%0X", send_buffer);
			QueueRemove(send_buffer);
			Free(send_buffer->tcp_packet);
			Free(send_buffer);
			send_buffer = tcp_socket->ack_queue->next;
		}

		pending = tcp_socket->accept_queue->next;
		while (pending != tcp_socket->accept_queue)
		{
			NetWarn("TCP SOCKET CLOSE: Freeing pending connection @ 0x%0X", pending);
			QueueRemove(pending);
			Free(pending);
			pending = tcp_socket->accept_queue->next;
		}


		Free(tcp_socket);
		Free(queue);
	}
	else
	{
		Debug("Didn't find queue at socket during TCPSocketClose!\n");
		return -1;
	}
	
	return 0;

}

I64 TCPSocketConnect(CTCPSocket *tcp_socket, CSocketAddressStorage *address)
{ // Bind and connect a socket to the destination address and port in address param, local port automatic.
	U16					source_port = EndianU16(0x8000 + tcp_globals.next_source_port++ & 0x7FFF);
	CTCPTreeNode	   *head = tcp_globals.bound_socket_tree;
	CTCPTreeNode	   *temp_node;
	CSocketAddressIPV4 *ipv4_address;
	CSocketAddressIPV4 *ipv4_destination;
	CSocketAddressIPV4 *ipv4_source;
	CSocketAddressIPV6 *ipv6_address;
	CSocketAddressIPV6 *ipv6_destination;
	I64					timeout;
	U16					port;


	if (!SocketConnect(tcp_socket->socket))
	{
		NetErr("TCP SOCKET CONNECT: Failed, Socket state-machine must be in READY state.");
		return -1;
	}

	if (tcp_socket->state != TCP_STATE_CLOSED)
	{
		NetErr("TCP SOCKET CONNECT: TCP Socket must be in CLOSED state.");
		return -1;
	}

	switch (address->family)
	{
		case AF_INET:

			if (tcp_socket->source_address.family == AF_INET6)
			{
				Debug("TODO: IPV6->IPV4 TCP CONNECT");
				return -1;
			}

			ipv4_address = address;
			ipv4_destination = &tcp_socket->destination_address;

			ipv4_destination->family			= AF_INET;
			ipv4_destination->address.address	= ipv4_address->address.address; // bind socket to address in parameter.
			ipv4_destination->port				= ipv4_address->port; // ... consistency would say keep in Big Endian ...

			ipv4_source = &tcp_socket->source_address;

			ipv4_source->port = source_port;

			port = EndianU16(source_port); // port member should be Big Endian,  so now we're going L.E (?)

			break;

		case AF_INET6:

			if (tcp_socket->source_address.family == AF_INET)
			{
				NetErr("TCP SOCKET CONNECT: Incompatible Address type.");
				return -1;
			}

			ipv6_address = address;
			ipv6_destination = &tcp_socket->destination_address;
			// ...
			// ...

			port = EndianU16(source_port); // port member should be Big Endian,  so now we're going L.E (?)

			Debug("TODO: IPV6 TCP CONNECT");

			break;

		case AF_UNSPEC:
			Debug("TODO: AF_UNSPEC TCP CONNECT -- param family");
			break;
	}

	if (head)
	{
		// look for our port.
		temp_node = TCPTreeNodeFind(port, head);

		if (temp_node)
		{ // if we find we have bound sockets at port, check address before adding to queue
			switch (address->family)
			{
				case AF_INET:
					// TODO: will any INADDR_ANY sockets bound at the port break this?
					if (TCPTreeNodeQueueIPV4Find(ipv4_destination->address.address, temp_node, TRUE))
					{
						NetErr("TCP SOCKET CONNECT: Address already in Bound Socket Tree !");
						return -1;
					}
					else
					{ // if no address match, free to add socket to the node queue
						NetDebug("TCP SOCKET CONNECT: No address match, adding socket to node queue");
						TCPTreeNodeQueueAdd(tcp_socket, temp_node);
					}

					break;

				case AF_INET6:
					Debug("TODO: IPV6 TCP CONNECT");
					break;

				case AF_UNSPEC:
					Debug("TODO: AF_UNSPEC TCP CONNECT -- found in bound tree");
					break;
			}
		}
		else
		{ // if we get no node back from port search, we didn't find it and are free to add a new node.
			temp_node = TCPTreeNodeParamAdd(port, head); // add new node with port, return its *.
			TCPTreeNodeQueueAdd(tcp_socket, temp_node);
		}
	}
	else // if no bound sockets, we init the tree as a new node
	{
		tcp_globals.bound_socket_tree = head = TCPTreeNodeParamInit(port); //... shouuuld be in L.E .. ?
		TCPTreeNodeQueueAdd(tcp_socket, head); // add the tcp socket to the port queue
		// maybe more checks to do before this, dunno rn.
	}

	tcp_socket->connection_time = tS;
	tcp_socket->receive_window	= TCP_WINDOW_SIZE;

	tcp_socket->state = TCP_STATE_SYN_SENT;
	TCPSendFlags(tcp_socket, TCPF_SYN);

	timeout = counts.jiffies + tcp_socket->timeout * JIFFY_FREQ / 1000;
	while (counts.jiffies < timeout)
	{
		switch (tcp_socket->state)
		{
			case TCP_STATE_ESTABLISHED:
			case TCP_STATE_CLOSED:
				timeout = 0; // break out of while loop
				break;

			default:
				Sleep(1);
		}
	}

	if (tcp_socket->state != TCP_STATE_ESTABLISHED)
		return -1;

	switch (tcp_socket->socket->state)
	{
		case SOCKET_STATE_CONNECT_REQ: //	if CONNECT request success, set OPEN.
			tcp_socket->socket->state = SOCKET_STATE_OPEN;
			break;

		default:
			NetErr("TCP SOCKET CONNECT: Failed, Misconfigured Socket state-machine.");
			return -1;
	}

	return 0;
}

I64 TCPSocketListen(CTCPSocket *tcp_socket, I64 backlog_size)
{ // Set a bound socket to Listen for incoming connections. Backlog size is max amount of waiting connections allowed
	if (!SocketListen(tcp_socket->socket))
	{
		NetErr("TCP SOCKET LISTEN: Socket state-machine must be in BOUND state.");
		return -1;
	}

	if (tcp_socket->state != TCP_STATE_CLOSED)
	{
		NetErr("TCP SOCKET LISTEN: TCP Socket must be in CLOSED state.");
		return -1;
	}

	tcp_socket->state = TCP_STATE_LISTEN;
	tcp_socket->accept_queue_limit = backlog_size;

	switch (tcp_socket->socket->state)
	{
		case SOCKET_STATE_LISTEN_REQ: //	if LISTEN request success, set BOUND.
			tcp_socket->socket->state = SOCKET_STATE_LISTENING;
			break;

		default:
			NetErr("TCP SOCKET BIND: Failed, Misconfigured Socket state-machine.");
			return -1;
	}

	return 0;
}

CTCPSocket *TCPSocketAccept(CTCPSocket *tcp_socket)
{ // Accepts & creates a new socket, uses timeout inherited from Listening socket.
	CTCPAcceptQueue 	*pending;
	CTCPSocket			*new_socket;
	CSocketAddressIPV4	*temp_addr;
	CSocketAddressIPV4	 ipv4_address;
	I64					 timeout;

	if (!SocketAccept(tcp_socket->socket))
	{
		NetErr("TCP SOCKET ACCEPT: Failed, Socket state-machine must be in LISTENING state.");
		return NULL;
	}

	if (tcp_socket->state != TCP_STATE_LISTEN)
	{
		NetErr("TCP SOCKET LISTEN: TCP Socket must be in LISTEN state.");
		return NULL;
	}

	timeout = counts.jiffies + tcp_socket->timeout * JIFFY_FREQ / 1000;
	while ((pending = tcp_socket->accept_queue->next) == tcp_socket->accept_queue)
	{
		if (counts.jiffies > timeout)
			return NULL;
		else
			Sleep(1);
			// Yield;
	}

	QueueRemove(pending); // whether successful accept() or not, remove pending connection.

// TODO: rework accept logic to handle IPV6 addresses
	new_socket = TCPSocket(AF_INET);

	new_socket->next_recv_seq_num	= ++pending->segment_seq_num;
	new_socket->connection_time		= tS;
	new_socket->receive_window		= TCP_WINDOW_SIZE;
	new_socket->timeout				= tcp_socket->timeout;

	temp_addr = &tcp_socket->source_address;

	ipv4_address.family				= AF_INET;
//	ipv4_address.port				= pending->port;
	ipv4_address.port				= temp_addr->port;
	ipv4_address.address.address	= pending->ipv4_address;

	NetDebug("TCP SOCKET ACCEPT: Attempting to Bind to pending connection %0X @ src/dst ports %d,%d.",
				pending->ipv4_address, ipv4_address.port, pending->port);

	if (TCPSocketBind(new_socket, &ipv4_address) == -1)
	{
		Free(pending);
		return NULL;
	}

	temp_addr = &new_socket->destination_address;

	temp_addr->port = pending->port;

	new_socket->state = TCP_STATE_SYN_RECEIVED;

	NetDebug("TCP SOCKET ACCEPT: Attempting Send Flags SYN ACK back to requester.");
	TCPSendFlags(new_socket, TCPF_SYN | TCPF_ACK);

	timeout = counts.jiffies + new_socket->timeout * JIFFY_FREQ / 1000;
	while (counts.jiffies < timeout)
	{
		switch (new_socket->state)
		{
			case TCP_STATE_ESTABLISHED:
			case TCP_STATE_CLOSED:
				timeout = 0; // break out of while loop
				break;

			default:
				Sleep(1);
		}
	}

	if (new_socket->state != TCP_STATE_ESTABLISHED)
	{
		TCPSocketClose(new_socket);
		Free(pending);
		return NULL;
	}

	Free(pending);

	new_socket->socket->state = SOCKET_STATE_OPEN;

	return new_socket;
}

I64 TCPSocketReceive(CTCPSocket *tcp_socket, U8 *buffer, I64 length)
{
	I64 read_position;
	I64 write_position;
	I64 read_total = 0;
	I64 step;
	I64 timeout;

	if (!SocketReceive(tcp_socket->socket))
	{
		NetErr("TCP SOCKET RECEIVE: Failed, Socket state-machine must be in OPEN or BOUND state.");
		return NULL;
	}

	timeout = counts.jiffies + tcp_socket->timeout * JIFFY_FREQ / 1000;

	while ((tcp_socket->state == TCP_STATE_ESTABLISHED || tcp_socket->state == TCP_STATE_FIN_WAIT1) &&
		   tcp_socket->read_position == tcp_socket->write_position)
	{
		TCPCheckACKQueue(tcp_socket);
		Sleep(1);

		if (counts.jiffies > timeout)
		{
//			if (tcp_socket->timeout != 0) // Don't flood NetLog on non-blocking receives.
//				NetErr("TCP SOCKET RECEIVE: Timed out.");

//			return -1;
			break;
		}


	}

	// Shrine has TODO: Should still be able to receive in closing states ...
	if ((tcp_socket->state != TCP_STATE_ESTABLISHED || tcp_socket->state == TCP_STATE_FIN_WAIT1) &&
		tcp_socket->read_position == tcp_socket->write_position || length == 0)
		return 0;


	if (counts.jiffies > timeout)
	{
		if (tcp_socket->timeout != 0) // Don't flood NetLog on non-blocking receives.
			NetErr("TCP SOCKET RECEIVE: Timed out.");

		return -1;
	}


	read_position	= tcp_socket->read_position;
	write_position	= tcp_socket->write_position;

	if (write_position < read_position)
	{
		step = tcp_socket->receive_buffer_size - read_position;

		if (step > length)
			step = length;

		MemCopy(buffer, tcp_socket->receive_buffer + read_position, step);
		buffer += step;
		length -= step;
//		read_position = (read_position + step) & (tcp_socket->receive_buffer_size - 1);
		read_position = (read_position + step) % tcp_socket->receive_buffer_size;
		read_total += step;
	}

	if (length > 0)
	{
		step = write_position - read_position;

		if (step > length)
			step = length;

		MemCopy(buffer, tcp_socket->receive_buffer + read_position, step);
		buffer += step;
		length -= step;
		read_position += step;
		read_total += step;
	}

	tcp_socket->read_position = read_position;

	return read_total;
}

I64 TCPSocketSend(CTCPSocket *tcp_socket, U8 *buffer, I64 length)
{
	I64 sent_total = 0;
	I64 timeout = counts.jiffies + tcp_socket->timeout * JIFFY_FREQ / 1000;
	I64 send_length;

	if (!SocketSend(tcp_socket->socket))
	{
		NetErr("TCP SOCKET SEND: Failed, Socket state-machine must be in OPEN state.");
		return -1;
	}

	while ((tcp_socket->state == TCP_STATE_ESTABLISHED || tcp_socket->state == TCP_STATE_CLOSE_WAIT) &&
		   length)
	{
		send_length = (tcp_socket->first_unacked_seq + tcp_socket->send_window - tcp_socket->next_send_seq_num) & 0xFFFFFFFF;

		// Shrine has TODO: Keep trying, tie to timeout: RFC 793 "Managing The Window"

		if (send_length == 0)
		{
			if (sent_total > 0)
				break;
			else
			{
				TCPCheckACKQueue(tcp_socket);
				Sleep(1);
			}
		}
		else
		{
			if (send_length > length)
				send_length = length;

			if (send_length > tcp_socket->max_segment_size)
				send_length = tcp_socket->max_segment_size;

			NetDebug("TCP SOCKET SEND: Trying TCPSendData() of %d bytes.", send_length);
			if (TCPSendData(tcp_socket, TCPF_ACK, buffer, send_length) < 0)
			{ // Stall until outgoing data acknowledged.
				if (sent_total > 0)
					break;
				else
				{
					TCPCheckACKQueue(tcp_socket);
					Sleep(1);
				}

			}
			else
			{
				buffer		+= send_length;
				length		-= send_length;
				sent_total	+= send_length;
			}
		}

		if (counts.jiffies > timeout)
		{
			if (tcp_socket->timeout != 0) // Don't flood NetLog on non-blocking sends.
				NetErr("TCP SOCKET SEND: Timed out.");

			break;
		}
	}

	return sent_total;
}

// TODO: TCPRep()

TCPGlobalsInit;