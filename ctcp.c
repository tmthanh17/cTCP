/******************************************************************************
 * ctcp.c
 * ------
 * Implementation of cTCP done here. This is the only file you need to change.
 * Look at the following files for references and useful functions:
 *   - ctcp.h: Headers for this file.
 *   - ctcp_iinked_list.h: Linked list functions for managing a linked list.
 *   - ctcp_sys.h: Connection-related structs and functions, cTCP segment
 *                 definition.
 *   - ctcp_utils.h: Checksum computation, getting the current time.
 *
 *****************************************************************************/

#include "ctcp.h"
#include "ctcp_linked_list.h"
#include "ctcp_sys.h"
#include "ctcp_utils.h"
#include <inttypes.h>
//#define DEBUG
#undef  DEBUG
int byteOut = 0;
int byteIn  = 0;


typedef struct {
	//bytes_not_yet_injectd_to_STDIN = byte_read_from_STDIN - last_ready_not_sent_seqno;
	uint32_t last_not_ready_not_sent_seqno;

	//bytes_not_yet_injectd_to_network (pointer NXT) = last_ready_not_sent_seqno - last_sent_nak_seqno ;
	uint32_t last_ready_not_sent_seqno;

	//bytes_sent_and_acknowledged (pointer UNA) = last_sent_ack_seqno
	uint32_t last_sent_ack_seqno;  
	
	// bytes_sent_but_not_yet_acknoledged = last_sent_nak_seqno - last_sent_ack_seqno;
	uint32_t last_sent_nak_seqno;    
	
	// Note: send_window_size = last_ready_not_sent_seqno - last_sent_ack_seqno;
	
	// check the EOF
	bool check_EOF;
	
	// list contains unacknowledge segments 
	linked_list_t* wrapped_nak_segment;
}tx_state_t;


typedef struct{
	//bytes_receiv_and_ack = last_recv_seqno
	uint32_t last_recv_seqno;   

	uint32_t last_recv_ack_seqno;  

	// check the FIN
	bool check_FIN;  

	// list segments that are outputed to STDOUT             
	linked_list_t* output_segment;
}rx_state_t;

typedef struct{
	// the number segment sent
	uint8_t num_retransmit;

	// the last time segment sent
	long last_sent_time;

	ctcp_segment_t segment;
}wrapped_segment_t; 
/**
 * Connection state.
 *
 * Stores per-connection information such as the current sequence number,
 * unacknowledged packets, etc.
 *
 * You should add to this to store other fields you might need.
 */
struct ctcp_state {
	struct ctcp_state *next;  /* Next in linked list */
	struct ctcp_state **prev; /* Prev in linked list */

	conn_t *conn;             /* Connection object -- needed in order to figure
								out destination when sending */
	//linked_list_t *segments;  
								/* Linked list of segments sent to this connection.
								It may be useful to have multiple linked lists
								for unacknowledged segments, segments that
								haven't been sent, etc. Lab 1 uses the
								stop-and-wait protocol and therefore does not
								necessarily need a linked list. You may remove
								this if this is the case for you */

	/* FIXME: Add other needed fields. */
	long start_closed_timer;
	tx_state_t tx_state;
	rx_state_t rx_state;
	ctcp_config_t ctcp_config; 
};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;

/* FIXME: Feel free to add as many helper functions as needed. Don't repeat
          code! Helper functions make the code clearer and cleaner. */


/* Send responsive segment to other hosts when received data segment*/
void ctcp_respond_segment(ctcp_state_t *state, wrapped_segment_t *wrapped_segment);

/* Print state of host*/
void print_state(ctcp_state_t *state);

/* Print list of unacknowledged segment */
void print_wrapped_nak_segment(ctcp_state_t *state);

/* This function is used to print output segment*/
void print_output_segment(ctcp_state_t *state);

/* Send data segment to other host. Note Data < Window Size */
void ctcp_send_segment(ctcp_state_t *state, wrapped_segment_t *wrapped_segment);

/* Send all available data in STDIN to other host */
void ctcp_retransmit_segment(ctcp_state_t *state);

/* Clear segments that are acknowledged from the nak segment link list */
void ctcp_clean_wrapped_ack_segment(ctcp_state_t *state, ctcp_segment_t *segment);

/* This function is used to check duplicate segment*/
bool ctcp_check_segment(ctcp_state_t *state, ctcp_segment_t *segment);

/* This function is used to sort received segments to ensure the order of link list of output segment*/
void process_segment (ctcp_state_t *state, ctcp_segment_t *segment);



ctcp_state_t *ctcp_init(conn_t *conn, ctcp_config_t *cfg) {
    /* Connection could not be established. */
	if (conn == NULL) {
		return NULL;
  	}

  	/* Established a connection. Create a new state and update the linked list
     of connection states. */
	ctcp_state_t *state = calloc(sizeof(ctcp_state_t), 1);
	state->next = state_list;
	state->prev = &state_list;
	if (state_list)
		state_list->prev = &state;
	state_list = state;

	/* Set fields. */
	state->conn = conn;
	/* FIXME: Do any other initialization here. */

	/* Configuration of TX State */
	state->tx_state.last_sent_ack_seqno = 0;
	state->tx_state.last_sent_nak_seqno = 0;
	state->tx_state.last_ready_not_sent_seqno = 0;
	state->tx_state.last_not_ready_not_sent_seqno = 0;
	state->tx_state.check_EOF = false;
	state->tx_state.wrapped_nak_segment = ll_create();

	/* Configuration of RX State */
	state->rx_state.last_recv_ack_seqno = 0;
	state->rx_state.check_FIN = false;
	state->rx_state.output_segment = ll_create();

	/* Configuration of ctcp*/
	state->ctcp_config.recv_window = cfg->recv_window;
	state->ctcp_config.send_window = cfg->send_window;
	state->ctcp_config.timer = cfg->timer;
	state->ctcp_config.rt_timeout = cfg->rt_timeout;

	state->start_closed_timer = 0;
	return state;
}


void ctcp_destroy(ctcp_state_t *state) {
	/* Update linked list. */
	if (state->next)
		state->next->prev = state->prev;
		*state->prev = state->next;
		conn_remove(state->conn);

	/* FIXME: Do any other cleanup here. */
	unsigned int len, i;

	/* Free wrapped_nak_segment of state */
	len = ll_length(state->tx_state.wrapped_nak_segment);
	for (i = 0; i < len; i++){
		ll_node_t* first_node = ll_front(state->tx_state.wrapped_nak_segment);
		free(first_node->object);
		ll_remove(state->tx_state.wrapped_nak_segment, first_node);
	}
	ll_destroy(state->tx_state.wrapped_nak_segment);

	/* Free output_segment of state */
	len = ll_length(state->rx_state.output_segment);
	for (i = 0; i < len; i++){
		ll_node_t* first_node = ll_front(state->rx_state.output_segment);
		free(first_node->object);
		ll_remove(state->rx_state.output_segment, first_node);
	}
	ll_destroy(state->rx_state.output_segment);

	free(state);
	end_client();
}


/**
 * This is called if there is input to be read. To read the input, call
 * conn_input() with a buffer of the correct size. If no data is available,
 * conn_input() will return 0. ctcp_read() is called automatically by the
 * library when there is more input to read (so you never need to call it
 * yourself).
 *
 * conn_input() will return -1 when it reads an EOF. You should send a FIN to
 * the other side when this occurs. Then, you will need to destroy any
 * connection state once the conditions are satisfied (see ctcp_destroy()).
 *
 * Create a segment from the input and send it to the connection associated with
 * the passed in state (by calling conn_send()).
 *
 * state: State for the connection associated with this input. Get the
 *        associated connection object with state->conn.
 */
void ctcp_read(ctcp_state_t *state) {
	/* FIXME */
	int byteRead, byteSent ;
	uint8_t buff[MAX_SEG_DATA_SIZE];
	wrapped_segment_t *wrapped_segment;
	if (state->tx_state.check_EOF)
		return;
	/* Send data from user to other host*/
	while ((byteRead = conn_input(state->conn, buff, MAX_SEG_DATA_SIZE)) > 0) { 
		byteSent  = byteRead;
		wrapped_segment = calloc(1, sizeof(wrapped_segment_t) + byteSent); 
		wrapped_segment->segment.seqno = htonl(state->tx_state.last_ready_not_sent_seqno + 1);
		wrapped_segment->segment.len = htons(sizeof(ctcp_segment_t) + byteSent);
		buff[byteRead] = '\0';
		memcpy(wrapped_segment->segment.data, buff, byteSent);
		/* Add unacknowledged segmen data*/
		ll_add(state->tx_state.wrapped_nak_segment, wrapped_segment);
		state->tx_state.last_ready_not_sent_seqno += byteSent;
		byteIn += byteSent;
	}
	/* EOF occur need to send FIN to other host */
	if (byteRead == -1) {      
		wrapped_segment= calloc(1, sizeof(wrapped_segment_t));
		wrapped_segment->segment.seqno  = htonl(state->tx_state.last_ready_not_sent_seqno + 1);
		wrapped_segment->segment.len    = htons((uint16_t)sizeof(ctcp_segment_t));
		wrapped_segment->segment.flags |= TH_FIN;
		ll_add(state->tx_state.wrapped_nak_segment, wrapped_segment);
		state->tx_state.check_EOF = true;
	} 
//#ifdef DEBUG
//printf("byte Input %d\n", byteIn);
//#endif
	ctcp_retransmit_segment(state);
}


/**
 * This is called by the library when a segment is received. You should send
 * ACKs accordingly and output the segment's data to STDOUT if there is data.
 * To output, call on ctcp_output(), which you also must implement.
 *
 * The received segment MUST BE FREED after you are done with it.
 *
 * If you receive a FIN segment, you should output an EOF by calling
 * conn_output() with a length of 0. Then, you will need to destroy any
 * connection state once the conditions are satisfied (see ctcp_destroy()).
 *
 * state: Associated connection state.
 * segment: Segment received from the server. You should free this when you are
 *          done with it.
 * len: Length of the segment (including the headers). There might be extra
 *      padding so the received length might be larger than the length field in
 *      the segment header. The segment may have also been truncated (len is
 *      smaller than the length of the segment).
 */
void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
  	/* FIXME */
	wrapped_segment_t *new_wrapped_segment;

  	/* If segment is truncated */
  	if (ntohs(segment->len) > len){
#ifdef DEBUG
fprintf(stderr, "segment is truncated");
#endif
		free(segment);
   		return;
  	}

    /* Check the received segment */
	uint16_t checksum_recv = segment->cksum;
	segment->cksum = 0;   // when calculated checksum, the field checksum in segment must be 0
	uint16_t checksum_calc = cksum(segment, (uint16_t)(ntohs(segment->len)));
	segment->cksum = checksum_recv;
	if (checksum_calc != checksum_recv){
#ifdef DEBUG
fprintf(stderr, "failed checksum");
#endif
		free(segment);
		return;
  	} 


	/* Received Sucessfully */
	uint16_t byteRead = ntohs(segment->len) - sizeof(ctcp_segment_t);
	if(byteRead == -1)
		return;
#ifdef DEBUG
printf("The uint16_t value is: %" PRIu32 "\n", state->rx_state.last_recv_ack_seqno);
fprintf(stderr,"Received segment\n");
print_hdr_ctcp(segment);
#endif

    /* Received ack segment from other host to confirm that this host trasmitted segment successfully*/
	if (!byteRead){
#ifdef DEBUG
printf("NAK segments before cleaning\n");
print_wrapped_nak_segment(state);
#endif
		//state->tx_state.last_sent_ack_seqno = ntohl(segment->ackno) - 1;
      	ctcp_clean_wrapped_ack_segment(state, segment);
#ifdef DEBUG
printf("NAK segments after cleaning\n");
print_wrapped_nak_segment(state);
print_state(state);
#endif
    }

	// if (byteRead){
	// 	uint32_t last_recv_seqno = ntohl(segment->seqno) + strlen(segment->data) - 1;
	// 	uint32_t lowest_recv_window  = state->rx_state.last_recv_seqno + 1;
	// 	uint32_t highest_recv_window = state->rx_state.last_recv_seqno + state->ctcp_config.recv_window;
	// 	if ((last_recv_seqno < lowest_recv_window) || (last_recv_seqno > highest_recv_window)){
	// 		new_wrapped_segment = calloc(1, sizeof(wrapped_segment_t));
	// 		new_wrapped_segment->segment.ackno  = htonl(ntohl(segment->seqno) + byteRead);
	// 		new_wrapped_segment->segment.seqno = segment->ackno;
	// 		ctcp_respond_segment(state, new_wrapped_segment);
	// 		free(segment);
	// 		return;
	// 	}

	// }

  	/* If receiving a FIN segment or data segment, checking whether the received seqno  is the byte that host expected*/
  	if (ntohl(segment->seqno) <=  (state->rx_state.last_recv_ack_seqno + 1)){
		if (byteRead || (segment->flags & TH_FIN)){
			/* Handle duplicate segment */
			if (ctcp_check_segment(state, segment) == false)
				return;
			new_wrapped_segment = calloc(1, sizeof(wrapped_segment_t));
			new_wrapped_segment->segment.ackno  = htonl(ntohl(segment->seqno) + byteRead);
			new_wrapped_segment->segment.seqno = segment->ackno;
			if (ntohl(segment->seqno) < (state->rx_state.last_recv_ack_seqno + 1)){
				ctcp_respond_segment(state, new_wrapped_segment);
				/* Handle the delay segment */
				process_segment(state, segment);
				return;
			} else {
				state->rx_state.last_recv_ack_seqno = ntohl(segment->seqno) + byteRead - 1;
				ctcp_respond_segment(state, new_wrapped_segment);
				ll_add(state->rx_state.output_segment, segment);
			}
#ifdef DEBUG
fprintf(stderr,"Send segment\n");
print_hdr_ctcp(&new_wrapped_segment->segment);
print_state(state);
printf("length of link list %u\n", ll_length(state->tx_state.wrapped_nak_segment));
#endif
		} 
	} 
	/* There are some lost packet in network */
	else if (ntohl(segment->seqno) > (state->rx_state.last_recv_ack_seqno + 1)) {
		/* Selective Repeat */
		if (byteRead || (segment->flags & TH_FIN)){
			new_wrapped_segment = calloc(1, sizeof(wrapped_segment_t));
			state->rx_state.last_recv_ack_seqno = ntohl(segment->seqno) + byteRead - 1;
			new_wrapped_segment->segment.seqno  = segment->ackno;
			new_wrapped_segment->segment.ackno  = segment->seqno;//htonl(state->rx_state.last_recv_ack_seqno + 1);
			ctcp_respond_segment(state, new_wrapped_segment);
			ll_add(state->rx_state.output_segment, segment);
#ifdef DEBUG
fprintf(stderr,"Send segment\n");
print_hdr_ctcp(&new_wrapped_segment->segment);
print_state(state);
printf("length of link list %u\n", ll_length(state->tx_state.wrapped_nak_segment));
#endif
		}
		return;
		// /* GoBack N */
		// free(segment);
		// return;
  	}
	// printf("Before outputing\n");
	// print_output_segment(state);
	ctcp_output(state);
	// printf("After outputing\n");
	// print_output_segment(state);
}



/**
 * Outputs cTCP segments associated with the given ctcp_state_t object. This
 * should be called by ctcp_receive() if a segment is ready to be outputted.
 *
 * Before outputting a segment, you will need to call conn_bufspace() to see
 * how many bytes can be outputted to STDOUT. If there is no room, ctcp_output()
 * will automatically be called by the library when there is. Call conn_output()
 * in order to actually output the segment. If you call conn_output() with more
* data than conn_bufspace() says is available, not all of it may be written.
 *
 * You should flow control the sender by not acknowledging segments if there
 * is no buffer space available for conn_output().
 *
 * state: Associated connection state with the output.
 */
/* This function used for received host*/

void ctcp_output(ctcp_state_t *state) {
  	/* FIXME */
	int byteSent = 0;
	//print_output_segment(state);
	while (ll_length(state->rx_state.output_segment) != 0){
		ll_node_t* first_node = ll_front(state->rx_state.output_segment);
		ctcp_segment_t *segment = (ctcp_segment_t *) first_node->object;
		uint16_t data_segment = ntohs(segment->len) - sizeof(ctcp_segment_t);
		/* Check received data */
		if (data_segment){
// #ifdef DEBUG
// fprintf(stderr, "the number bytes data of segment %d", data_segment);
// #endif
			/* Check the space of STDOUT */
			if(data_segment > conn_bufspace(state->conn)){
#ifdef DEBUG
fprintf(stderr,"The value of size is: %zu\n", conn_bufspace(state->conn));
fprintf(stderr, "No space for buff");
#endif
        		return;
      		}
			/* Ouput data to STDOUT */
			byteSent = conn_output(state->conn, segment->data, data_segment);
			if (byteSent == -1 ){
#ifdef DEBUG
fprintf(stderr, "conn_output");
#endif
				return;
			}
			byteOut += byteSent;
    	}

	    /* If receiving a FIN segment, Outputing an EOF with a length 0*/
		if (segment->flags & TH_FIN){
			state->rx_state.check_FIN = true;
			conn_output(state->conn, segment->data, 0);
		}
		/*Remove segment after outputing */
		free(first_node->object);
		ll_remove(state->rx_state.output_segment, first_node);

    }
//#ifdef DEBUG
//printf("byte Output %d\n", byteOut);
//#endif
}




/**
 * Called periodically at specified rate (see the timer field in the
 * ctcp_config_t struct).
 *
 * You can use this timer to inspect segments and retransmit ones that have not
 * been acknowledged. Do not retransmit every segment every time the timer is
 * fired! A segment should only be retransmitted rt_timeout milliseconds after
 * it was last sent (also defined in the ctcp_config_t struct).
 *
 * After 5 retransmission attempts (so a total of 6 times) for a segment, you
 * should assume the other end of the connection is unresponsive and tear down
 * the connection (via a call to ctcp_destroy()).
 *
 * Note that this is called BEFORE ctcp_init() so state_list might be NULL.
 */
void ctcp_timer() {
	/* FIXME */
	ctcp_state_t *current_state;
	if (state_list ==  NULL)
		return;

	for (current_state = state_list; current_state != NULL; current_state = current_state->next){
		ctcp_retransmit_segment(current_state);
		/* When following requirements meet
		- You have received a FIN from the other side.
		- You have read an EOF or error from your input (conn_input returned -1)
		and have sent a FIN to the other side.
		- All sent segments (including the FIN) have been acknowledged.
		- All received segments have been outputted.
		*/
		if (current_state->tx_state.check_EOF && current_state->rx_state.check_FIN && (ll_length(current_state->tx_state.wrapped_nak_segment) == 0) && (ll_length(current_state->rx_state.output_segment) == 0)){
			if (current_state->start_closed_timer == 0){
				current_state->start_closed_timer = current_time();
			} else if ((current_time() - current_state->start_closed_timer) > (2 * MAX_SEG_LIFETIME_MS)  ){
				ctcp_destroy(current_state);
				return;
			}
		} 
	} 
}





void ctcp_send_segment(ctcp_state_t *state, wrapped_segment_t *wrapped_segment){
	wrapped_segment->segment.ackno  = htonl(state->rx_state.last_recv_ack_seqno + 1);
	wrapped_segment->segment.flags  |= TH_ACK;
	wrapped_segment->segment.window = htons(state->ctcp_config.recv_window);
	wrapped_segment->segment.cksum  = 0;
	wrapped_segment->segment.cksum  = cksum(&wrapped_segment->segment, ntohs(wrapped_segment->segment.len));
	int byteSent = conn_send(state->conn, &wrapped_segment->segment, ntohs(wrapped_segment->segment.len));
	wrapped_segment->last_sent_time = current_time();
	if (byteSent <  ntohs(wrapped_segment->segment.len)){
#ifdef DEBUG
fprintf(stderr,"conn_send");
#endif
		return;
	}

	if (byteSent == -1){
		ctcp_destroy(state);
		return;
	}
	wrapped_segment->num_retransmit ++;
}


void ctcp_respond_segment(ctcp_state_t *state, wrapped_segment_t *wrapped_segment){
	wrapped_segment->segment.len = htons(sizeof(ctcp_segment_t));
	wrapped_segment->segment.flags  |= TH_ACK;
	wrapped_segment->segment.window = htons(state->ctcp_config.recv_window);
	wrapped_segment->segment.cksum  = 0;
	wrapped_segment->segment.cksum  = cksum(&wrapped_segment->segment, ntohs(wrapped_segment->segment.len));
	conn_send(state->conn, &wrapped_segment->segment, sizeof(ctcp_segment_t));
}


void print_state(ctcp_state_t *state){
	printf("state->rx_state.last_recv_ack_seqno: %" PRIu32 " \n", state->rx_state.last_recv_ack_seqno);
	printf("state->tx_state.last_sent_ack_seqno: %" PRIu32 " \n", state->tx_state.last_sent_ack_seqno);
	printf("state->tx_state.last_sent_nak_seqno: %" PRIu32 " \n", state->tx_state.last_sent_nak_seqno);
}

void print_wrapped_nak_segment(ctcp_state_t *state){
	ll_node_t *current_node = ll_front(state->tx_state.wrapped_nak_segment);
	wrapped_segment_t *nak_wrapped_segment;
	while(current_node != NULL){
		nak_wrapped_segment = (wrapped_segment_t *) current_node->object;
		printf("nak_wrapped_segment.seqno: %" PRIu32 "\n",ntohl(nak_wrapped_segment->segment.seqno));
		current_node = current_node->next;
	}
}

void print_output_segment(ctcp_state_t *state){
	ll_node_t *current_node = ll_front(state->rx_state.output_segment);
	ctcp_segment_t *current_segment;
	while (current_node != NULL){
		current_segment = (ctcp_segment_t *) current_node->object;
		printf("output_segment: %" PRIu32 "\n", ntohl(current_segment->seqno));
		current_node = current_node->next;
	}
}

void ctcp_retransmit_segment(ctcp_state_t *state){
	if (ll_length(state->tx_state.wrapped_nak_segment) == 0)
		return;

	ll_node_t *current_node = ll_front(state->tx_state.wrapped_nak_segment);
	int i = 0;
	//uint16_t remaining_send_window; 
	uint32_t last_allowed_seqno;
	if (current_node == NULL)
		return;
	while (i < ll_length(state->tx_state.wrapped_nak_segment)){
		wrapped_segment_t *wrapped_nak_segment =  (wrapped_segment_t *) current_node->object;

		last_allowed_seqno = state->tx_state.last_sent_ack_seqno + state->ctcp_config.send_window;
		if (last_allowed_seqno < (ntohl(wrapped_nak_segment->segment.seqno) + strlen(wrapped_nak_segment->segment.data) - 1))
			return;


		if (wrapped_nak_segment->num_retransmit == 0){
			ctcp_send_segment(state, wrapped_nak_segment);
			state->tx_state.last_sent_nak_seqno += strlen(wrapped_nak_segment->segment.data);
			//remaining_send_window = state->ctcp_config.send_window - strlen(wrapped_nak_segment->segment.data);
#ifdef DEBUG
fprintf(stderr,"Send segment\n");
print_hdr_ctcp(&wrapped_nak_segment->segment);
print_state(state);
printf("num retransmit %d\n", wrapped_nak_segment->num_retransmit);
printf("check_EOF: %d\n", state->tx_state.check_EOF);
if (wrapped_nak_segment->segment.flags & TH_FIN)
fprintf(stderr,"FIN\n");
#endif  	
		}
		if (current_time() - wrapped_nak_segment->last_sent_time >= state->ctcp_config.rt_timeout){
			if (state->tx_state.last_sent_nak_seqno != state->tx_state.last_sent_ack_seqno){
				if (wrapped_nak_segment->num_retransmit >= MAX_NUM_XMITS){
					ctcp_destroy(state);
					return;
				}
				ctcp_send_segment(state, wrapped_nak_segment);
				//remaining_send_window = state->ctcp_config.send_window - strlen(wrapped_nak_segment->segment.data);
				//continue;
			} 
		}
		current_node = current_node->next;
		i++;
	}
}



void ctcp_clean_wrapped_ack_segment(ctcp_state_t *state, ctcp_segment_t *segment){
	ll_node_t *first_node = ll_front(state->tx_state.wrapped_nak_segment);
	while (first_node != NULL){
		wrapped_segment_t *acknowledged_wrapped_segment = (wrapped_segment_t *) first_node->object;
		if ((ntohl(acknowledged_wrapped_segment->segment.seqno) + strlen(acknowledged_wrapped_segment->segment.data)) == ntohl(segment->ackno)){
			state->tx_state.last_sent_ack_seqno += strlen(acknowledged_wrapped_segment->segment.data);
			free(acknowledged_wrapped_segment);
			ll_remove(state->tx_state.wrapped_nak_segment, first_node);
			break;
		} else {
			first_node =  first_node->next;
		}
	}
}


bool ctcp_check_segment(ctcp_state_t *state, ctcp_segment_t *segment){
	if (ll_length(state->rx_state.output_segment) == 0)
		return true;
	ll_node_t *current_node = ll_front(state->rx_state.output_segment);
	while (current_node != NULL){
		wrapped_segment_t *output_segment = (wrapped_segment_t *) current_node->object;
		if ((ntohl(segment->seqno) == ntohl(output_segment->segment.seqno))){ 
			free(segment);
			return false;
		}
		current_node = current_node->next;
	}
	return true;
}

void process_segment(ctcp_state_t *state, ctcp_segment_t *segment){
	if (ll_length(state->rx_state.output_segment) == 0){
		ll_add(state->rx_state.output_segment, segment);
	}
	else if (ll_length(state->rx_state.output_segment) == 1){
		ll_add_front(state->rx_state.output_segment, segment);
	}
	else{
		ll_node_t *current_node = ll_front(state->rx_state.output_segment);
		ctcp_segment_t *current_segment = (ctcp_segment_t *) current_node->object;
		ctcp_segment_t *next_segment = (ctcp_segment_t *) current_node->next->object;
		while (current_node != NULL){
			if ((ntohl(segment->seqno) > ntohl(current_segment->seqno)) ||
				(ntohl(segment->seqno) < ntohl(next_segment->seqno))){
				ll_add_after(state->rx_state.output_segment, current_node->next, segment);
				break;
			}
			current_node = current_node->next;
		}
	}
}