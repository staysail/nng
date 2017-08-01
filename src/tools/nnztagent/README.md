nnztagent
=========

The nnztagent program is a "service" (daemon) that brokers ZeroTier
node services to libnng client applications.  This allows a single user
to run many applications, all of which work with a single ZeroTier
identity.

The brokering is done based on 16-bit local port number. The client
program must request a local port number, and then it will receive
a "connection" object suitable for use.

The agent understands TCP, and delivers stream oriented data to the
client program.  All of this is multiplexed over a single connection
to the daemon.

ZeroTier Identity
-----------------

nnztagent manages control of the ZeroTier node's identity, including
it's public and private key pairs.  These files are located in
a directory called ~/.nnztagent (or in $NNZTAGENT_HOME) on Unix systems.
This directory should only be accessible by the user.

Access Control
-------------

nnztagent relies on a shared secret, stored between the nnztagent
and the end user.  This allows the user to permit other users to
access the agent, or even other systems to, if the agent is permitted
to listen on a non-loopback network (this is not recommended!)

In order to authenticate that the remote client has permission to access
the connection, a challenge/response protocol is used, where each side
sends a challenge (random nonce to be encrypted), and the remote side
sends a response which ensures that the party knows the key.

By default the key is located in the .nnztagent directory, but it
can be stored in a different file by setting $NNZTAGENT_KEYFILE.
The agent will create this if it does not exist.

Security Considerations
-----------------------

The agent and client programs are assumed to have a secure channel with
each other; that is a channel that is immune to snooping or tampering
once formed.  This property is generally satisfied by loopback and IPC
connections.  It generally is *NOT* true when using TCP connections
over external networks.

The agent and client by default assume that local file system policies
(such as ACLs or POSIX permission bits) ensure that shared key material
is both accessible to authorized parties, and inaccessible to unauthorized
parties.  The simplest way to ensure this on UNIX systems is to ensure
both agent and client run under the same user, and that the .nnztagent
directory is mode 600 and owned by the same user.


Protocol Details
----------------

The protocol is a fully bi-directional protocol, and completely pairwise.
All integer values described below are transmitted in network-byte order
(big-endian).  Every message is prefixed by a pair of 32-bit words making
up a header, which is formatted thusly:

~~~~
  +---------------+------------------+----------------------+
  | Code <8 bits> | Length <24-bits> | Request ID <32-bits> |
  +---------------+------------------+----------------------+
~~~~

The length, which includes the 32-bit header, will always be less than 2^20.
(That is, we never send messages larger than 1MB, and never smaller than 4
bytes.)  The Code is an operation (described below), and the Request ID
is an identifier associated with each request, as described below.

The protocol operates with a request/reply model, whereby either party
may issue requests, and the receiving party must respond to each request.
There may be multiple requests outstanding; replies to the requests
must use the same request ID as the original request.  The Request ID
allows each party to match up responses with requests.

The initial request ID used by a sender should start at a random value,
and each new request should use the next request ID (adding one, and
wrapping if necessary) from the one prior.

Following the header is the payload, which varies by request type.


XXX... maybe we can use PAIR protocol for this?  Would be easier to
use an SP socket.  Then agent could probably just be moved out of the
tree altogether...  This would allow mangos to use this agent too.

ZeroTier Methods
================

`OK` (Code 0x00)
----------------

`OK` is issued in response to a successful request.  The payload for
the reply varies depending on the request, and is documented with each
request code.

`ERR` (Code 0x0F)
-----------------
`ERR` indicates that the request failed for some reason.  The response
payload is a 32-bit integer (network byte order) which is an error code
from the system, followed by a human readable message (such as the
output from strerror()).  (Error codes to be documented.)

`HELLO` (Code 0x01)
-------------------
`HELLO` is sent by each side at the start of the conversation.  The payload
is a structure of the following form:

~~~
  struct HELLO {
          uint64_t timestamp; // UNIX time in seconds
          uchar_t nonce[8]; // 64-bit random nonce
  };
~~~

If the timestamp differs by the recipients host clock by more than
30 seconds in either direction, then the message is discarded, and
the connection is dropped.

Otherwise an `OK` reply is created, with a payload that contains
a 32 byte hash, which is the output from SHA3-256 over the concatenation
of the request payload (timestamp+nonce) and the authentication key
(which should be at least 16 bytes of randomly generated data.)

The original request sender calculates the same hash indepently,
and compares it with the payload in the `OK` reply.  If they match,
then the peer has validated properly, and further communication can
occur. If they do not match, the connection is terminated immediately.

Note that each side must send a `HELLO` request and receive
the appropriate `OK` response before it can send any other type
of request.  Any other kinds of messages received before this
cause the connection to be terminated.

`IDENTIFY` (Code 0x02)
----------------------
`IDENTIFY` is sent by a client to request the ZeroTier Node ID.
The request payload is empty.  The `OK` reply payload is a 64-bit
integer containing the ZeroTier Node ID.  As Node IDs are only 40-bits
long, the upper 24 bits will be zero.


6PLANE Methods
==============

Note that all 6PLANE specific methods have '6' in the high order nibble of
the request code.  The client is expected to be able to calculate it's own
6PLANE address from it's Node ID and the network ID.

`BIND6P` (Code 0x61)
--------------------
`BIND6P` is used to bind to a port, and optionally to start the
process of listening.  The request payload has the following
structure:

~~~
        struct bind6p_req {
                uint64_t network_id;
                uint16_t local_port;
                uint16_t listen_depth;
        };
~~~

The `network_id` is the ZeroTier network to join.
The `local_port` is just a TCP port to bind to -- which may be
zero to request that the agent choose a port at random.  The
`listen_depth` is depth of the listen queue.  If the value of
`listen_depth` is zero, then the port is not bound in listening
mode, but is to be used instead for an outbound connection.

If the `listen_depth` is non-zero, then after a successful response,
the agent may begin sending the client `ACCEPT6P` requests as
remote peers attempt to connect to the service.

The `OK` response contains no payload.  If the actual bound port needs
to be known, then the client may request `SELFNAME6P`.

A given client may be bound at most once.  If multiple bindings
are needed, such as with servers listening on different ports, then
the client should open separate sessions to the agent.

`CONNECT6P` (Code 0x62)
-----------------------

`CONNECT6P` is used to initiate a request in the 6PLANE from one party
to another.  The client issues this to the agent, which should then
initiate an outgoing TCPv6 connection.  The request payload is described
by this struct:

~~~
    struct connect6p_req {
            uint64_t remote_id;
            uint16_t remote_port;
    };
~~~

The `remote_id` is a ZeroTier NodeID.  This can extracted from a regular
6PLANE address.  The `remote_port` is the remote server's TCP port to
connect on.  The agent will construct a proper 6PLANE IPv6 address from
the `remote_id` and the currently joined network id.

~~~
    struct connect6p_res {
            uint32_t conv_id;
    };
~~~

The `OK` reply consists of a unique 32-bit integer identifying this
conversation, which can be thought of as a TCP connection.  Note that
only a single connection can be active at a time on per session.

`ACCEPT6P` (Code 0x63)
----------------------
`ACCEPT6P` is issued from the agent in response to incoming connections.
It has a payload of the following form:

~~~
    struct accept6p_req {
            uint32_t conv_id;
    };
~~~

The `conv_id` is the 32-bit conversation ID, which can be thought of as a
TCP connection.

`PEERNAME6P` (Code 0x64) and `SELFNAME6P` (Code 0x65)
-----------------------------------------------------
These requests determine either the local or remote name associated with
a conversation.  The 'name' here is really the ZeroTier identity and
TCP port.  Both of these accept a single 32-bit conversation ID as
a payload.  This converation ID should correspond to the TCP
connection.  If the conversation ID is zero, then a `SELFNAME6P` request
will return information about the local port.

The reply in both instances is one of these:
~~~
    struct name6p {
        uint64_t node_id;
        uint16_t node_port;
    };
~~~

`DISCONNECT6P` (Code 0x66)
--------------------------
The `DISCONNECT6P`, indicates that the conversation is closed.  No further
`DATA6P` for that conversation shall be permitted.  If the agent receives this,
it shall close the underlying TCP connection.

`DATA6P` (Code 0x67)
--------------------
The `DATA6P` is used to transport TCP payload data.  The payload consists of
a single 32-bit value, the conversation ID, followed by the actual payload.
The conversation ID permits multiple TCP connections to be multiplexed
over a single session to the agent.  A successful response has no payload;
it is not possible to accept a partial amount of data.
