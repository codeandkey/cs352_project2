## Project 2

Justin Stanley
COM S 352 Spring 2018
Project 2

## Compiling

To compile both the client and the server, execute `make` in this directory.

### Notes

The server will not respond to malformed requests. It requires root to execute as it makes use of a chroot jail to simplify file hierarchies and to improve server security.

The server uses forking instead of threading to simplify the source.

All times sent through If-Modified-Since headers to the server should be UTC.
