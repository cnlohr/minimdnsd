# minimdns

Extremely simple MDNS server for Linux. Much smaller and lighter weight than Avahi.

⚠️ This has not been battle hardened, or even thoroughly checked ⚠️

Primarily MDNS Hostname responder - i.e. run this, and any computer on your network can say

`ping your_hostname.local`  and it will resolve to your PC.

 * Uses no CPU unless event requested.
 * Only needs around 32kB RAM.
 * Can run as suser or root.
 * Zero config + Watches for /etc/hostname changes.
 * Works on IPv6

## Building

### Prerequisits
 * build-essential (make + GCC + system headers)

### Build process
 * `make`

## Things I learned

IPv6 is pain.  this would have been a tiny fraction of its size if it weren't for IPv6.

Also IPv6 with MDNS is in a really sorry state, so I was barely able to test this with IPv6.

https://superuser.com/questions/1086954/how-do-i-use-mdns-for-ssh-6

And ping6 has a similar issue.

