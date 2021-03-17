The library consists of just two files: scgilib.h and helloworld.c, which implements a bare-bones "Hello World" server using the library.

# Features

    Asynchronous, non-blocking sockets. All the necessary socket programming is taken care of in scgilib.c. If one client connects to the library and slooooooowly starts sending a request, and while that request is still trickling in, a second client connects and sends a second request, the library will handle both requests simultaneously, without making the second client wait. This is accomplished without forking the server into multiple processes (thus allowing the server to store an enormous and dynamic database in RAM).
    Listening for connections on multiple ports is as easy as calling the library initialization function multiple times.
    The library files are generously full of comments, I hope this will facilitate easily modifying the libraries as needed.

# Documentation

There are three primary functions for interacting with the library: scgi_initialize for turning the server on, scgi_recv for obtaining connections, and scgi_write for sending the response. Connections are returned by scgi_recv in the form of an scgi_request structure (defined in scgilib.h) which contains fields for common things you might want to know about the request, such as what query string they sent, what their IP address is, etc.
scgi_initialize

## int scgi_initialize( int port );

Attempt to start an SCGI server which listens for connections to the specified port.

Returns 1 on success, 0 on failure.

Can be called multiple times with different port numbers, which will cause the library to listen on each port. (This feature hasn’t been very rigorously tested)
scgi_recv

## scgi_request *scgi_recv( void );

Returns a pointer to a structure containing data about an incoming request. The structure, struct SCGI_REQUEST, is defined in scgilib.h. If there are multiple connections awaiting a response, scgi_recv will send the one which has been ready the longest. If there are no new connections awaiting response, scgi_recv returns NULL.

To read the info about the request (e.g., what query string they sent), simply read from the appropriate fields of the structure. See helloworld.c for an example.

Garbage collection is handled in scgilib.c: the structures returned by scgi_recv are NOT meant to be manually freed. They will automatically be freed shortly after you specify an HTTP response using scgi_write (you ARE sending responses to each request, right? Even if the request is nonsense, you should at least send a 404 File Not Found). A request will also be free’d any time the library detects that the connection has been terminated– this can be dangerous if you still have a pointer to the structure, so see the next paragraph.

Since you (the library user) do not manually do the garbage collection, you may want to have a way to check whether a given request still exists in memory. For this purpose, you may associate the request with an int, and when/if the SCGI library frees the request, the int will have its value set to 1. This is done by setting the scgi_request’s int *dead field, which is NULL by default. See helloworld.c for an example.
scgi_write

## int scgi_write( scgi_request *req, char *txt );

Tell the library what HTTP response you would like to be sent in response to the request. This is meant to be called only once per request. Due to the non-blocking sockets feature, the response is not instantly sent, instead it is stored. The actual transmission of the response occurs when scgi_recv is called. If there is no time to send the entire transmission all at once when scgi_recv is called, the library will send as much of the response as it can, and send the rest on subsequent calls to scgi_recv.

# Example

For a basic example, see helloworld.c.

# Instructions

There is no installation or configuration for the library itself: just act like you wrote the .c and .h files yourself, putting them in the same location as all the other .c files in your project, etc.

Of course, web browsers don't send requests in SCGI. You must configure your webserver to act as the middleman. I will give instructions in Apache for now (NOTE: these instructions are probably outdated by now). I have not done it in any other webserver; maybe I will add instructions for other webservers later. You can always use a search engine and search for how to use scgi with your webbrowser.

Instructions for configuring Apache 2.2 to reroute (e.g.) all traffic to /scgilib/helloworld/ to an SCGI server listening on (say) port 8000:

1. First, install the mod_scgi module for Apache.
2. Then add the following to /etc/apache2/apache2.conf (the specific config file may vary with your Apache installation): SCGIMount /scgilib/helloworld/ 127.0.0.1:8000
3. Finally, reboot Apache.
4. Having done the above, now any time someone goes to http://www.mywebsite.com/scgilib/helloworld/, Apache takes their request, translates it into SCGI, and forwards it to the SCGI server on port 8000. Assuming that the helloworld server is actually running, it will give a response to Apache, who will then give that to the original client.

Sorry for the rather poor instructions section. I can only hope that anyone actually looking for something as obscure and specific as an SCGI Library already knows what they’re doing and can figure out the rest on their own!