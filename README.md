# minimdns
Extremely simple MDNS server for Linux


## Reasons I hate avahi
 * Takes up ___ % CPU
 * Takes up ___ MB
 * It needs a chroot helper.
 * Default installation needs it's own whole user
 * It's ___ LOC
 * Hides that it's listening on port 5353
 * Difficult to disable
 * It doens't even follow the multicast rules, and binds to 0.0.0.0:5353 / :::5353

