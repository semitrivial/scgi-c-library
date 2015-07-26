/*
 *  SCGI C Library
 *  By Sam Alexander
 *
 *  Version 0.2, Last Updated:  26 July 2015
 *
 *  On the web... http://www.xamuel.com/scgilib/
 *
 *  scgilib.h - SCGI C Library header file
 *
 *  Instructions:  #include this library file into whatever project you're working on.
 *                 Compile your project along with the accompanying scgilib.c file.
 *                 Use the library's functions to your heart's content.
 *
 *  Copyright/license:  MIT.
 */

#ifndef SCGILIB_H
#define SCGILIB_H

#include <unistd.h>
#include <fcntl.h>

typedef struct SCGI_PORT scgi_port;
typedef struct SCGI_HEADER scgi_header;
typedef struct SCGI_REQUEST scgi_request;
typedef struct SCGI_DESC scgi_desc;

#if !defined(FNDELAY)
#define FNDELAY O_NDELAY
#endif

/*
 * If a browser connects, but doesn't do anything, how long until kicking them off
 * (See also the next comment below)
 */
#define SCGI_KICK_IDLE_AFTER_X_SECS 60

/*
 * How many times, per second, will your main project be checking for new connections?
 * (Rather than keep track of the exact time a client is idle, rather we keep track of
 *  the number of times we've checked for updates and found none.  When the client has
 *  been idle for SCGI_KICK_IDLE_AFTER_X_SECS * SCGI_PULSE_PER_SEC consecutive checks,
 *  they will be booted.  SCGI_PULSES_PER_SEC is not used anywhere else.  Thus, it is
 *  not terribly important that it be completely precise, a ballpark estimate is good
 *  enough.
 */
#define SCGI_PULSES_PER_SEC 10

/*
 * Different states of a client.
 */
#define SCGI_SOCKSTATE_READING_REQUEST 0
#define SCGI_SOCKSTATE_WRITING_RESPONSE 1

/*
 * How many bytes of memory to initially allocate for I/O buffers when a client connects.
 * (These will automatically be grown when/if the client sends a bigger amount of input or
 * SCGI C Library responds with a bigger amount of output)
 */
#define SCGI_INITIAL_OUTBUF_SIZE 16384
#define SCGI_INITIAL_INBUF_SIZE 16384

/*
 * Upper limits on the number of bytes for I/O buffers.  If they send more data than this,
 * or compel us to send a bigger response, SCGI C Library assumes it is an attack and kills
 * the connection.
 */
#define SCGI_MAX_INBUF_SIZE 131072
#define SCGI_MAX_OUTBUF_SIZE 524288

/*
 * If multiple clients simultaneously attempt to connect, how many connections should SCGI C Library
 * accept at once?  Any additional simultaneous connections beyond this limit will have to wait
 * their turn.
 */
#define SCGI_LISTEN_BACKLOG_PER_PORT 32

/*
 * Different parts of the SCGI protocol
 */
typedef enum
{
  SCGI_PARSE_HEADLENGTH, SCGI_PARSE_HEADNAME, SCGI_PARSE_HEADVAL,
  SCGI_PARSE_BODY
} types_of_states_for_the_request_parser;

/*
 * Different HTTP request types
 * (right now the SCGI C Library is mainly only built to handle GET and HEAD)
 */
typedef enum
{
  SCGI_METHOD_UNSPECIFIED, SCGI_METHOD_UNKNOWN,
  SCGI_METHOD_GET, SCGI_METHOD_POST, SCGI_METHOD_HEAD
} types_of_methods_for_http_protocol;

/*
 * Macros for handling generic doubly-linked lists
 */
#define SCGI_LINK(link, first, last, next, prev) \
do                                          \
{                                           \
   if ( !(first) )                          \
   {                                        \
      (first) = (link);                     \
      (last) = (link);                      \
   }                                        \
   else                                     \
      (last)->next = (link);                \
   (link)->next = NULL;                     \
   if ((first) == (link))                   \
      (link)->prev = NULL;                  \
   else                                     \
      (link)->prev = (last);                \
   (last) = (link);                         \
} while(0)

#define SCGI_UNLINK(link, first, last, next, prev)   \
do                                              \
{                                               \
   if ( !(link)->prev )                         \
   {                                            \
      (first) = (link)->next;                   \
      if ((first))                              \
         (first)->prev = NULL;                  \
   }                                            \
   else                                         \
   {                                            \
      (link)->prev->next = (link)->next;        \
   }                                            \
   if ( !(link)->next )                         \
   {                                            \
      (last) = (link)->prev;                    \
      if((last))                                \
         (last)->next = NULL; \
   }                                            \
   else                                         \
   {                                            \
      (link)->next->prev = (link)->prev;        \
   }                                            \
} while (0)

/*
 * Data structure for a port -- SCGI C Library has support for listening on multiple ports simultaneously
 */
struct SCGI_PORT
{
  scgi_port *next;
  scgi_port *prev;
  scgi_desc *first_scgi_desc;	// first descriptor, i.e. connection (in a doubly-linked list)
  scgi_desc *last_scgi_desc;	// last descriptor, i.e. connection (in a doubly-linked list)
  int port;			// port number
  int sock;			// socket number for listening on this port
};

/*
 * Data structure for a header in the SCGI protocol
 */
struct SCGI_HEADER
{
  scgi_header *next;
  scgi_header *prev;
  char *name;			// name of the header
  char *value;			// value of the header
};

/*
 * Data structure for a successfully parsed SCGI request.
 * This is what any project using the SCGI C Library will primarily interact with.
 */
struct SCGI_REQUEST
{
  scgi_request *next;
  scgi_request *prev;
  scgi_request *next_unrecved;
  scgi_request *prev_unrecved;
  scgi_desc *descriptor;	// info about the connection
  scgi_header *first_header;	// doubly-linked list of request headers
  scgi_header *last_header;
  char *body;			// request body
  int scgi_content_length;	// length of the request body
  char scgi_scgiheader;		// whether or not the request included the "SCGI" header
  int *dead;			// pointer to an int which SCGI C Library can use to specify whether a connection is dead (see documentation for details)
  int request_method;		// type of request (SCGI_METHOD_GET, SCGI_METHOD_POST, SCGI_METHOD_HEAD, or SCGI_METHOD_UNKNOWN)
  char *http_host;		// which host name are they connecting to (in principle, with this, you can have one program serve multiple domain names)
  /*
   * The remaining fields are some individual headers that might be sent
   */
  char *query_string;
  char *request_uri;
  char *http_cache_control;
  char *raw_http_cookie;
  char *http_connection;
  char *http_accept_encoding;
  char *http_accept_language;
  char *http_accept_charset;
  char *http_accept;
  char *user_agent;
  char *remote_addr;		// Client's IP address
  char *server_port;
  char *server_addr;
  char *server_protocol;
};

/*
 * Info about a connection
 */
struct SCGI_DESC
{
  scgi_desc *next;
  scgi_desc *prev;
  scgi_port *port;		//which port are they connected to
  scgi_request *req;		//info about the request they are sending
  int sock;			//which socket they're bound to
  char *buf;			//input buffer for the data they're sending us
  int bufsize;			//how much space we've allocated so far for the data they're sending us
  int buflen;			//how much data they've sent us so far
  char *outbuf;			//output buffer for data we're going to send them
  int outbufsize;		//how much space we've allocated for outbuf so far
  int outbuflen;		//how long outbuf has become so far
  int idle;			//how many times we checked the connection for new data and found it idle
  int state;			//which state is this connection in
  char *writehead;		//pointer to the end of the data currently stored in outbuf
  /*
   * The remaining fields are technical fields used by the parser
   */
  int parsed_chars;
  char *string_starts;
  int true_header_length;
  int true_request_length;
  int parser_state;
};

/*
 * Global variables from scgilib.c
 */
extern scgi_port *first_scgi_port;	// doubly-linked list of ports for SCGI C Library to listen on
extern scgi_port *last_scgi_port;

extern scgi_request *first_scgi_req;	// doubly-linked list of SCGI requests awaiting response
extern scgi_request *last_scgi_req;

extern fd_set scgi_inset;		// socket programming stuff
extern fd_set scgi_outset;
extern fd_set scgi_excset;

/*
 * Function prototypes for functions from scgilib.c
 */
void scgi_update_connections_port( scgi_port *p );
void scgi_update_connections( void );
void scgi_kill_socket( scgi_desc *d );
void free_scgi_request( scgi_request *r );
void scgi_flush_response( scgi_desc *d );
void scgi_listen_to_request( scgi_desc *d );
void scgi_answer_the_phone( scgi_port *p );
void scgi_perror( char *txt );
int scgi_initialize(int port);
int scgi_send( scgi_request *req, char *txt, int len );
int scgi_write( scgi_request *req, char *txt );
scgi_request *scgi_recv( void );

/*
 * Memory allocation macro
 */
#define SCGI_CREATE(result, type, number)				\
do									\
{									\
   if (!((result) = (type *) calloc ((number), sizeof(type))))		\
   {									\
      fprintf(stderr, "scgilib: Out of RAM! Emergency shutdown.\n" );	\
      abort();								\
   }									\
} while(0)

#endif //ends the "#ifdef SCGILIB_H" from the beginning of the file
