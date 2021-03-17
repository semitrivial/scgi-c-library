/*
 *  SCGI C Library
 *  By Sam Alexander
 *
 *  Version 0.2, Last Updated:  01 Mar 2014
 *
 *  helloworld.c - SCGI Library example file
 *                 Creates a server (on port 8000) to listen for SCGI and respond with Hello World!
 *
 *  To actually make this work, one must configure one's webserver to redirect (certain) traffic to
 *  port 8000 using the SCGI protocol.  How to do this varies from webserver to webserver.
 *  Of course, 8000 can be replaced with whatever port number you want.
 *
 *  For example, on my Apache server, first I installed the mod-scgi module, then I added
 *  the following line in etc/apache2/apache2.conf :
 *    SCGIMount /scgilib/helloworld/ 127.0.0.1:8000
 *  Then I restarted Apache.
 *  This tells Apache that when anyone requests /scgilib/helloworld/, Apache is to translate the request
 *  into the SCGI protocol, forward it to port 8000, obtain a response, and forward the response back to
 *  whoever made the original request.
 *
 *  Copyright/license:  MIT
 */

#include "scgilib.h"
#include <stdio.h>

#define HELLOWORLDPORT 8000

/*
 * nginx does not correctly implement the SCGI protocol.
 * After much hair-pulling, I discovered that as of version
 * 1.1.19, nginx will accept SCGI responses if they send
 * the header "HTTP/1.1 200 OK" and no other headers.
 *
 * To make helloworld.c work with Apache, comment out the following line.
 * To make it work with nginx, leave the following line defined.
 *
 */

//#define SUPPORT_FOR_BUGGY_NGINX


int main(void)
{
  int connections;

  /*
   * Attempt to initialize the SCGI Library and make it listen on a port
   */

  if ( scgi_initialize( HELLOWORLDPORT ) )
    printf( "Successfully initialized the SCGI library.  Listening on port %d.\n", HELLOWORLDPORT );
  else
  {
    printf(	"Could not listen for incoming connections on port %d.\n"
		"Aborting helloworld.\n\n", HELLOWORLDPORT );
    return 0;
  }

  /*
   * Enter an infinite loop, serving up responses to SCGI connections forever.
   */
  while ( 1 )
  {
    /*
     * Check for connections ten times per second (once per 100000 microseconds).
     * A typical server (such as this helloworld server) will spend the vast majority of its time sleeping.
     * Nothing magical about 100000 microseconds, an SCGI Library user can call the library as often or rarely
     * as desired (of course, if you wait foreeeever, eventually your webserver will issue an "Internal Server Error").
     */
    usleep(100000);

#define MAX_CONNECTIONS_TO_ACCEPT_AT_ONCE 5

    connections = 0;

    while ( connections < MAX_CONNECTIONS_TO_ACCEPT_AT_ONCE )
    {
      /*
       * If any connections are awaiting the server's attention, scgi_recv() will output a pointer to one of them.
       * Otherwise, it will return NULL.
       */
      scgi_request *req = scgi_recv();
      int dead;

      if ( req != NULL )
      {
        /*
         * Got a connection!
         */
        connections++;

        /*
         * Since there is no way to check whether memory has been free'd, let's give the library a way to
         * let us know whether the request still exists in memory or not, by giving it the address of an
         * int.  Once we've done this, we can check the int at any time, to see whether the request still
         * exists in memory.
         */
        dead = 0;
        req->dead = &dead;

        /*
         * Send some log messages to stdout (pretty silly, but this is to illustrate how scgilib works)
         */

        printf( "SCGI C Library received an SCGI connection on port %d.\n", req->descriptor->port->port );
        if ( req->remote_addr )
          printf( "The connection originated from remote IP address %s.\n", req->remote_addr );
        if ( req->http_host )
          printf( "The connection was addressed to domain name %s.\n", req->http_host );
        if ( req->request_method == SCGI_METHOD_GET )
          printf( "The connection made an HTTP GET request.\n" );
        else if ( req->request_method == SCGI_METHOD_POST )
          printf( "The connection made an HTTP POST request.\n" );
        else if ( req->request_method == SCGI_METHOD_HEAD )
          printf( "The connection made an HTTP HEAD request.\n" );
        else
          printf( "The connection made some other HTTP request than GET, POST, or HEAD.\n" );
        if ( req->user_agent )
          printf( "The webclient identified itself as: %s\n", req->user_agent );
        if ( req->query_string && *req->query_string )
          printf( "They included a query string: %s\n", req->query_string );

#ifndef SUPPORT_FOR_BUGGY_NGINX
        if ( !scgi_write( req,  "Status: 200 OK\r\n"
                                "Content-Type: text/plain\r\n\r\n"
                                "Hello World!" ) )
#else
        if ( !scgi_write( req,  "HTTP/1.1 200 OK\r\n\r\n"
                                "Hello World!" ) )
#endif
        {
          printf( "Our response could not be sent, we couldn't allocate the necessary RAM.\n" );
        }
        else
          if ( dead == 1 )
            printf(	"Oh my, something went wrong!\n"
			"The connection was killed by the SCGI Library when we tried to send the response.\n" );

        /*
         * From here on, helloworld.c forgets about the request (though the library itself still remembers it)
         * so we can relieve the library from having to maintain the req->dead
         */
        if ( !dead )
          req->dead = NULL;
        printf("\n");
      }
      else
        break;
    }
  }
}
