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

// For detecting interfaces going away or coming back.
#include <linux/netlink.h>
#include <linux/rtnetlink.h>


#define MAX_MDNS_NAMES 1
#define MAX_MDNS_PATH 32

#define MDNS_PORT 5353

struct in_addr localInterface;
struct sockaddr_in groupSock;

int sd;
int sd6;
int sdifaceupdown;

int AddMDNSInterface4( struct in_addr * saddr )
{
	struct ip_mreq mreq;
	mreq.imr_multiaddr.s_addr = inet_addr( "224.0.0.251" );
	memcpy( &mreq.imr_interface, saddr, sizeof( *saddr ) );
	if ( setsockopt( sd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&mreq, sizeof(mreq)) == -1)
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
	return IN6_IS_ADDR_LINKLOCAL( addr ) || IN6_IS_ADDR_SITELOCAL( addr ) || ( addr->s6_addr[0] == 0xfd && addr->s6_addr[1] == 0xdc );
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
						char name[IFNAMSIZ] = { 0 };
						if_indextoname( ifa->ifa_index, name );
						//printf( "Update Device: %s / Family %d\n", name, ifa->ifa_family );
						int pld = RTA_PAYLOAD(rth);
						if ( ifa->ifa_family == AF_INET6 )
						{
							struct sockaddr_in6 sai = { 0 };
							sai.sin6_family = AF_INET6;
							memcpy( &sai.sin6_addr, RTA_DATA(rth), pld );
							CheckAndAddMulticast( (struct sockaddr*)&sai );
						}
						else if ( ifa->ifa_family == AF_INET )
						{
							struct sockaddr_in sai = { 0 };
							sai.sin_family = AF_INET;
							memcpy( &sai.sin_addr, RTA_DATA(rth), pld );
							CheckAndAddMulticast( (struct sockaddr*)&sai );
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


static void SendOurARecord( uint8_t * namestartptr, int xactionid, int stlen, uint32_t ourip )
{
	//Found match with us.
	//Send a response.

	uint8_t outbuff[MAX_MDNS_PATH+32];
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
	memcpy( obptr, &ourip, 4 );
	obptr+=4;
	uint32_t md = MDNS_BRD;
	memcpy( pMDNSServer->proto.udp->remote_ip, &md, 4 );
	espconn_sent( pMDNSServer, outbuff, obptr - outbuff );
}


static inline void HandleRX( int sock )
{
	
	uint8_t buffer[8192];
	char path[MAX_MDNS_PATH];
	int i, j, stlen;
	struct sockaddr sender;

	// This is a little tricky - so we can avoid having a separate socket for every single
	// interface, we can instead, just recvmsg and discern which interfaces the message
	// came frmo.

	char ctrlbuff[512];
//	int r = recvfrom( sd, buffer, sizeof(buffer), MSG_DONTWAIT, &sender, &address_len );
//	if( r <= 0 ) return;
	struct sockaddr_in peeraddr;
	// if you want access to the data you need to init the msg_iovec fields
	struct msghdr msghdr = {
		.msg_name = &sender,
		.msg_namelen = sizeof( sender ),
		.msg_control = ctrlbuff,
		.msg_controllen = sizeof( ctrlbuff ),
	};
	recvmsg(sock, &msghdr, 0);

XXX TODO: Can we use recv here?
	
	uint16_t * psr = (uint16_t*)buffer;
	uint16_t xactionid = ntohs( psr[0] );
	uint16_t flags = ntohs( psr[1] );
	uint16_t questions = ntohs( psr[2] );
	uint16_t answers = ntohs( psr[3] );

	uint8_t * dataptr = (uint8_t*)pusrdata + 12;
	uint8_t * dataend = dataptr + len;

	if( flags & 0x8000 )
	{
		// MDNS reply (we are a server, not a client, so discard).
		return;
	}


	//Query
	for( i = 0; i < questions; i++ )
	{
		uint8_t * namestartptr = dataptr;
		//Work our way through.
		dataptr = ParseMDNSPath( dataptr, path, &stlen );

		if( dataend - dataptr < 10 ) return;

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
		for( i = 0; i < MAX_MDNS_NAMES; i++ )
		{
			//Handle [hostname].local, or [hostname].[service].local
			if( MDNSNames[i] && dotlen && strncmp( MDNSNames[i], path, dotlen ) == 0 && dotlen == strlen( MDNSNames[i] ))
			{
				found = 1;
				if( record_type == 0x0001 ) //A Name Lookup.
					SendOurARecord( namestartptr, xactionid, stlen,  );
				else
					SendSpecificService( i, namestartptr, xactionid, stlen, 1 );
			}
		}

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
	}
}

int main (int argc, char *argv[] )
{
	struct sockaddr_in sinsock = { 0 };
	struct sockaddr_in respsock;

	sd = socket( AF_INET, SOCK_DGRAM, 0 );
	if ( sd < 0 )
	{
		perror("Opening datagram socket error");
		exit(1);
	}

	sd6 = socket( AF_INET, SOCK_DGRAM, 0 );
	if ( sd6 < 0 )
	{
		fprintf( stderr, "Warning: Opening IPv6 datagram socket error");
		sd6 = 0;
	}

	int opt = 1;
	if( setsockopt(sd, IPPROTO_IP, IP_PKTINFO, &opt, sizeof(opt)) != 0 )
	{
		fprintf( stderr, "Fatal: OS Does not support IP_PKTINFO on IPv4 socket.\n" );
		return -9;
	}

	if( setsockopt(sd6, IPPROTO_IP, IP_PKTINFO, &opt, sizeof(opt)) != 0 )
	{
		fprintf( stderr, "Fatal: OS Does not support IP_PKTINFO on IPv6 socket.\n" );
		return -9;
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


	// Bind the normal MDNS socket, that will get the messages.
	sinsock.sin_family = AF_INET;
	sinsock.sin_addr.s_addr = htonl( INADDR_ANY );
	sinsock.sin_port = htons( MDNS_PORT );

	if ( bind( sd, (struct sockaddr *)&sinsock, sizeof( sinsock ) ) == -1 )
	{
		fprintf( stderr, "Warning: Could not bind to MDNS port\n" );
		fprintf( stderr, "You can figure out whose fault it is with:\n" );
		fprintf( stderr, " sudo netstat -nap | grep 5353\n" );
		fprintf( stderr, "But, we are going to attempt to enable SO_REUSEPORT and see if we can keep going.\n" );

		//fprintf( stderr, "Is avahi running? If so, you can kill it with:\n" );
		//fprintf( stderr, " sudo systemctl disable avahi-daemon\n" );
		//fprintf( stderr, " sudo service avahi-daemon stop\n" );


		int optval = 1;
		if ( setsockopt( sd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof( optval ) ) != 0 )
		{
			fprintf( stderr, "Warning: Could not set SO_REUSEPORT\n" );
		}

		if ( bind( sd, (struct sockaddr *)&sinsock, sizeof( sinsock ) ) == -1 )
		{
			fprintf( stderr, "Fatal: Even after attempting SO_REUSEPORT, could not bind.  Aborting\n" );
			exit(-1);
		}
	}




	// Bind the normal MDNS socket (but with IPv6), that will get the messages.
	struct sockaddr_in6 sin6 = { 0 };
	sin6.sin6_family = AF_INET6;
	sin6.sin6_addr.s_addr6 = IN6ADDR_ANY_INIT;
	sin6.sin6_port = htons( MDNS_PORT );

	if ( bind( sd6, (struct sockaddr *)&sin6, sizeof(sin6) ) == -1 )
	{
		fprintf( stderr, "Warning: Could not bind to MDNS port\n" );
		fprintf( stderr, "You can figure out whose fault it is with:\n" );
		fprintf( stderr, " sudo netstat -nap | grep 5353\n" );
		fprintf( stderr, "But, we are going to attempt to enable SO_REUSEPORT and see if we can keep going.\n" );

		//fprintf( stderr, "Is avahi running? If so, you can kill it with:\n" );
		//fprintf( stderr, " sudo systemctl disable avahi-daemon\n" );
		//fprintf( stderr, " sudo service avahi-daemon stop\n" );


		int optval = 1;
		if ( setsockopt( sd6, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof( optval ) ) != 0 )
		{
			fprintf( stderr, "Warning: Could not set SO_REUSEPORT\n" );
		}

		if ( bind( sd6, (struct sockaddr *)&sin6, sizeof( sin6 ) ) == -1 )
		{
			fprintf( stderr, "Fatal: Even after attempting SO_REUSEPORT, could not bind.  Aborting\n" );
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
		struct pollfd fds[3] = {
			{ .fd = sd, .events = POLLIN | POLLHUP | POLLERR, .revents = 0 },
			{ .fd = sdifaceupdown, .events = POLLIN | POLLHUP | POLLERR, .revents = 0 },
			{ .fd = sd6, .events = POLLIN | POLLHUP | POLLERR, .revents = 0 },
		};

		int socks = ( sd6 ) ? 3 : 2;

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
				HandleRX( sd );
			}

			if( fds[0].revents & ( POLLHUP | POLLERR ) )
			{
				fprintf( stderr, "Fatal: IPv4 socket experienced fault.  Aborting\n" );
				return -14;
			}			
		}

		if ( fds[2].revents )
		{
			if ( fds[2].revents & POLLIN )
			{
				HandleRX( sd6 );
			}

			if( fds[2].revents & ( POLLHUP | POLLERR ) )
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
