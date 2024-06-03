#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <fcntl.h>
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

// For detecting interfaces going away or coming back.
#include <linux/netlink.h>
#include <linux/rtnetlink.h>


#define MAX_MDNS_NAMES 1

char	hostname[HOST_NAME_MAX+1];
time_t	hosttime = -1;
int		hostnamelen = 0;


struct sockaddr_in * sockaddrListByIFace4;
int    maxIFaceList4;
struct sockaddr_in6 * sockaddrListByIFace6;
int    maxIFaceList6;

#define MAX_MDNS_PATH (HOST_NAME_MAX+8)

#define MDNS_PORT 5353

struct in_addr localInterface;
struct sockaddr_in groupSock;

int sdsock;
int is_bound_6;
int sdifaceupdown;

int AddMDNSInterface4( struct in_addr * saddr )
{
	struct ip_mreq mreq;
	mreq.imr_multiaddr.s_addr = inet_addr( "224.0.0.251" );
	memcpy( &mreq.imr_interface, saddr, sizeof( *saddr ) );
	if ( setsockopt( sdsock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&mreq, sizeof(mreq)) == -1)
	{
		char * addr = inet_ntoa( *saddr );
		fprintf( stderr, "WARNING: Could not join membership to %s / code %d (%s)\n", addr, errno, strerror(errno) );
		return -1;
	}
	return 0;
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

int IsAddress6Local( struct in6_addr * addr )
{
	return IN6_IS_ADDR_LINKLOCAL( addr ) || IN6_IS_ADDR_SITELOCAL( addr );
}

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
		AddMDNSInterface4( &sa4->sin_addr );
	}
	else if ( family == AF_INET6 )
	{
		char addrbuff[INET6_ADDRSTRLEN+10] = { 0 }; // Actually 46 for IPv6, but let's add some buffer.
		struct sockaddr_in6 * sa6 = (struct sockaddr_in6 *)addr;
		const char * addrout = inet_ntop( family, &sa6->sin6_addr, addrbuff, sizeof( addrbuff ) - 1 );
		int local = IsAddress6Local( &sa6->sin6_addr );
		if ( !local ) return -3;
		printf( "LOCAL: %s, but join not written yet\n", addrout );
	}
	return 0;
}

int HandleRequestingInterfaces()
{
	static int failcount;
	struct ifaddrs * ifaddr = 0;
	if ( getifaddrs( &ifaddr ) == -1 )
	{
		fprintf( stderr, "Error: Could not query devices.\n" );
		return -1;
	}
	else
	{
		failcount = 0;
		for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
		{
			struct sockaddr * addr = ifa->ifa_addr;
			CheckAndAddMulticast( addr );
		}
	}
	return 0;
}



static inline void HandleNetlinkData()
{
	int len;
	struct nlmsghdr *nlh;
	char buffer[4096];
	nlh = (struct nlmsghdr *)buffer;
	while ( ( len = recv( sdifaceupdown, nlh, 4096, 0 ) ) > 0 )
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
						int ifindex = ifa->ifa_index;
						char name[IFNAMSIZ] = { 0 };
						if_indextoname( ifa->ifa_index, name );
						//printf( "Update Device: %s / Family %d\n", name, ifa->ifa_family );
						int pld = RTA_PAYLOAD(rth);

						// Record the index.
						if ( ifa->ifa_family == AF_INET6 )
						{
							struct sockaddr_in6 sai = { 0 };
							sai.sin6_family = AF_INET6;
							sai.sin6_scope_id = ifindex;
							memcpy( &sai.sin6_addr, RTA_DATA(rth), pld );
							CheckAndAddMulticast( (struct sockaddr*)&sai );
							if( ifindex >= maxIFaceList6 )
							{
								int newlen = ifindex + 1;
								sockaddrListByIFace6 = realloc( sockaddrListByIFace6, newlen * sizeof( sai ) );
								memset( sockaddrListByIFace6 + maxIFaceList6, 0, sizeof( sai ) * (newlen - maxIFaceList6) );
								maxIFaceList6 = newlen;
							}
							memcpy( &sockaddrListByIFace6[ifindex], &sai, sizeof( sai ) );
						}
						else if ( ifa->ifa_family == AF_INET )
						{
							struct sockaddr_in sai = { 0 };
							sai.sin_family = AF_INET;
							memcpy( &sai.sin_addr, RTA_DATA(rth), pld );
							CheckAndAddMulticast( (struct sockaddr*)&sai );
							if( ifindex >= maxIFaceList4 )
							{
								int newlen = ifindex + 1;
								sockaddrListByIFace4 = realloc( sockaddrListByIFace4, newlen * sizeof( sai ) );
								memset( sockaddrListByIFace4 + maxIFaceList4, 0, sizeof( sai ) * (newlen - maxIFaceList6) );
								maxIFaceList4 = newlen;
							}
							memcpy( &sockaddrListByIFace4[ifindex], &sai, sizeof( sai ) );
						}
					}
					rth = RTA_NEXT( rth, rtl );
				}
			}
			nlh = NLMSG_NEXT( nlh, len );
		}
	}
}

// MDNS functions from esp32xx

uint8_t * ParseMDNSPath( uint8_t * dat, char * topop, int * len )
{
	int l;
	int j;
	*len = 0;
	do
	{
		l = *(dat++);
		if( l == 0 )
		{
			*topop = 0;
			return dat;
		}
		if( *len + l >= MAX_MDNS_PATH ) return 0;

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
		topop += l;
		dat += l;
		*topop = 0; //Null terminate.
		*len += l;
	} while( 1 );
}


static inline void HandleRX( int sock )
{
	uint8_t buffer[9036]; // RFC6762 Section 6.1
	char path[MAX_MDNS_PATH];
	int i, j, stlen;

	struct sockaddr_in6 sender = { 0 };
	socklen_t sl = sizeof( sender );
	int r = recvfrom( sock, buffer, sizeof(buffer), 0, (struct sockaddr*) &sender, &sl );

	if( r <= 0 )
		return;	

	for( i = 0; i < sl; i++ )
		printf( "%02x ", ((uint8_t*)&sender)[i] );
	printf( "\n" );

	int iface = htonl( sender.sin6_scope_id );
	printf( "FAMILY: %d / %d\n", sender.sin6_family, iface );

	uint16_t * psr = (uint16_t*)buffer;
	uint16_t xactionid = ntohs( psr[0] );
	uint16_t flags = ntohs( psr[1] );
	uint16_t questions = ntohs( psr[2] );
	uint16_t answers = ntohs( psr[3] );

	// Tricky - index 12 bytes in, we can do a direct reply.
	uint8_t * dataptr = (uint8_t*)buffer + 12;
	uint8_t * dataend = dataptr + r - 12;

	// MDNS reply (we are a server, not a client, so discard).
	if( flags & 0x8000 )
		return;

	//Query
	for( i = 0; i < questions; i++ )
	{
		uint8_t * namestartptr = dataptr;
		//Work our way through.
		dataptr = ParseMDNSPath( dataptr, path, &stlen );

		if( dataend - dataptr < 4 ) return;

		if( !dataptr )
		{
			return;
		}

		int pathlen = strlen( path );
		if( pathlen < 6 )
		{
			continue;
		}
		if( strcmp( path + pathlen - 6, ".local" ) != 0 )
		{
			continue;
		}

		uint16_t record_type = ( dataptr[0] << 8 ) | dataptr[1];
		uint16_t record_class = ( dataptr[2] << 8 ) | dataptr[3];

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

		int found = 0;

		struct stat s;
		int d = stat( "/etc/hostname", &s );
		if( d )
		{
			goto hostnamefault;
		}
		if( s.st_mtime != hosttime )
		{
			hosttime = s.st_mtime;
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
		}

		if( hostname[0] && dotlen && dotlen == hostnamelen && memcmp( hostname, path, dotlen ) == 0 )
		{
			struct sockaddr_in  * ipv4 = ( iface < maxIFaceList4 ) ? sockaddrListByIFace4 + iface : 0;
			struct sockaddr_in6 * ipv6 = ( iface < maxIFaceList6 ) ? sockaddrListByIFace6 + iface : 0;

			// Check validity.
			if( ipv4 && !ipv4->sin_family ) ipv4 = 0;
			if( ipv6 && !ipv6->sin6_family ) ipv6 = 0;

			printf( "%d %p %p\n", iface, ipv4, ipv6 );

			if( record_type == 1 /*A*/ && ipv4 ) //A Name Lookup.
			{
				//Found match with us.
				//Send a response.

				uint8_t outbuff[MAX_MDNS_PATH+48];
				uint8_t * obptr = outbuff;
				uint16_t * obb = (uint16_t*)outbuff;
				*(obb++) = xactionid;
				*(obb++) = htons(0x8400); //Authortative response.
				*(obb++) = 0;
				*(obb++) = htons(1); //1 answer.
				*(obb++) = 0;
				*(obb++) = 0;

				obptr = (uint8_t*)obb;
				memcpy( obptr, namestartptr, stlen+1 ); //Hack: Copy the name in.
				obptr += stlen+1;
				*(obptr++) = 0;
				*(obptr++) = 0x00; *(obptr++) = 0x01; //A record
				*(obptr++) = 0x80; *(obptr++) = 0x01; //Flush cache + in ptr.
				*(obptr++) = 0x00; *(obptr++) = 0x00; //TTL
				*(obptr++) = 0x00; *(obptr++) = 240;  //240 seconds (4 minutes)
				*(obptr++) = 0x00; *(obptr++) = 0x04; //Size 4 (IP)
				
				memcpy( obptr, &ipv4->sin_addr, 4 );
				obptr+=4;

				sendto( sock, outbuff, obptr - outbuff, 0, (struct sockaddr*)&sender, sl );
			}
			else if( record_type == 28 /*AAAA*/ && ipv6 )
			{
				// XXX TODO: Figure out IPv6 Replies.

			}

//			else
	//			SendSpecificService( i, namestartptr, xactionid, stlen, 1 );
			found = 1;
		}	

/*
		for( i = 0; i < MAX_MDNS_NAMES; i++ )
		{
			//Handle [hostname].local, or [hostname].[service].local
			if( MDNSNames[i] && dotlen && strncmp( MDNSNames[i], path, dotlen ) == 0 && dotlen == strlen( MDNSNames[i] ))
			{
				found = 1;
				if( record_type == 0x0001 ) //A Name Lookup.
					SendOurARecord( namestartptr, xactionid, stlen, 1 );
				else
					SendSpecificService( i, namestartptr, xactionid, stlen, 1 );
			}
		}
*/


#if 0

		if( !found ) //Not a specific entry lookup...
		{
			//Is this a browse?
			if( strcmp( path, "_services._dns-sd._udp.local" ) == 0 )
			{
				SendAvailableServices( namestartptr, xactionid, stlen );
			}
			else
			{
				// FUTURE: Possibly support services.
/*
				//A specific service?
				for( i = 0; i < MAX_MDNS_SERVICES; i++ )
				{
					const char * srv = MDNSServices[i];
					if( !srv ) continue;
					int sl = strlen( srv );
					if( strncmp( path, srv, sl ) == 0 )
					{
						SendSpecificService( i, namestartptr, xactionid, stlen, 0 );
					}
				}
*/
			}
		}
#endif

	}
	return;
hostnamefault:
	fprintf( stderr, "ERROR: Can't stat /etc/hostname\n" );
	return;
}

int main (int argc, char *argv[] )
{
	struct sockaddr_in respsock;

	sdsock = socket( AF_INET6, SOCK_DGRAM, 0 );
	if ( sdsock < 0 )
	{
		fprintf( stderr, "Warning: Opening IPv6 datagram socket error.  Trying IPv4");
		sdsock = socket( AF_INET, SOCK_DGRAM, 0 );
		is_bound_6 = 0;
	}
	else
	{
		is_bound_6 = 1;
	}

	// Not just avahi, but other services, too will bind to 5353, but we can use
	// SO_REUSEPORT to allow multiple people to bind simultaneously.
	int optval = 1;
	if ( setsockopt( sdsock, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof( optval ) ) != 0 )
	{
		fprintf( stderr, "Warning: Could not set SO_REUSEPORT\n" );
	}

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
	{
		struct sockaddr_in sin = {
			.sin_family = AF_INET,
			.sin_addr = INADDR_ANY,
			.sin_port = htons( MDNS_PORT )
		};
		if ( bind( sdsock, (struct sockaddr *)&sin, sizeof(sin) ) == -1 )
		{
			fprintf( stderr, "FATAL: Could not bind to IPv4 MDNS port (%d %s)\n", errno, strerror( errno ) );
			exit(-1);
		}
	}

	char ** lastiplist = 0;

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
		struct pollfd fds[2] = {
			{ .fd = sdsock, .events = POLLIN | POLLHUP | POLLERR, .revents = 0 },
			{ .fd = sdifaceupdown, .events = POLLIN | POLLHUP | POLLERR, .revents = 0 },
		};

		int socks = 2;

		// Make poll wait for literally forever.
		r = poll( fds, socks, -1 );

		if ( r < 0 )
		{
			fprintf( stderr, "Fatal: Poll failed\n" );
			return -10;
		}

		if ( fds[0].revents )
		{
			if ( fds[0].revents & POLLIN )
			{
				HandleRX( sdsock );
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
				HandleNetlinkData();
			}
			if( fds[1].revents & ( POLLHUP | POLLERR ) )
			{
				fprintf( stderr, "Fatal: NETLINK socket experienced fault.  Aborting\n" );
				return -14;
			}
		}
	}
	return 0;
}
