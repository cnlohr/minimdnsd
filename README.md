# minimdnsd (MDNS server)

Extremely simple MDNS server for Linux. Much smaller and lighter weight than Avahi.

⚠️ This has not been battle hardened, or even thoroughly checked ⚠️

Primarily MDNS Hostname responder - i.e. run this, and any computer on your network can say `ping your_hostname.local`  and it will resolve to your PC. Specifically, it uses whatever name is in your `/etc/hostname`

 * Uses no CPU unless event requested.
 * Only needs around 32kB RAM.
 * Compiles to between 15-45kB
 * Can run as a user or root.
 * Zero config + Watches for `/etc/hostname` changes.
 * Works on IPv6

## General Motivation

1. To obviate need for avahi-daemon
2. To provide an MDNS server on very simple systems

### To demonstrate the following:

1. Use of inotify to detect changes of `/etc/hostname`
2. Use of `getifaddrs` to iterate through all available interfaces
3. Use of `NETLINK_ROUTE` and `RTMGRP_IPV4_IFADDR` and `RTMGRP_IPV6_IFADDR` to monitor for any new network interfaces or addresses.
4. Use of multicast in IPv4 and IPv6 to join a multicast group
5. Use of `recvmsg` to get the interface and address that a UDP packet is received on
 
## Building

### Prerequisits
 * build-essential (make + GCC + system headers)

### Build process
 * `make`
 * or, optionally `make install` to install it to /usr/local/bin/minimdnsd

## Things I learned

IPv6 is pain.  this would have been a tiny fraction of its size if it weren't for IPv6.

Also IPv6 with MDNS is in a really sorry state, so I was barely able to test this with IPv6.

https://superuser.com/questions/1086954/how-do-i-use-mdns-for-ssh-6

And ping6 has a similar issue.

