mqtt-dirpub
===========

A mqtt subscriber-client that lets you to save/organize your data messages into filesystem directory.

This helps in organizing your subscribed data in neat hierarchy.

**mqtt-dirpub** is based on original `sub_client.c` that comes with `mosquitto`.

### Available mqtt-dirpub **new** options:

`--fmask <file-mask>`

**path to message outfile**
    allowed masks are:

`@[epoch|date|year|month|day|datetime|hour|min|sec|id|topic[1-9]]`

**eg.** 
`--fmask '/tmp/msgs/@id@date@topic'`
will create file: 
`/tmp/msgs/id-2010-12-21-topicname`

**topic** having hierarchy structure gets further resolved to directory.



Dependencies
-------------
- libmosquitto

Your can also drop/replace `sub_client.c` file in `mosquitto-<ver>/client/` directory
to compile with parent package.

