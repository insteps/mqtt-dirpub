mqtt-dirpub
===========

A mqtt subscriber-client that lets you to save/organize your data messages into filesystem directory.

This helps in organizing your subscribed data in neat hierarchy.

**mqtt-dirpub** is based on original `sub_client.c` that comes with `mosquitto`.

### Available mqtt-dirpub **new** options:

`--fmask <file-mask>`

**file-mask** - path to message outfile.

allowed masks are:

`@[epoch|date|year|month|day|datetime|hour|min|sec|id|topic[1-9]]`

**eg.**
`--fmask '/tmp/msgs/@year/@month@-@day/@topic/@id@-@hour@min'`
will create file: 
`/tmp/msgs/2010/10-29/topic/id-0540`

Note: **topic**/s *having hierarchy structure gets further resolved to directory.*

`--overwrite`

Works only with `--fmask`. This option starts client in overwrite mode.
*Caution: The existing data files get overwritten with every messages received.*

`--nodesuffix`

Works only with `--fmask`. This option provides file suffix for leaf/text nodes.


Dependencies
-------------
- libmosquitto

Your can also drop/replace `sub_client.c` file in `mosquitto-<ver>/client/` directory
to compile with parent package.

