//
// MIT License
//
// Copyright 2024 <>< Charles Lohr
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the “Software”), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
//
// The following is mostly a demo of:
//  * Use of inotify to detect changes of /etc/hostname
//  * Use of `getifaddrs` to iterate through all available interfaces
//  * Use of `NETLINK_ROUTE` and `RTMGRP_IPV4_IFADDR` and `RTMGRP_IPV6_IFADDR`
//    to monitor for any new network interfaces or addresse.
//  * Use of multicast in IPv4 and IPv6 to join a multicast group
//  * Leveraging `poll` to have programs that are completely asleep when not
//    actively needed.
//  * Use of `recvmsg` to get the interface and address that a UDP packet is
//    received on
//  * Use optarg to parse command-line arguments.
//  * But it does implement a fully function mnds server that advertises your
//    host to other peers on your LAN!
//  * Also, it's shim "dns server" that bridges DNS to MDNS.
//

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <net/if.h>
#include <string.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <linux/in6.h>
#include <limits.h>
#include <fcntl.h>

// For detecting interfaces going away or coming back.
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

// For detecting "hostname" change.
#include <sys/inotify.h>

// For DNS -> MDNS forwarding we use fork/wait
#include <sys/wait.h>

//#define DISABLE_IPV6

#define MAX_MDNS_PATH (HOST_NAME_MAX+8)
#define MDNS_PORT 5353
#define RESOLVER_PORT 53
#define RESOLVER_IP "127.0.0.67"

# if __BYTE_ORDER == __BIG_ENDIAN
#define MDNS_BRD_ADDR ((in_addr_t) 0xe00000fb)  // 224.0.0.251
#else
#define MDNS_BRD_ADDR ((in_addr_t) 0xfb0000e0)  // 224.0.0.251
#endif

const char * hostname_override;
char         hostname[HOST_NAME_MAX+1];
int          hostnamelen = 0;
int          hostname_watch;

struct in_addr localInterface;
struct sockaddr_in groupSock;

int sdsock;
int is_ipv4_only;
int is_bound_6;
int sdifaceupdown;
int resolver;
int resolver_listener;

// For multicast queries, and multicast replies.
struct sockaddr_in sin_multicast = {
	.sin_family = AF_INET,
	.sin_addr = { MDNS_BRD_ADDR },
	.sin_port = 0 // Will get filled in at main()
};

static void ReloadHostname( void )
{
	if( hostname_override )
	{
		hostnamelen = strlen( hostname_override );
		if( hostnamelen >= HOST_NAME_MAX )
		{
			hostnamelen = HOST_NAME_MAX - 1;
		}
		memcpy( hostname, hostname_override, hostnamelen );
		hostname[hostnamelen] = 0;
		printf( "Using overridden name: \"%s.local\"\n", hostname );
		return;
	}

	int fh = open( "/etc/hostname", O_RDONLY );
	if( fh < 1 )
	{
		goto hostnamefault;
	}
	int rd = read( fh, hostname, HOST_NAME_MAX );
	close( fh );
	if( rd <= 0 )
	{
		goto hostnamefault;
	}

	hostnamelen = rd;

	int j;
	for( j = 0; j < rd; j++ )
	{
		char c = hostname[j];
		if( c == '\n' )                 // Truncate at newline
		{
			hostnamelen = j;
		}
		else if( c >= 'A' && c <= 'Z' ) // Convert to lowercase
		{
			hostname[j] = c + 'z' - 'Z';
		}
	}
	hostname[hostnamelen] = 0;

	printf( "Responding to hostname: \"%s.local\"\n", hostname );
	fflush( stdout );
	return;

hostnamefault:
	fprintf( stderr, "ERROR: Can't stat /etc/hostname\n" );
	return;
}

#ifndef DISABLE_IPV6
void AddMDNSInterface6( int interface )
{
	// Multicast v6 = ff01:0:0:0:0:0:0:fb

	struct ipv6_mreq mreq6 = {
		.ipv6mr_multiaddr = { { { 0xff,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0xfb } } },
		.ipv6mr_interface = interface,
	};

	if ( setsockopt( sdsock, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, (char *)&mreq6, sizeof(mreq6)) == -1)
	{
		fprintf( stderr, "WARNING: Could not join ipv6 membership to interface %d (%d %s)\n", interface, errno, strerror(errno) );
	}
}
#endif

void AddMDNSInterface4( struct in_addr * saddr )
{
	struct ip_mreq mreq = {
		.imr_multiaddr.s_addr = MDNS_BRD_ADDR,
		.imr_interface = *saddr
	};
	if ( setsockopt( sdsock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&mreq, sizeof(mreq)) == -1)
	{
		char * addr = inet_ntoa( *saddr );
		fprintf( stderr, "WARNING: Could not join membership to %s / code %d (%s)\n", addr, errno, strerror(errno) );
	}
}

int IsAddressLocal( struct in_addr * testaddr )
{
	uint32_t check = ntohl( testaddr->s_addr );
	if ( ( check & 0xff000000 ) == 0x7f000000 ) return 1; // 127.x.x.x (Link Local, but still want to join)
	if ( ( check & 0xff000000 ) == 0x0a000000 ) return 1; // 10.x.x.x
	if ( ( check & 0xfff00000 ) == 0xac100000 ) return 1; // 172.[16-31].x.x
	if ( ( check & 0xffff0000 ) == 0xc0a80000 ) return 1; // 192.168.x.x
	if ( ( check & 0xffff0000 ) == 0xa9fe0000 ) return 1; // 169.254.x.x (RFC5735)
	return 0;
}

#ifndef DISABLE_IPV6
int IsAddress6Local( struct in6_addr * addr )
{
	return IN6_IS_ADDR_LINKLOCAL( addr ) || IN6_IS_ADDR_SITELOCAL( addr );
}
#endif

int CheckAndAddMulticast( struct sockaddr * addr )
{
	if ( !addr )
	{
		return -1;
	}

	int family = addr->sa_family;

	if ( family == AF_INET )
	{
		char addrbuff[INET_ADDRSTRLEN+10] = { 0 }; // Actually 46 for IPv6, but let's add some buffer.
		struct sockaddr_in * sa4 = (struct sockaddr_in*)addr;
		const char * addrout = inet_ntop( family, &sa4->sin_addr, addrbuff, sizeof( addrbuff ) - 1 );
		int local = IsAddressLocal( &sa4->sin_addr );
		if ( !local ) return -2;
		printf( "Multicast adding address: %s\n", addrout );
		fflush( stdout );
		AddMDNSInterface4( &sa4->sin_addr );
	}
#ifndef DISABLE_IPV6
	else if ( family == AF_INET6 && !is_ipv4_only )
	{
		char addrbuff[INET6_ADDRSTRLEN+10] = { 0 }; // Actually 46 for IPv6, but let's add some buffer.
		struct sockaddr_in6 * sa6 = (struct sockaddr_in6 *)addr;
		const char * addrout = inet_ntop( family, &sa6->sin6_addr, addrbuff, sizeof( addrbuff ) - 1 );
		int local = IsAddress6Local( &sa6->sin6_addr );
		if ( !local ) return -3;
		printf( "Multicast adding interface: %d (%s)\n", sa6->sin6_scope_id, addrout );
		fflush( stdout );
		AddMDNSInterface6( sa6->sin6_scope_id );
	}
#endif
	return 0;
}

static int HandleRequestingInterfaces( void )
{
	struct ifaddrs * ifaddr = 0;
	if ( getifaddrs( &ifaddr ) == -1 )
	{
		fprintf( stderr, "Error: Could not query devices.\n" );
		return -1;
	}
	else
	{
		for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
		{
			struct sockaddr * addr = ifa->ifa_addr;
			CheckAndAddMulticast( addr );
		}
		freeifaddrs( ifaddr );
	}
	return 0;
}

static inline void HandleNetlinkData( void )
{
	int len;
	struct nlmsghdr *nlh;
	char buffer[4096];
	nlh = (struct nlmsghdr *)buffer;
	while ( ( len = recv( sdifaceupdown, nlh, 4096, MSG_DONTWAIT ) ) > 0 )
	{
		// technique is based around https://stackoverflow.com/a/2353441/2926815
		while ( ( NLMSG_OK( nlh, len ) ) && ( nlh->nlmsg_type != NLMSG_DONE ) )
		{
			if ( nlh->nlmsg_type == RTM_NEWADDR )
			{
				struct ifaddrmsg *ifa = (struct ifaddrmsg *) NLMSG_DATA( nlh );
				struct rtattr *rth = IFA_RTA( ifa );

				int rtl = IFA_PAYLOAD( nlh );

				while ( rtl && RTA_OK( rth, rtl ) )
				{
					if ( /*rth->rta_type == IFA_LOCAL || */ rth->rta_type == IFA_ADDRESS )
					{
						char name[IFNAMSIZ] = { 0 };
						if_indextoname( ifa->ifa_index, name );
						int pld = RTA_PAYLOAD(rth);

						// Record the index.
						if ( ifa->ifa_family == AF_INET )
						{
							struct sockaddr_in sai = { 0 };
							sai.sin_family = AF_INET;
							memcpy( &sai.sin_addr, RTA_DATA(rth), pld );
							CheckAndAddMulticast( (struct sockaddr*)&sai );
						}
#ifndef DISABLE_IPV6
						else if ( ifa->ifa_family == AF_INET6 )
						{
							int ifindex = ifa->ifa_index;
							struct sockaddr_in6 sai = { 0 };
							sai.sin6_family = AF_INET6;
							sai.sin6_scope_id = ifindex;
							memcpy( &sai.sin6_addr, RTA_DATA(rth), pld );
							CheckAndAddMulticast( (struct sockaddr*)&sai );
						}
#endif
					}
					rth = RTA_NEXT( rth, rtl );
				}
			}
			nlh = NLMSG_NEXT( nlh, len );
		}
	}
}

// MDNS functions from esp32xx

uint8_t * ParseMDNSPath( uint8_t * dat, uint8_t * dataend, char * topop, int * len )
{
	int l;
	int j;

	*len = 0;

	while(dat != dataend)
	{
		// See how long the string we should read is.
		l = *(dat++);

		// Zero-length strings indicate end-of-string.
		if( l == 0 )
			break;

		if( *len + l >= MAX_MDNS_PATH )
			return 0;

		//If not our first time through, add a '.'
		if( *len != 0 )
		{
			*(topop++) = '.';
			(*len)++;
		}

		for( j = 0; j < l; j++ )
		{
			if( dat[j] >= 'A' && dat[j] <= 'Z' )
				topop[j] = dat[j] - 'A' + 'a';
			else
				topop[j] = dat[j];
		}

		// Move along in the string, if there are more strings to concatenate.
		topop += l;
		dat += l;
		*len += l;
	}

	*topop = 0; //Null terminate.
	return dat;
}


static inline void HandleRX( int sock, int is_resolver )
{
	uint8_t buffer[9036]; // RFC6762 Section 6.1
	char path[MAX_MDNS_PATH];
	int i, stlen;

	struct sockaddr_in6 sender = { 0 };
	socklen_t sl = sizeof( sender );

	// Using recvmsg

	// This is a little tricky - so we can avoid having a separate socket for every single
	// interface, we can instead, just recvmsg and discern which interfaces the message
	// came frmo.

	// if you want access to the data you need to init the msg_iovec fields
	struct iovec iov = {
		.iov_base = buffer,
		.iov_len = sizeof( buffer ),
	};
	uint8_t cmbuf[1024];
	struct msghdr msghdr = {
		.msg_name = &sender,
		.msg_namelen = sizeof( sender ),
		.msg_control = cmbuf,
		.msg_controllen = sizeof( cmbuf ),
		.msg_flags = 0,
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};

	int r = recvmsg( sock, &msghdr, 0 );

	if( r < 0 || msghdr.msg_flags & (MSG_TRUNC | MSG_CTRUNC) )
	{
		// This should basically never happen.
		return;
	}

	if( r < 12 )
	{
		// Runt packet - can't do anything with these.
		return;
	}

	struct in_addr local_addr_4 = { 0 };
	int rxinterface = 0;
	int ipv4_valid = 0;
#ifndef DISABLE_IPV6
	struct in6_addr local_addr_6 = { 0 };
	int ipv6_valid = 0;
#endif

	for ( struct cmsghdr *cmsg = CMSG_FIRSTHDR( &msghdr );
    		cmsg != NULL;
    		cmsg = CMSG_NXTHDR( &msghdr, cmsg ) )
	{
		// ignore the control headers that don't match what we want
		// see https://stackoverflow.com/a/5309155/2926815
		if ( cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO )
		{
			struct in_pktinfo * pi = (struct in_pktinfo *)CMSG_DATA(cmsg);
			// at this point, peeraddr is the source sockaddr
			// pi->ipi_spec_dst is the destination in_addr
			// pi->ipi_addr is the destination address, in_addr
			local_addr_4 = pi->ipi_spec_dst;
			rxinterface = pi->ipi_ifindex;
			// pi->ipi_addr is actually the multicast address.
			ipv4_valid = 1;
		}
#ifndef DISABLE_IPV6
		else if( cmsg->cmsg_level == IPPROTO_IPV6 && 
				( cmsg->cmsg_type == IPV6_PKTINFO || cmsg->cmsg_type == IPV6_RECVPKTINFO ) )
		{
			// Some build platforms do not include this.
			struct in6_pktinfo_shadow
			{
				struct in6_addr ipi6_addr;	/* src/dst IPv6 address */
				unsigned int ipi6_ifindex;	/* send/recv interface index */
			};

			struct in6_pktinfo_shadow * pi = (struct in6_pktinfo_shadow *)CMSG_DATA(cmsg);

			local_addr_6 = pi->ipi6_addr;
			ipv6_valid = 1;
			rxinterface = pi->ipi6_ifindex;

			//int i;
			//for( i = 0; i < sizeof( local_addr_6 ); i++ )
			//	printf( "%02x", ((uint8_t*)&local_addr_6)[i] );
			//printf( "\n" );
		}
#endif
	}

	// Tricky - if ipv4 is valid, that means the ipv6 address is not to be trusted.
	// it's the broadcast address.
#ifndef DISABLE_IPV6
	if( ipv4_valid ) ipv6_valid = 0;
#endif
	uint16_t * psr = (uint16_t*)buffer;
	uint16_t xactionid = ntohs( psr[0] );
	uint16_t flags = ntohs( psr[1] );
	uint16_t questions = ntohs( psr[2] );
	// We discard answers.
	//uint16_t answers = ntohs( psr[3] );

	// Tricky - index 12 bytes in, we can do a direct reply.
	uint8_t * dataptr = (uint8_t*)buffer + 12;
	uint8_t * dataend = dataptr + r - 12;

	// MDNS reply (we are a server, not a client, so discard).
	if( flags & 0x8000 )
		return;

	int is_a_suitable_mdns_record_query = 0;

	int found = 0;

	//Query
	for( i = 0; i < questions; i++ )
	{
		uint8_t * namestartptr = dataptr;
		//Work our way through.
		dataptr = ParseMDNSPath( dataptr, dataend, path, &stlen );

		// Make sure there is still room left for the rest of the record.
		if( !dataptr || dataend - dataptr < 4 ) break;

		int pathlen = strlen( path );

		if( pathlen < 6 || strcmp( path + pathlen - 6, ".local" ) != 0 ) continue;

		uint16_t record_type = ( dataptr[0] << 8 ) | dataptr[1];

		// Record class is not used.
		//uint16_t record_class = ( dataptr[2] << 8 ) | dataptr[3];

		if( ( record_type == 1 ) || ( !is_ipv4_only && ( record_type == 28 ) ) )
		{
			is_a_suitable_mdns_record_query = 1;
		}

		const char * path_first_dot = path;
		const char * cpp = path;
		while( *cpp && *cpp != '.' ) cpp++;
		int dotlen = 0;
		if( *cpp == '.' )
		{
			path_first_dot = cpp+1;
			dotlen = path_first_dot - path - 1;
		}
		else
			path_first_dot = 0;

		if( hostname[0] && dotlen && dotlen == hostnamelen && memcmp( hostname, path, dotlen ) == 0 )
		{
			uint8_t outbuff[MAX_MDNS_PATH*2+128];
			uint8_t * obptr = outbuff;
			uint16_t * obb = (uint16_t*)outbuff;

			int sendA = ( record_type == 1 /*A*/ && ipv4_valid );
#ifndef DISABLE_IPV6
			int sendAAAA = ( record_type == 28 /*AAAA*/ && ipv6_valid );
#else
			int sendAAAA = 0;
#endif

			if( sendA || sendAAAA )
			{
				*(obb++) = xactionid;
				*(obb++) = htons(0x8400); //Authortative response.
				*(obb++) = 0;
				*(obb++) = htons( 1 ); //1 answer.
				*(obb++) = 0;
				*(obb++) = 0;

				obptr = (uint8_t*)obb;

				// Answer
				memcpy( obptr, namestartptr, stlen+1 ); //Hack: Copy the name in.
				obptr += stlen+1;
				*(obptr++) = 0;
				*(obptr++) = 0x00; *(obptr++) = (sendA ? 0x01 : 0x1c ); // A record
				*(obptr++) = 0x80; *(obptr++) = 0x01; //Flush cache + in ptr.
				*(obptr++) = 0x00; *(obptr++) = 0x00; //TTL
				*(obptr++) = 0x00; *(obptr++) = 240;  //240 seconds (4 minutes)
			}

			if( sendA )
			{
				*(obptr++) = 0x00; *(obptr++) = 0x04; //Size 4 (IP)
				memcpy( obptr, &local_addr_4.s_addr, 4 );
				obptr+=4;
			}
#ifndef DISABLE_IPV6
			else if( sendAAAA )
			{
				*(obptr++) = 0x00; *(obptr++) = 0x10; //Size 16 (IPv6)
				memcpy( obptr, &local_addr_6.s6_addr, 16 );
				obptr+=16;
			}
#endif

			if( sendA || sendAAAA )
			{
				sendto( sock, outbuff, obptr - outbuff, MSG_NOSIGNAL, (struct sockaddr*)&sender, sl );

				// Tricky: Make another socket to send
				int socks_to_send = socket( AF_INET, SOCK_DGRAM, 0 );
				//struct ip_mreqn txif = { 0 };
				//txif.imr_multiaddr.s_addr = MDNS_BRD_ADDR;
				//txif.imr_address.s_addr = local_addr_4.s_addr;
				//txif.imr_ifindex = rxinterface;
				rxinterface = rxinterface; // We aren't using this now, see note below.

				// With IP_MULTICAST_IF you can either pass in an ip_mreqn, or just the local_addr4.
				// We tried to do the full txif for clarity / example. But, it seems to cause issues?
				if( setsockopt( socks_to_send, IPPROTO_IP, IP_MULTICAST_IF, &local_addr_4, sizeof(local_addr_4)) != 0 )
				{
					fprintf( stderr, "WARNING: Could not set IP_MULTICAST_IF for reply\n" );
				}

				int optval = 1;
				if ( setsockopt( socks_to_send, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof( optval ) ) != 0 )
				{
					fprintf( stderr, "WARNING: Could not set SO_REUSEPORT for reply\n" );
				}
				struct sockaddr_in sin = {
					.sin_family = AF_INET,
					.sin_addr = { INADDR_ANY },
					.sin_port = htons( MDNS_PORT )
				};
				if ( bind( socks_to_send, (struct sockaddr *)&sin, sizeof(sin) ) == -1 )
				{
					fprintf( stderr, "WARNING: When sending reply, could not bind to IPv4 MDNS port (%d %s)\n", errno, strerror( errno ) );
				}
				if ( sendto( socks_to_send, outbuff, obptr - outbuff, MSG_NOSIGNAL,
					(struct sockaddr*)&sin_multicast, sizeof(sin_multicast) ) != obptr - outbuff )
				{
					fprintf( stderr, "WARNING: Could not send multicast reply for MDNS query\n" );
				}
				close( socks_to_send );
			}

			found = 1;
		}
	}


	// We could also reply with services here.

	// But, if we aren't sending a response, and we're a resolver, we have to do more work.
//	printf( "CHECK: %d %d %d %d\n", found, is_resolver, resolver, is_an_a_mdns_record_query );
	if( !found && is_resolver && resolver )
	{
		// If we are resolving, just yolo this off to the rest of the network.
		// As a note, Only IPv4 records are supported.  AAAA records seem to jank things up.
		if( is_a_suitable_mdns_record_query )
		{
			int pid_of_resolver = fork();

			if( pid_of_resolver == 0 )
			{
				// This is a fork()'d pid - from here on out we have to make sure to exit.

				#define MAX_IFACES_SOCKS
				int socks_to_send = socket( AF_INET, SOCK_DGRAM, 0 );
				if( !socks_to_send )
				{
					fprintf( stderr, "WARNING: Could not create multicast message\n" );
					exit( 0 );
				}

				int loopbackEnable = 0;
				if( setsockopt( socks_to_send, IPPROTO_IP, IP_MULTICAST_LOOP, &loopbackEnable, sizeof( loopbackEnable ) ) < 0 )
				{
					fprintf( stderr, "WARNING: Cannot prevent self-looping of mdns packets\n" );
					exit( 0 );
				}

				struct timeval tv = { .tv_sec = 3 };
				if( setsockopt( socks_to_send, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof( tv ) ) < 0 )
				{
					fprintf( stderr, "WARNING: Could not set sock option on repeated socket\n" );
					close( socks_to_send );
					exit( 0 );
				}

				if( sendto( socks_to_send, buffer, r, MSG_NOSIGNAL, (struct sockaddr*)&sin_multicast, sizeof( sin_multicast ) ) < 0 )
				{
					fprintf( stderr, "WARNING: Could not repeat as MDNS request\n" );
					close( socks_to_send );
					exit( 0 );
				}

				for( ;; )
				{
					r = recv( socks_to_send, buffer, sizeof(buffer), 0 );
					if( r <= 0 )
						break;

					// If the packet is a reply, not a question, we can forward it back to the asker.
					uint16_t flags = ntohs( ((uint16_t*)buffer)[1] );
					if( ( flags & 0x8000 ) )
						sendto( sock, buffer, r, MSG_NOSIGNAL, (struct sockaddr*)&sender, sl );
				}

				close( socks_to_send );
				exit( 0 );
			}
		}
		else
		{
			// We want to make them go away.
			uint16_t * psr = (uint16_t*)buffer;
			//  psr[0] is the transaction ID
			psr[1] = 0x8100; // If we wanted, we could set this to be 0x8103, to say "no such name" - but then if there's an AAAA query as well, that will cancel out an A query.
			// Send the packet back at them.
			sendto( sock, buffer, r, MSG_NOSIGNAL, (struct sockaddr*)&sender, sl );
		}
	}
	return;
}

int main( int argc, char *argv[] )
{
	int c;
	while ( ( c = getopt (argc, argv, "r4h:" ) ) != -1 )
	{
		switch (c)
		{
		case 'h':
			hostname_override = optarg;
			break;
		case 'r':
			resolver = socket( AF_INET, SOCK_DGRAM, 0 );
			break;
		case '4':
			is_ipv4_only = 1;
			break;
		default:
		case '?':
			fprintf( stderr, "Error: Usage: minimdnsd [-r] [-h hostname override]\n" );
			return -5;
		}
	}

	sin_multicast.sin_port = htons( MDNS_PORT );

	ReloadHostname();

	int inotifyfd = inotify_init1( IN_NONBLOCK );

	if( !hostname_override )
	{
		hostname_watch = inotify_add_watch( inotifyfd, "/etc/hostname", IN_MODIFY | IN_CREATE );
		if( hostname_watch < 0 )
		{
			fprintf( stderr, "WARNING: inotify cannot watch file\n" );
		}
	}

	if( resolver < 0 )
	{
		fprintf( stderr, "FATAL: Resolver requested but unavailable.\n" );
		return -5;
	}

	if( resolver )
	{
		// The resolver uses child processes.  To clean up zombies, we catch SIGCHILD.
		//signal( SIGCHLD, &ChildProcessComplete );

		int optval = 1;
		if ( setsockopt( resolver, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof( optval ) ) != 0 )
		{
			fprintf( stderr, "WARNING: Could not set SO_REUSEPORT on resolver\n" );
			return -5;
		}
		struct sockaddr_in sin_resolve = {
			.sin_family = AF_INET,
			.sin_addr = { inet_addr( RESOLVER_IP ) },
			.sin_port = htons( RESOLVER_PORT )
		};
		if ( bind( resolver, (struct sockaddr *)&sin_resolve, sizeof(sin_resolve) ) == -1 )
		{
			fprintf( stderr, "FATAL: Could not bind to IPv4 MDNS port (%d %s)\n", errno, strerror( errno ) );
			return -5;
		}
		printf( "Resolver configured on \"%s\"\n", RESOLVER_IP );
	}

#ifndef DISABLE_IPV6
	if( !is_ipv4_only )
	{
		sdsock = socket( AF_INET6, SOCK_DGRAM, 0 );
		if ( sdsock < 0 )
		{
			fprintf( stderr, "WARNING: Opening IPv6 datagram socket error.  Trying IPv4");
			sdsock = socket( AF_INET, SOCK_DGRAM, 0 );
			is_bound_6 = 0;
		}
		else
		{
			is_bound_6 = 1;
		}
	}
	else
	{
#endif
	sdsock = socket( AF_INET, SOCK_DGRAM, 0 );
	if ( sdsock < 0 )
	{
		fprintf( stderr, "FATAL: Could not open IPv4 Socket\n");
	}
#ifndef DISABLE_IPV6
	}
#endif

	// Not just avahi, but other services, too will bind to 5353, but we can use
	// SO_REUSEPORT to allow multiple people to bind simultaneously.
	int optval = 1;
	if ( setsockopt( sdsock, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof( optval ) ) != 0 )
	{
		fprintf( stderr, "WARNING: Could not set SO_REUSEPORT\n" );
	}

	if ( setsockopt( sdsock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof( optval ) ) != 0 )
	{
		fprintf( stderr, "WARNING: Could not set SO_REUSEADDR\n" );
	}

	// We have to enable PKTINFO so we can use recvmsg, so we can get desination address
	// so we can reply accordingly.
	if( setsockopt( sdsock, IPPROTO_IP, IP_PKTINFO, &optval, sizeof( optval ) ) != 0 )
	{
		fprintf( stderr, "Fatal: OS Does not support IP_PKTINFO on IPv6 socket.\n" );
		return -9;
	}

#ifndef DISABLE_IPV6
	if( is_bound_6 && setsockopt( sdsock, IPPROTO_IPV6, IPV6_RECVPKTINFO, &optval, sizeof( optval ) ) != 0 )
	{
		fprintf( stderr, "Fatal: OS Does not support IP_PKTINFO on IPv6 socket.\n" );
		return -9;
	}
#endif

	sdifaceupdown = socket( PF_NETLINK, SOCK_RAW, NETLINK_ROUTE );
	if ( sdifaceupdown < 0 )
	{
		fprintf( stderr, "WARNING: Couldn't open socket for monitoring address changes.\n");
	}
	else
	{
		// Bind looking for interface changes.
		struct sockaddr_nl addr;
		memset(&addr, 0, sizeof(addr));
		addr.nl_family = AF_NETLINK;
		addr.nl_groups = RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
		if (bind( sdifaceupdown, (struct sockaddr *)&addr, sizeof(addr)) == -1)
		{
			fprintf( stderr, "WARNING: couldn't bind looking for address changes\n" );
			close( sdifaceupdown );
			sdifaceupdown = -1;
		}
	}

	// Bind the normal MDNS socket
#ifndef DISABLE_IPV6
	if( is_bound_6 )
	{
		struct sockaddr_in6 sin6 = {
			.sin6_family = AF_INET6,
			.sin6_addr = IN6ADDR_ANY_INIT,
			.sin6_port = htons( MDNS_PORT )
		};
		if ( bind( sdsock, (struct sockaddr *)&sin6, sizeof(sin6) ) == -1 )
		{
			fprintf( stderr, "FATAL: Could not bind to IPv6 MDNS port (%d %s)\n", errno, strerror( errno ) );
			exit(-1);
		}
	}
	else
#endif
	{
		struct sockaddr_in sin = {
			.sin_family = AF_INET,
			.sin_addr = { INADDR_ANY },
			.sin_port = htons( MDNS_PORT )
		};
		if ( bind( sdsock, (struct sockaddr *)&sin, sizeof(sin) ) == -1 )
		{
			fprintf( stderr, "FATAL: Could not bind to IPv4 MDNS port (%d %s)\n", errno, strerror( errno ) );
			exit(-1);
		}
	}

	// Some things online recommend using IPPROTO_IP, IP_MULTICAST_LOOP
	// But, we can just ignore the replies.

	int r;
	do
	{
		int failcount = 0;
		r = HandleRequestingInterfaces();
		if ( r != 0 )
		{
			if ( failcount++ > 10 )
			{
				fprintf( stderr, "Fatal: Too many failures getting interfaces. Aborting\n" );
				return -9;
			}
		}
	} while ( r != 0 );

	while ( 1 )
	{
		struct pollfd fds[4] = {
			{ .fd = sdsock, .events = POLLIN | POLLHUP | POLLERR, .revents = 0 },
			{ .fd = sdifaceupdown, .events = POLLIN | POLLHUP | POLLERR, .revents = 0 },
			{ .fd = inotifyfd, .events = POLLIN, .revents = 0 },
			{ .fd = resolver, .events = POLLIN | POLLHUP | POLLERR, .revents = 0 },
		};

		int polls = resolver ? 4 : 3;

		// Make poll wait for literally forever.
		r = poll( fds, polls, -1 );

		//printf( "%d: %d / %d / %d / %d\n", r, fds[0].revents, fds[1].revents, fds[2].revents, fds[3].revents );

		if ( r < 0 )
		{
			fprintf( stderr, "Fatal: poll = %d failed (%d %s)\n", r, errno, strerror( errno ) );
			return -10;
		}

		if ( fds[0].revents )
		{
			if ( fds[0].revents & POLLIN )
			{
				HandleRX( sdsock, 0 );
			}

			if( fds[0].revents & ( POLLHUP | POLLERR ) )
			{
				fprintf( stderr, "Fatal: IPv6 socket experienced fault.  Aborting\n" );
				return -14;
			}
		}

		if ( fds[1].revents )
		{
			if ( fds[1].revents & POLLIN )
			{
				HandleNetlinkData( );
			}
			if( fds[1].revents & ( POLLHUP | POLLERR ) )
			{
				fprintf( stderr, "Fatal: NETLINK socket experienced fault.  Aborting\n" );
				return -14;
			}
		}

		if ( fds[2].revents )
		{
			struct inotify_event event;
			int r = read( inotifyfd, &event, sizeof( event ) );
			r = r;
			ReloadHostname();
		}
		if ( fds[3].revents )
		{
			if ( fds[3].revents & POLLIN )
			{
				HandleRX( resolver, 1 );
			}

			if ( fds[3].revents & ( POLLHUP | POLLERR ) )
			{
				fprintf( stderr, "Fatal: resolver socket experienced fault.  Aborting\n" );
				return -14;
			}
		}

		// Cleanup any remaining zombie processes from resolver.
		// Could also be done in a SIGCHLD signal handler, but that would
		// Interrupt the poll.
		if( resolver )
		{
			int wstat;
			wait3( &wstat, WNOHANG, NULL );
		}
	}
	return 0;
}
