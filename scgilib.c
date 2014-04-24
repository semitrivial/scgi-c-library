/*
 *  SCGI C Library
 *  By Sam Alexander
 *
 *  Version 0.2, Last Updated:  4 Jun 2012
 *
 *  On the web... http://www.xamuel.com/scgilib
 *
 *  scgilib.c - SCGI Library code file
 *
 *  Instructions:  Compile scgilib.c along with all your other code files.
 *                 #include the accompanying header file "scgilib.h" anywhere you wish
 *                 to use the SCGI Library.
 *                 Use the library's functions to your heart's content.
 *
 *  Copyright/license:  This code is released into the public domain.
 */

#include "scgilib.h"
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * Doubly-linked list of ports to listen on
 */
scgi_port *first_scgi_port;
scgi_port *last_scgi_port;

/*
 * Doubly-linked list of requests from clients
 */
scgi_request *first_scgi_req;
scgi_request *last_scgi_req;

/*
 * Doubly-linked list of new requests which have been parsed and are ready to be returned by scgi_recv
 */
scgi_request *first_scgi_unrecved_req;
scgi_request *last_scgi_unrecved_req;

/*
 * Socket programming stuff
 */
fd_set scgi_inset;
fd_set scgi_outset;
fd_set scgi_excset;

/*
 * Function prototypes (there are additional function prototypes in scgilib.h)
 */
int resize_buffer( scgi_desc *d, char **buf );
void scgi_parse_input( scgi_desc *d );
void scgi_deal_with_socket_out_of_ram( scgi_desc *d );
int scgi_is_number( char *arg );
int scgi_add_header( scgi_desc *d, char *name, char *val );

/*
 * Listen for incoming requests on all open ports
 */
void scgi_update_connections( void )
{
  scgi_port *p;

  for ( p = first_scgi_port; p; p = p->next )
    scgi_update_connections_port( p );
}

/*
 * Listen for incoming requests on one specified port
 */
void scgi_update_connections_port( scgi_port *p )
{
  static struct timeval zero_time;
  scgi_desc *d, *d_next;
  int top_desc;

  /*
   * initialize socket stuff
   */
  FD_ZERO( &scgi_inset );
  FD_ZERO( &scgi_outset );
  FD_ZERO( &scgi_excset );
  FD_SET( p->sock, &scgi_inset );
  top_desc = p->sock;

  for ( d = p->first_scgi_desc; d; d = d->next )
  {
    if ( d->sock > top_desc )
      top_desc = d->sock;
    if ( d->state == SCGI_SOCKSTATE_READING_REQUEST )
      FD_SET( d->sock, &scgi_inset );
    else
    if ( d->state == SCGI_SOCKSTATE_WRITING_RESPONSE )
      FD_SET( d->sock, &scgi_outset );
    FD_SET( d->sock, &scgi_excset );
  }

  /*
   * Poll the sockets!
   */
  if ( select( top_desc+1, &scgi_inset, &scgi_outset, &scgi_excset, &zero_time ) < 0 )
  {
    scgi_perror( "Fatal: scgilib failed to poll the descriptors." );
    exit(1);
  }

  /*
   * If we've got a new incoming connection, deal with it
   */
  if ( !FD_ISSET( p->sock, &scgi_excset ) && FD_ISSET( p->sock, &scgi_inset ) )
  {
    scgi_answer_the_phone(p);
  }

  for ( d = p->first_scgi_desc; d; d = d_next )
  {
    /*
     * We may be killing things in this list as we traverse it,
     * so need a safe copy of the next thing in the list.
     */
    d_next = d->next;

    d->idle++;

    /*
     * Kick connections out if they raise any kind of exception, or if they're idle too long
     */
    if ( FD_ISSET( d->sock, &scgi_excset )
    ||   d->idle > SCGI_KICK_IDLE_AFTER_X_SECS * SCGI_PULSES_PER_SEC )
    {
      FD_CLR( d->sock, &scgi_inset );
      FD_CLR( d->sock, &scgi_outset );

      scgi_kill_socket( d );
      continue;
    }

    /*
     * Handle remote I/O, provided the connections are ready for it
     */
    if ( d->state == SCGI_SOCKSTATE_READING_REQUEST
    &&   FD_ISSET( d->sock, &scgi_inset ) )
    {
      d->idle = 0;
      scgi_listen_to_request( d );
    }
    else
    if ( d->state == SCGI_SOCKSTATE_WRITING_RESPONSE
    &&   d->outbuflen > 0
    &&   FD_ISSET( d->sock, &scgi_outset ) )
    {
      d->idle = 0;
      scgi_flush_response( d );
    }
  }
}

/*
 * Kick a connection offline and delete it from memory
 */
void scgi_kill_socket( scgi_desc *d )
{
  SCGI_UNLINK( d, d->port->first_scgi_desc, d->port->last_scgi_desc, next, prev );

  free( d->buf );

  free( d->outbuf );

  free_scgi_request( d->req );
  close( d->sock );
  free( d );
}

/*
 * Delete an SCGI request from memory
 */
void free_scgi_request( scgi_request *r )
{
  scgi_header *h, *h_next;
  scgi_request *ptr;

  if ( !r )
    return;

  /*
   * The request is now dead.  If the programmer (you) supplied the location of an integer,
   * we will use it to signal the request's deadness, so you can avoid trying to do anything
   * with the no-longer-existent connection.
   */
  if ( r->dead )
  {
    *r->dead = 1;
  }

  SCGI_UNLINK( r, first_scgi_req, last_scgi_req, next, prev );

  for ( ptr = first_scgi_unrecved_req; ptr; ptr = ptr->next_unrecved )
  {
    if ( ptr == r )
    {
      SCGI_UNLINK( r, first_scgi_unrecved_req, last_scgi_unrecved_req, next_unrecved, prev_unrecved );
      break;
    }
  }

  for ( h = r->first_header; h; h = h_next )
  {
    h_next = h->next;
    free( h->name );
    free( h->value );
    free( h );
  }

  if ( r->body )
    free( r->body );

  free( r );
}

/*
 * Accept new connections
 */
void scgi_answer_the_phone( scgi_port *p )
{
  struct sockaddr_storage their_addr;
  socklen_t addr_size = sizeof(their_addr);
  int caller;
  scgi_desc *d;
  scgi_request *req;

  if ( ( caller = accept( p->sock, (struct sockaddr *) &their_addr, &addr_size) ) < 0 )
  {
    scgi_perror( "Warning: scgilib's phone rang but something prevented scgilib from answering it." );
    return;
  }

  /*
   * SCGI is intended for applications which accept multiple connections asynchronously, so
   * if a socket cannot be set to non-blocking for some reason, it gets the boot.
   */
  if ( ( fcntl( caller, F_SETFL, FNDELAY ) ) == -1 )
  {
    scgi_perror( "Warning: scgilib was unable to set a socket to non-blocking mode.  scgilib hung up the phone on this socket." );
    close(caller);
    return;
  }

  /*
   * The connection has been made.  Let's commit it to RAM.
   */
  SCGI_CREATE( d, scgi_desc, 1 );
  d->next = NULL;
  d->prev = NULL;
  d->port = p;
  d->sock = caller;
  d->idle = 0;
  d->state = SCGI_SOCKSTATE_READING_REQUEST;
  d->writehead = NULL;
  d->parsed_chars = 0;
  d->string_starts = NULL;
  d->parser_state = SCGI_PARSE_HEADLENGTH;

  SCGI_CREATE( d->buf, char, SCGI_INITIAL_INBUF_SIZE + 1 );
  d->bufsize = SCGI_INITIAL_INBUF_SIZE;
  d->buflen = 0;
  *d->buf = '\0';

  SCGI_CREATE( d->outbuf, char, SCGI_INITIAL_OUTBUF_SIZE + 1 );
  d->outbufsize = SCGI_INITIAL_OUTBUF_SIZE;
  d->outbuflen = 0;
  *d->outbuf = '\0';

  SCGI_CREATE( req, scgi_request, 1 );
  req->next = NULL;
  req->prev = NULL;
  req->next_unrecved = NULL;
  req->prev_unrecved = NULL;
  req->descriptor = d;

  req->first_header = NULL;
  req->last_header = NULL;
  req->body = NULL;
  req->scgi_content_length = -1;
  req->scgi_scgiheader = 0;
  req->dead = NULL;

  req->request_method = SCGI_METHOD_UNSPECIFIED;
  req->http_host = NULL;
  req->query_string = NULL;
  req->request_uri = NULL;
  req->http_cache_control = NULL;
  req->raw_http_cookie = NULL;
  req->http_connection = NULL;
  req->http_accept_encoding = NULL;
  req->http_accept_language = NULL;
  req->http_accept_charset = NULL;
  req->http_accept = NULL;
  req->user_agent = NULL;
  req->remote_addr = NULL;
  req->server_port = NULL;
  req->server_addr = NULL;
  req->server_protocol = NULL;

  d->req = req;

  SCGI_LINK( req, first_scgi_req, last_scgi_req, next, prev );

  SCGI_LINK( d, p->first_scgi_desc, p->last_scgi_desc, next, prev );

  return;
}

/*
 * If more I/O space is needed than allocated, allocate more (up to a limit)
 * Returns 0 (and kills the connection) if the specified limit has been reached
 */
int resize_buffer( scgi_desc *d, char **buf )
{
  int max, *size;
  char *tmp;

  if ( *buf == d->buf )
  {
    max = SCGI_MAX_INBUF_SIZE;
    size = &d->bufsize;
  }
  else
  {
    max = SCGI_MAX_OUTBUF_SIZE;
    size = &d->outbufsize;
  }

  *size *= 2;
  if ( *size >= max )
  {
    scgi_kill_socket(d);
    return 0;
  }

  /*
   * Special treatment rather than the usual malloc macro, just because I thought this
   * particular function might have a bigger risk of sucking up too much RAM and so it
   * would be better to handle it directly rather than use a generic macro
   */
  tmp = (char *) calloc((*size)+1, sizeof(char) );
  if ( !tmp )
  {
    scgi_deal_with_socket_out_of_ram(d);
    return 0;
  }

  sprintf( tmp, "%s", *buf );
  free( *buf );
  *buf = tmp;
  return 1;
}

/*
 * A socket is ready for us to read (continue reading?) its input!  So read it.
 */
void scgi_listen_to_request( scgi_desc *d )
{
  int start = d->buflen, readsize;

  /*
   * If their buffer is sufficiently near full and there's still more to be read,
   * then increase the buffer.  If they're spamming with an enormous request,
   * the connection will be terminated in resize_buffer.
   */
  if ( start >= d->bufsize - 5 )
  {
    if ( !resize_buffer( d, &d->buf ) )
      return;
  }

  /*
   * Read as much as we can.  Can't wait around, since there may be other connections to attend to,
   * so just read as much as possible and make a note of how much that was (the socket is non-blocking
   * so this won't cause us to hang even if the incoming message would otherwise take time to recv)
   */
  readsize = recv( d->sock, d->buf + start, d->bufsize - 5 - start, 0 );

  /*
   * There's new input, successfully read and stored in memory!  Let's parse it and figure out what
   * the heck they're asking for!  (Who knows whether we've got their full transmission or whether
   * there's still more in the pipeline-- we'll let the parser figure that out based on the SCGI
   * protocol)
   */
  if ( readsize > 0 )
  {
    d->buflen += readsize;
    scgi_parse_input( d );
    return;
  }

  /*
   * Something unexpected happened.  This is the wild untamed internet, so kill the connection first and
   * ask questions later.
   */
  if ( readsize == 0 || errno != EWOULDBLOCK )
  {
    scgi_kill_socket( d );
    return;
  }
}

/*
 * We've got a response ready for a connection, and the connection is ready to receive it.
 * Transmit!
 */
void scgi_flush_response( scgi_desc *d )
{
  int sent_amount;

  if ( !d->writehead )
    d->writehead = d->outbuf;

  /*
   * Don't take too long transmitting, since other connections may be waiting.
   * Send as much as we can right now, and if there's more left, send the rest next time.
   */
  sent_amount = send(d->sock, d->writehead, d->outbuflen, 0 );

  /*
   * Transmission complete... Sayonara.
   */
  if ( sent_amount >= d->outbuflen )
  {
    scgi_kill_socket( d );
    return;
  }

  /*
   * Transmission incomplete.  Make a note of where we left off, we'll send the rest next time.
   */
  d->outbuflen -= sent_amount;
  d->writehead = &d->writehead[sent_amount];

  return;
}

void scgi_perror( char *txt )
{
  fprintf( stderr, "%s\n", txt );
  return;
}

/*
 * Parse input according to the SCGI protocol.
 * Due to the asynchronous nature of SCGI, we may or may not have received
 * the full input (it might be that more is still on its way), and there's no
 * way to tell without parsing, so the parser must be capable of stopping,
 * remembering where it left off, and indicating as much (which it does via
 * states in the descriptor structure).
 */
void scgi_parse_input( scgi_desc *d )
{
  char *parser = &d->buf[d->parsed_chars], *end, *headername, *headerval;
  int len, total_req_length, headernamelen;

  /*
   * Everything has already been parsed, so do nothing until new input arrives.
   */
  if ( d->parsed_chars == d->buflen )
    return;

  /*
   * If they are not following the SCGI protocol, we have no choice but to hang up on them.
   * The very first character must not be 0 or : or it would be an invalid netstring
   * (well, technically it could be the empty netstring, but that's invalid SCGI as well,
   * if it's at the very start of the transmission)
   */
  if ( d->parsed_chars == 0 && (*d->buf == '0' || *d->buf == ':') )
  {
    scgi_kill_socket(d);
    return;
  }

  end = &d->buf[d->buflen];


scgi_parse_input_label:

  /*
   * How to proceed depends where we left off last time (if ever) we were parsing this input.
   */
  switch( d->parser_state )
  {
    case SCGI_PARSE_HEADLENGTH:   // Oh yeah, we were in the middle of reading the length of their headers.  (This is the default state)
      while ( parser < end )
      {
        d->parsed_chars++;

        /*
         * The end of the header length is indicated by :, we've successfully read the header's length.
         */
        if ( *parser == ':' )
        {
          d->parser_state = SCGI_PARSE_HEADNAME; // the next task is to read the first header's name.
          /*
           * Replace the colon with an end-of-string so we can use strtoul to read the number.
           */
          *parser = '\0';
          d->true_header_length = strtoul(d->buf,NULL,10) + strlen(d->buf) + 2;
          *parser = ':'; // undo the end-of-string change we made above
          parser++;
          d->string_starts = parser;
          goto scgi_parse_input_label;
        }
        if ( *parser < '0' || *parser > '9' )
        {
          /*
           * If they're trying to indicate a non-number length, they're making a mockery of the SCGI protocol,
           * kick them right out.
           */
          scgi_kill_socket(d);
          return;
        }
        parser++;
      }
      break;

    case SCGI_PARSE_HEADNAME: // Oh yeah, we were in the middle of reading a header's name.
      while ( parser < end )
      {
        d->parsed_chars++;

        if ( d->parsed_chars == d->true_header_length )
        {
          /*
           * If we're supposedly at the end of the headers (based on the length they transmitted),
           * but the headers don't end with "\0,", then it's invalid SCGI.  Been nice knowing you...
           */
          if ( *parser != ',' || parser[-1] != '\0' )
          {
            scgi_kill_socket(d);
            return;
          }

          /*
           * They didn't send an "SCGI" header with value 1.
           * Are they using some different protocol?  Whatever, not our problem.  Door's that way.
           */
          if ( !d->req->scgi_scgiheader )
          {
            scgi_kill_socket(d);
            return;
          }

          /*
           * If their headers indicated that no body is coming, then we're done.
           * Put the parsed request in the list of requests which have been parsed but not yet
           * communicated to you (the programmer of whatever program is including scgilib).
           */
          if ( d->req->scgi_content_length == 0 )
          {
            SCGI_CREATE( d->req->body, char, 2 );

            *d->req->body = '\0';

            SCGI_LINK( d->req, first_scgi_unrecved_req, last_scgi_unrecved_req, next_unrecved, prev_unrecved );
            return;
          }
          len = strtoul(d->req->first_header->value,NULL,10);
          d->true_request_length = len + d->true_header_length;
          parser++;
          d->string_starts = parser;

          /*
           * Next task is to start reading the body after the headers
           */
          d->parser_state = SCGI_PARSE_BODY;

          goto scgi_parse_input_label;
        }

        /*
         * A '\0' indicates the end of the header's name.
         */
        if ( *parser == '\0' )
        {
          /*
           * Of course, a header with the empty string as its name is forbidden and no such
           * nonsense will be tolerated.
           */
          if ( parser == d->string_starts )
          {
            scgi_kill_socket(d);
            return;
          }

          /*
           * Having a header's name, our next task is to parse its value.
           */
          d->parser_state = SCGI_PARSE_HEADVAL;
          parser++;
          goto scgi_parse_input_label;
        }
        parser++;
      }
      break;

    case SCGI_PARSE_HEADVAL:
      while ( parser < end )
      {
        d->parsed_chars++;

        /*
         * We expected a header value, and instead we reached the end of the headers (according to
         * the header length they specified)?!  Nope.jpg
         */
        if ( d->parsed_chars == d->true_header_length )
        {
          scgi_kill_socket(d);
          return;
        }

        /*
         * We've successfully read the value of the current header.
         * Create a structure for this header and store it.
         */
        if ( *parser == '\0' )
        {
          headernamelen = strlen(d->string_starts);
          SCGI_CREATE( headername, char, headernamelen+1 );
          sprintf( headername, "%s", d->string_starts );
          SCGI_CREATE( headerval, char, strlen(&d->string_starts[headernamelen+1])+1 );
          sprintf( headerval, "%s", &d->string_starts[headernamelen+1] );
          if ( !scgi_add_header( d, headername, headerval ) )
            return;
          /*
           * Next task: parse the next header's name.
           */
          d->parser_state = SCGI_PARSE_HEADNAME;
          parser++;
          d->string_starts = parser;
          goto scgi_parse_input_label;
        }
        parser++;
      }
      break;

    case SCGI_PARSE_BODY:
      total_req_length = d->true_header_length + d->req->scgi_content_length;

      while ( parser < end )
      {
        d->parsed_chars++;

        if ( d->parsed_chars == total_req_length )
        {
          parser[1] = '\0';
          SCGI_CREATE( d->req->body, char, strlen(d->string_starts)+1 );
          sprintf( d->req->body, "%s", d->string_starts );
          SCGI_LINK( d->req, first_scgi_unrecved_req, last_scgi_unrecved_req, next_unrecved, prev_unrecved );

          return;
        }
        parser++;
      }
      break;
  }

  return;
}

/*
 * Macro to save finger leather in the following function
 * (repeatedly checking whether a header's name matches "match" and if so, storing its value in "address")
 */
#define SCGIKEY(match,address) else if (!strcmp(name,match)) do {d->req->address = val;} while(0)

/*
 * Having read a header's name and value, attempt to make a record of it.
 * If something violates the SCGI protocol, kill the connection and return 0.
 */
int scgi_add_header( scgi_desc *d, char *name, char *val )
{
  scgi_header *h;

  /*
   * First header is required to be CONTENT_LENGTH and have a nonnegative numeric value.
   */
  if ( !d->req->first_header )
  {
    if ( strcmp( name, "CONTENT_LENGTH" )
    ||  !scgi_is_number( val ) )
    {
      free( name );
      free( val );
      scgi_kill_socket(d);
      return 0;
    }

    d->req->scgi_content_length = strtoul(val,NULL,10);

    if ( d->req->scgi_content_length < 0 )
    {
      free( name );
      free( val );
      scgi_kill_socket(d);
      return 0;
    }
  }

  SCGI_CREATE( h, scgi_header, 1 );
  h->name = name;
  h->value = val;

  SCGI_LINK( h, d->req->first_header, d->req->last_header, next, prev );

  /*
   * Certain headers' values have space allocated especially for them in the request structure...
   */

  if ( !strcmp( name, "SCGI" ) && !strcmp( val, "1" ) )
    d->req->scgi_scgiheader = 1;

  if ( !strcmp(name, "HTTP_COOKIE") )
  {
    d->req->raw_http_cookie = val;
  }
  else if ( !strcmp(name, "REQUEST_METHOD" ) )
  {
    if ( !strcmp(val,"GET") ) d->req->request_method = SCGI_METHOD_GET;
    else if ( !strcmp(val,"POST") ) d->req->request_method = SCGI_METHOD_POST;
    else if ( !strcmp(val,"HEAD") ) d->req->request_method = SCGI_METHOD_HEAD;
    else d->req->request_method = SCGI_METHOD_UNKNOWN;
  }
  SCGIKEY("HTTP_CONNECTION", http_connection);
  SCGIKEY("HTTP_CACHE_CONTROL", http_cache_control);
  SCGIKEY("HTTP_ACCEPT_CHARSET", http_accept_charset);
  SCGIKEY("HTTP_ACCEPT_ENCODING", http_accept_encoding);
  SCGIKEY("HTTP_ACCEPT_LANGUAGE", http_accept_language);
  SCGIKEY("HTTP_ACCEPT", http_accept );
  SCGIKEY("HTTP_USER_AGENT", user_agent );
  SCGIKEY("USER_AGENT", user_agent );
  SCGIKEY("HTTP_HOST", http_host );
  SCGIKEY("QUERY_STRING", query_string );
  SCGIKEY("REQUEST_URI", request_uri );
  SCGIKEY("REMOTE_ADDR", remote_addr );
  SCGIKEY("SERVER_ADDR", server_addr );
  SCGIKEY("SERVER_PORT", server_port );
  SCGIKEY("SERVER_PROTOCOL", server_protocol );

  return 1;
}

/*
 * Function to check whether a string is a number
 */
int scgi_is_number( char *arg )
{
    int first = 1;
    if ( *arg == '\0' )
        return 0;

    for ( ; *arg != '\0'; arg++ )
    {
        if ( first && *arg == '-')
        {
                first = 0;
                continue;
        }
        if ( !isdigit(*arg) )
            return 0;
        first = 0;
    }

    return 1;
}

void scgi_deal_with_socket_out_of_ram( scgi_desc *d )
{
  scgi_kill_socket( d );
}

/*
 * Function to initialize the SCGI C Library (and start it listening on the specified port).
 * Returns 0 on failure.
 * May be called multiple times with different port numbers, if you want a single program
 * listening on different ports.
 *
 * This is one of the functions which you (the programmer making use of the SCGI C Library)
 * are likely to use in practice.
 */
int scgi_initialize(int port)
{
  scgi_port *p;
  int status, sock;
  struct addrinfo hints, *servinfo;
  char portstr[128];

  /*
   * Socket stuff
   */
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  sprintf( portstr, "%d", port );

  if ((status=getaddrinfo(NULL,portstr,&hints,&servinfo)) != 0)
  {
    return 0;
  }

  sock = socket( servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol );

  if ( sock == -1 )
    return 0;

  if ( bind(sock, servinfo->ai_addr, servinfo->ai_addrlen) == -1
  ||   listen(sock, SCGI_LISTEN_BACKLOG_PER_PORT) == -1 )
  {
    close(sock);
    return 0;
  }

  /*
   * At this point, SCGI C Library has successfully opened its ears to listen on the specified port.
   * Commit the port to memory.
   */

  SCGI_CREATE( p, scgi_port, 1 );
  p->next = NULL;
  p->prev = NULL;
  p->first_scgi_desc = NULL;
  p->last_scgi_desc = NULL;
  p->port = port;
  p->sock = sock;

  SCGI_LINK(p, first_scgi_port, last_scgi_port, next, prev );

  return 1;
}

/*
 * If any pending requests have successfully been parsed and are waiting to be served by your program,
 * this function will return a pointer to one of them.  A NULL return value indicates there are no such
 * requests.
 *
 * This is one of the functions which you (the programmer making use of the SCGI C Library)
 * are likely to use in practice.
 */
scgi_request *scgi_recv( void )
{
  scgi_request *req;

  if ( !first_scgi_unrecved_req )
  {
    scgi_update_connections();

    if ( !first_scgi_unrecved_req )
      return NULL;
  }

  req = first_scgi_unrecved_req;

  /*
   * After scgi_recv returns the pointer to the request, it is up to you (the programmer using SCGI Library)
   * to do something with it-- in most cases by sending a response.
   */
  req->descriptor->state = SCGI_SOCKSTATE_WRITING_RESPONSE;

  SCGI_UNLINK( req, first_scgi_unrecved_req, last_scgi_unrecved_req, next_unrecved, prev_unrecved );

  return req;
}

/*
 * Send a response to a request, without explicitly specifying the response's length.
 * NOTE: scgi_write should only be called once per request. Once it has been called, every time
 * the SCGI Library updates it will send as much of the response as it can, until the whole
 * response has been sent, and at that time, the request will be free'd.
 *
 * Returns 0 in case of failure due to inability to allocate RAM.
 *
 * This is one of the functions which you (the programmer making use of the SCGI C Library)
 * are likely to use in practice.
 */
int scgi_write( scgi_request *req, char *txt )
{
  int len = strlen(txt);

  return scgi_send( req, txt, len );
}

/*
 * Send a response to a request, explicitly specifying the response's length.
 * NOTE: scgi_send should only be called once per request. Once it has been called, every time
 * the SCGI Library updates it will send as much of the response as it can, until the whole
 * response has been sent, and at that time, the request will be free'd.
 *
 * Returns 0 in case of failure due to inability to allocate RAM.
 *
 * This is one of the functions which you (the programmer making use of the SCGI C Library)
 * are likely to use in practice.
 */
int scgi_send( scgi_request *req, char *txt, int len )
{
  scgi_desc *d = req->descriptor;

  /*
   * If more is being sent than we've allocated space for, then allocate more space
   */
  if ( len >= d->outbufsize - 5 )
  {
    char *newbuf = calloc( len + 6, sizeof(char) );

    if ( !newbuf )
      return 0;
    memcpy( newbuf, txt, len );
    free( d->outbuf );
    d->outbuf = newbuf;
    d->outbuflen = len;
    d->outbufsize = len + 6;
    return 1;
  }

  memcpy( d->outbuf, txt, len );
  d->outbuflen = len;

  /*
   * The actual physical transmission will be handled by the scgi_flush_response function,
   * once the socket is ready to receive it.
   */

  return 1;
}


void scgi_302_redirect( scgi_request *req, char *address )
{
  char buf[256], *b = buf;
  int len;

  len = 100 + strlen(address);

  if ( len >= 250 )
  {
    SCGI_CREATE( b, char, len + 1 );
  }

  sprintf( b, "Status: 302 Found\n\rLocation: %s\n\rContent-Length: 0\n\r\n\r", address );
  scgi_send( req, b, len );

  if ( b != buf )
    free( b );
}
