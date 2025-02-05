// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;

// callback pentru parser http
static int aws_on_path_cb(http_parser *parser, const char *buffer, size_t length)
{	//buffer este calea ureleului
	struct connection *conn = (struct connection *)parser->data;

	for (size_t i = 0; i < length; i++)
		conn->request_path[i] = buffer[i];
	conn->request_path[length] = '\0';
	conn->have_path = 1; // se marhceaza faptul ca calea a fost procesata

	return 0;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the reply header. */

	// clear before usage
	for (size_t i = 0; i < sizeof(conn->send_buffer); i++)
		conn->send_buffer[i] = 0;

	
	int header_length = snprintf(conn->send_buffer, sizeof(conn->send_buffer),
								 "HTTP/1.1 200 OK Content-Length: %ld\r\n\r\n", conn->file_size);

	conn->send_len = header_length;
}

static void connection_prepare_send_404(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the 404 header. */

	for (size_t i = 0; i < sizeof(conn->send_buffer); i++)
		conn->send_buffer[i] = 0;

	int header_length = snprintf(conn->send_buffer, sizeof(conn->send_buffer),
								 "HTTP/1.1 404 Not Found Content-Length: 0\r\n\r\n");

	conn->send_len = header_length;
	conn->file_size = 0;
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	/* TODO: Get resource type depending on request path/filename. Filename should
	 * point to the static or dynamic folder.
	 */

	char *static_match = strstr(conn->request_path, "static");

	if (static_match != NULL)
		return RESOURCE_TYPE_STATIC;

	char *dynamic_match = strstr(conn->request_path, "dynamic");

	if (dynamic_match != NULL)
		return RESOURCE_TYPE_DYNAMIC;

	return RESOURCE_TYPE_NONE;
}

static void initialize_connection_buffers(struct connection *conn)
{
	// receive and send buffers
	memset(conn->recv_buffer, 0, BUFSIZ);
	memset(conn->send_buffer, 0, BUFSIZ);
}

static void initialize_connection_socket(struct connection *conn, int sockfd)
{
	// some socket related fields init
	conn->sockfd = sockfd;
	conn->fd = -1;
	conn->eventfd = eventfd(0, EFD_NONBLOCK);
}

static void initialize_connection_state(struct connection *conn)
{
	// initial states
	conn->state = STATE_INITIAL;
	conn->res_type = RESOURCE_TYPE_NONE;
}

struct connection *connection_create(int sockfd)
{
	struct connection *conn = malloc(sizeof(*conn));

	DIE(conn == NULL, "malloc");

	initialize_connection_state(conn);
	initialize_connection_socket(conn, sockfd);
	initialize_connection_buffers(conn);

	conn->ctx = ctx;

	return conn;
}

void connection_start_async_io(struct connection *conn)
{
	/* TODO: Start asynchronous operation (read from file).
	 * Use io_submit(2) & friends for reading data asynchronously.
	 */
	conn->state = STATE_ASYNC_ONGOING;
	struct iocb *prep_io = &conn->iocb;

	io_prep_pread(prep_io, conn->fd, conn->send_buffer, BUFSIZ, conn->file_pos);
	conn->piocb[0] = prep_io;
	io_set_eventfd(prep_io, conn->eventfd);

	if (io_submit(conn->ctx, 1, conn->piocb) < 0) {
		perror("io_submit");
		exit(EXIT_FAILURE);
	}

	// update epoll with file descriptors
	int epoll_fd = epollfd;
	
	w_epoll_add_ptr_in(epoll_fd, conn->eventfd, conn);
	
	w_epoll_update_ptr_in(epoll_fd, conn->sockfd, conn);
}

void connection_remove(struct connection *conn)
{
	if (conn->fd != -1) {
		int close_result = close(conn->fd);

		if (close_result < 0)
			perror("Error closing file descriptor");
	}

	int close_eventfd_result = close(conn->eventfd);

	if (close_eventfd_result < 0)
		perror("Error closing eventfd");

	int close_sockfd_result = close(conn->sockfd);

	if (close_sockfd_result < 0)
		perror("Error closing socket");
	// change its state to close
	conn->state = STATE_CONNECTION_CLOSED;

	free(conn);
}

void handle_new_connection(void)
{
	/* TODO: Handle a new connection request on the server socket. */
	struct connection *new_conn;
	struct sockaddr_in client_address;
	static int server_socket;
	socklen_t client_addr_len = sizeof(struct sockaddr_in);

	// accept new connection
	server_socket = accept(listenfd, (SSA *)&client_address, &client_addr_len);
	if (server_socket < 0) {
		perror("accept");
		exit(EXIT_FAILURE);
	}

	// setting socket to non-blocking
	int flags = fcntl(server_socket, F_GETFL, 0);

	if (fcntl(server_socket, F_SETFL, flags | O_NONBLOCK) < 0) {
		perror("set to be non-blocking");
		exit(EXIT_FAILURE);
	}

	new_conn = connection_create(server_socket);

	// add the socket
	if (w_epoll_add_ptr_in(epollfd, server_socket, new_conn) < 0) {
		perror("w_epoll_add_ptr_in");
		exit(EXIT_FAILURE);
	}

	http_parser_init(&new_conn->request_parser, HTTP_REQUEST);
}

void receive_data(struct connection *conn)
{
	/* TODO: Receive message on socket.
	 * Store message in recv_buffer in struct connection.
	 */
	conn->state = STATE_RECEIVING_DATA;

	ssize_t received_bytes;
	// keep it going while we are receiving data
	while (1) {
		received_bytes = recv(conn->sockfd, conn->recv_buffer + conn->recv_len, BUFSIZ, 0);

		// if no data received, stop
		if (received_bytes <= 0)
			break;

		conn->recv_len += received_bytes;
	}

	int header_status = parse_header(conn);
	int file_status = connection_open_file(conn);

	if (header_status < 0 || file_status < 0) {
		// sends 404 message for failure
		connection_prepare_send_404(conn);
		conn->state = STATE_SENDING_404;
		return;
	}

	// mark data as received
	conn->state = STATE_REQUEST_RECEIVED;

	// update epoll with the new state
	int epoll_update_status = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);

	if (epoll_update_status < 0) { //failure
		perror("w_epoll_update_ptr_out");
		exit(EXIT_FAILURE);
	}
}

int connection_open_file(struct connection *conn)
{
	conn->fd = open(conn->filename, O_RDONLY);

	if (conn->fd < 0)
		return -1;

	struct stat file_info;

	// get file stats
	int fstat_result = fstat(conn->fd, &file_info);

	if (fstat_result < 0) {
		perror("fstat");
		return -1;
	}

	// assign file size and position from the stat structure
	conn->file_size = file_info.st_size;
	conn->file_pos = 0;

	return 0;
}

static void prepare_for_sending_data(struct connection *conn, size_t bytes_read)
{
	conn->state = STATE_SENDING_DATA;
	conn->send_len = bytes_read;
	conn->send_pos = 0;
	conn->file_pos += bytes_read;
}

void connection_complete_async_io(struct connection *conn)
{
	struct io_event events[10];

	if (io_getevents(ctx, 1, 1, events, NULL) < 0)
		perror("io_getevents");

	size_t bytes_read = events[0].res;

	prepare_for_sending_data(conn, bytes_read);

	w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
	w_epoll_remove_ptr(epollfd, conn->eventfd, conn);
}

int parse_header(struct connection *conn)
{
	/* TODO: Parse the HTTP header and extract the file path. */

	// Define the HTTP parser settings with only on_path callback set
	http_parser_settings path_parser_settings = {
		.on_message_begin = NULL,
		.on_header_field = NULL,
		.on_header_value = NULL,
		.on_path = aws_on_path_cb,
		.on_url = NULL,
		.on_fragment = NULL,
		.on_query_string = NULL,
		.on_body = NULL,
		.on_headers_complete = NULL,
		.on_message_complete = NULL};

	// setting connection data for the parser
	conn->request_parser.data = conn;

	// parse on the received buffer
	size_t bytes_processed = http_parser_execute(&conn->request_parser, &path_parser_settings,
												 conn->recv_buffer, conn->recv_len);

	if (conn->have_path) {
		conn->res_type = connection_get_resource_type(conn);

		// no valid source found
		if (conn->res_type == RESOURCE_TYPE_NONE)
			return -1;

		// filename
		snprintf(conn->filename, BUFSIZ, "%s%s", AWS_DOCUMENT_ROOT, conn->request_path + 1);
	}

	return 0;
}

static int send_static_data(struct connection *conn, off_t *off)
{
	return sendfile(conn->sockfd, conn->fd, off, conn->file_size - *off);
}

enum connection_state connection_send_static(struct connection *conn)
{
	off_t off = conn->file_pos;
	int bytes_sent = send_static_data(conn, &off);

	if (bytes_sent < 0) {
		connection_prepare_send_404(conn);
		return STATE_SENDING_404;
	}

	conn->file_pos += bytes_sent;
	return (conn->file_pos == conn->file_size) ? STATE_DATA_SENT : STATE_SENDING_DATA;
}

int connection_send_data(struct connection *conn)
{
	int total_bytes_sent = 0;

	while (conn->send_pos < conn->send_len) {
		int bytes_sent = send(conn->sockfd, conn->send_buffer + conn->send_pos, conn->send_len - conn->send_pos, 0);

		if (bytes_sent <= 0)
			return -1;

		conn->send_pos += bytes_sent;
		total_bytes_sent += bytes_sent;
	}

	return total_bytes_sent;
}

int connection_send_dynamic(struct connection *conn)
{
	connection_complete_async_io(conn);

	if (connection_send_data(conn) < 0)
		return -1;

	conn->state = (conn->file_pos == conn->file_size) ? STATE_DATA_SENT : STATE_SENDING_DATA;
	return 0;
}

void handle_input(struct connection *conn)
{
	switch (conn->state) {
	case STATE_INITIAL:
		// attempt to receive data
		receive_data(conn);
		break;

	case STATE_SENDING_404:
		{
			ssize_t data_sent = connection_send_data(conn);

			if (data_sent < 0) // failure
				return;
			// mark as sent
			conn->state = STATE_404_SENT;
		}
		break;

	case STATE_ASYNC_ONGOING:
		{
			int result = connection_send_dynamic(conn);

			if (result < 0) {
				// If sending dynamic content fails, prepare a 404 response
				connection_prepare_send_404(conn);
				conn->state = STATE_SENDING_404;
			}
		}
		break;

	case STATE_404_SENT:
		// once sent clean the connection
		connection_remove(conn);
		break;

	default:
		// Handle any unexpected states by exiting the program with an error code
		fprintf(stderr, "Unexpected state encountered\n");
		exit(EXIT_FAILURE);
	}
}

void handle_output(struct connection *conn)
{
	switch (conn->state) {
	case STATE_REQUEST_RECEIVED:
		connection_prepare_send_reply_header(conn);
		conn->state = STATE_SENDING_HEADER;
		break;

	case STATE_SENDING_HEADER:
		connection_send_data(conn);
		conn->state = STATE_HEADER_SENT;
		break;

	case STATE_HEADER_SENT:
		conn->state = (conn->res_type == RESOURCE_TYPE_NONE)
						  ? STATE_SENDING_404
						  : STATE_SENDING_DATA;
		break;

	case STATE_SENDING_404:
		connection_send_data(conn);
		conn->state = STATE_404_SENT;
		break;

	case STATE_404_SENT:
		connection_remove(conn);
		break;

	case STATE_SENDING_DATA:
		if (conn->res_type == RESOURCE_TYPE_STATIC) {
			conn->state = connection_send_static(conn);
		} else if (conn->res_type == RESOURCE_TYPE_DYNAMIC) {
			conn->state = STATE_ASYNC_ONGOING;// it goes to state ongoing
		} else {
			// if resource type is invalid, treat is a 404
			connection_prepare_send_404(conn);
			conn->state = STATE_SENDING_404;
		}
		break;

	case STATE_ASYNC_ONGOING:
		connection_start_async_io(conn);
		break;

	case STATE_DATA_SENT:
		connection_remove(conn);
		break;

	default:
		ERR("Unexpected state: %d\n");
		exit(EXIT_FAILURE);
	}
}

void handle_client(uint32_t event, struct connection *conn)
{
	/* TODO: Handle new client. There can be input and output connections.
	 * Take care of what happened at the end of a connection.
	 */
}


int main(void)
{
	// async init
	int setup_result = io_setup(1, &ctx);

	if (setup_result != 0) {
		fprintf(stderr, "Error setting up asynchronous I/O: %d\n", setup_result);
		exit(EXIT_FAILURE);
	}

	// multiplexing init
	epollfd = w_epoll_create();
	if (epollfd < 0) {
		perror("Error creating epoll instance");
		exit(EXIT_FAILURE);
	}

	// creating server socket
	listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);
	if (listenfd < 0) {
		perror("Error creating server listener");
		exit(EXIT_FAILURE);
	}

	// adding the server to epoll object
	int add_fd_result = w_epoll_add_fd_in(epollfd, listenfd);

	if (add_fd_result < 0) {
		perror("Error adding server socket to epoll");
		exit(EXIT_FAILURE);
	}

	/* Server main loop */
	while (1) {
		struct epoll_event rev;

		/* TODO: Wait for events. */
		int wait_result = w_epoll_wait_infinite(epollfd, &rev);

		if (wait_result < 0) {
			perror("Error waiting for epoll events");
			exit(EXIT_FAILURE);
		}

		if (rev.data.fd == listenfd) {
			if (rev.events & EPOLLIN)
				handle_new_connection();
		} else {
			// handle client events
			if (rev.events & EPOLLIN) { //input
				handle_input((struct connection *) rev.data.ptr);
			} else if (rev.events & EPOLLOUT) { //output
				handle_output((struct connection *) rev.data.ptr);
			} else {
				// neither, then remove
				connection_remove((struct connection *) rev.data.ptr);
			}
		}
	}

	return 0;
}
