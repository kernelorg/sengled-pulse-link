# sengled-pulse-link

**English** | [Русский](README.ru.md) | [Українська](README.uk.md) | [Polski](README.pl.md)

Reverse engineering of the Sengled Pulse Link **C01-BR30** gateway and Pulse
speaker bulbs.

The protocol below was recovered from two sources that agree byte for byte:

- a logic-analyzer capture of the UART between the Bluetooth module
  (Ehong EH-MB05) and the gateway CPU, 115200 8N1;
- the decompiled Android app *Sengled Pulse* 4.2.1 (`com.sengled.stspeaker`,
  package `com.sengled.stspeaker.service.command`).

The Bluetooth module is a transparent SPP ↔ UART bridge: the phone opens an
RFCOMM socket with the standard SPP UUID `00001101-0000-1000-8000-00805F9B34FB`,
and the frames it writes appear unchanged on the UART. The module itself only
adds text status lines such as `\r\nSS=01,<BD address>\r\n`, which most
likely report the connection state.

## Lamp address

Every lamp has a fixed 4-byte address (AD), e.g. `00020056`. Commands for one
lamp and pairing use it.

- **New lamp:** the address is in the QR code on the lamp. Scan it with any QR
  reader; the text looks like `SN:14411008EU01926;AD:00020056`, and the address
  is the part after `AD:`. The phone app reads the same QR code when you add a
  lamp. `main.py` and ESPHome accept either the address or the whole QR text.
- **Lamp already paired:** `python3 main.py mode=nodes`, or the *Gateway nodes*
  sensor in ESPHome.

To pair a new lamp: power it on near the gateway, then
`python3 main.py mode=add dst=<address>` or *Add lamp* in ESPHome (see below).

## main.py

Build, send, sniff and decode frames. Arguments are `key=value` pairs.

```
pip install pyserial

python3 main.py help                     # all keys
python3 main.py help=commands            # all commands

python3 main.py                                          # sniff /dev/ttyUSB0
python3 main.py mode=sniff port=/dev/ttyUSB0 raw=1       # sniff, also print raw hex
python3 main.py mode=build cmd=lamp_off dst=all          # print frame only
python3 main.py mode=send port=/dev/rfcomm0 cmd=volume value=40 dst=all
python3 main.py mode=send cmd=lamp_state dst=00020056
python3 main.py mode=decode hex="7E 00 00 00 00 FE FF FF FF 00 00 01 00 01 FD 00 7E"
python3 main.py mode=nodes                               # lamps known to the gateway
python3 main.py mode=add dst=00020056                    # pair a lamp, about 20 s
python3 main.py mode=add dst="SN:14411008EU01926;AD:00020056"   # the same, QR text
python3 main.py mode=remove dst=0001FEB6                 # unpair a lamp
```

| key | default | meaning |
|---|---|---|
| `mode` | `sniff` | `sniff`, `send`, `build`, `decode`, `nodes`, `add`, `remove` |
| `port` | `/dev/ttyUSB0` | serial port (UART adapter, or `/dev/rfcomm0` over Bluetooth) |
| `baud` | `115200` | baud rate |
| `cmd` | | command name, see `help=commands` |
| `dst` | `master` | `master`, `all`, or a lamp address as 8 hex digits (`00020056`) |
| `value`, `value2` | | command parameters |
| `timeout` | `3` | seconds to wait for replies in `send` mode |
| `raw` | `0` | `1` prints raw hex of each frame |
| `hex` | | frame for `mode=decode` |

To talk to the gateway over Bluetooth from Linux:

```
bluetoothctl pair <MAC>
sdptool browse <MAC>                  # find the SPP channel
sudo rfcomm bind 0 <MAC> <channel>
python3 main.py mode=send port=/dev/rfcomm0 cmd=handshake
```

## ESPHome

`esphome/` contains an ESPHome node that connects an ESP32 to the gateway's
internal UART and exposes it to Home Assistant:

- lights: *All lamps*, *Gateway lamp* (the gateway's own lamp; useless if
  *Gateway type* says `audio box`, e.g. a Pulse Link) and 8 lamp
  slots *Lamp 1* … *Lamp 8* (the gateway supports up to 8 lamps); each slot
  also has its own *volume*, *mute* and *channel*, read back from the lamp;
- volume, mute, equalizer, channel, audio source, light memory for all lamps;
- *Gateway type*: gateway name and productID from `generic_info`, e.g.
  `C01-BR30 Link (productID 2, audio box, no lamp)`;
- "identify" buttons (the lamp blinks), lamp on/brightness sensors;
- adding and removing lamps: enter the lamp address (`00020056`, or the QR
  text `...;AD:00020056`) into *Lamp address* and press *Add lamp* /
  *Remove lamp*. The procedure is the same as in the app and takes about
  20 s; progress is shown in *Pairing status*, the gateway's lamp list in
  *Gateway nodes*. If anything fails while adding, the gateway network code
  is restored.

Lamp slots are bound to lamps at run time, there are no addresses in the
config and no reflashing when lamps change:

- after boot the slots are filled from the gateway's node list;
- a lamp added with *Add lamp* takes the first free slot, a removed lamp frees
  its slot;
- the binding is stored in flash, so a lamp keeps its slot across reboots;
- *Lamp N address* shows which lamp is in a slot (`empty` if none). Rename the
  entities in Home Assistant as you like and hide unused slots.

A freed slot is reused by the next new lamp, so after removing a lamp check
the names you gave to that slot.

To add a lamp: power it on near the gateway, put its address (or the QR text)
into *Lamp address*, press *Add lamp*, wait until *Pairing status* says
`added`.

State is read back from the gateway at boot and every 30 s, so changes made
from the phone app show up too. The light entities are optimistic.

| ESP32 | gateway |
|---|---|
| GPIO16 (RX) | CPU TX line → EH-MB05 pin 39 (UART_RX) |
| GPIO17 (TX) | CPU RX line ← EH-MB05 pin 38 (UART_TX) |
| GND | GND |

The CPU RX line is already driven by the module's TX, so do not connect the
ESP TX to it directly. Either cut the trace from pin 38 (the phone app stops
working), or join both transmitters with a diode wired-AND: cut the trace,
connect pin 38 and GPIO17 each through a Schottky diode (cathode towards the
transmitter) to the CPU RX line, with a 4.7–10 kΩ pull-up to 3.3 V.

> **Warning:** the C01-BR30 is a BR30 bulb with a mains power supply inside,
> most likely non-isolated. Do not touch the board while it is plugged in, and
> do not connect the ESP to a PC over USB while the bulb is on mains. Power the
> ESP32 from a separate isolated supply.

```
cd esphome
# create secrets.yaml with api_key, ota_password, wifi_ssid, wifi_password
esphome run sengled-pulse.yaml
```

## Frame format

```
offset  size  field
0       1     7E                 start flag
1       4     source address, little-endian
5       4     destination address, little-endian
9       1     subsystem          ([10] = 00)
11      1     command            ([12] = 00)
13      ..    payload
n-3     1     checksum = sum(bytes[1 .. n-4]) & 0xFF
n-2     1     00
n-1     1     7E                 end flag
```

Inside a frame, `7D` is sent as `7D 5D` and `7E` as `7D 5E` (HDLC-style
escaping). The checksum is calculated before escaping.

### Addresses

| address (LE on the wire) | value | meaning |
|---|---|---|
| `FE FF FF FF` | `FFFFFFFE` | client: the phone app / Bluetooth side |
| `00 00 00 00` | `00000000` | master: the gateway itself |
| `FF FF FF FF` | `FFFFFFFF` | broadcast to all nodes |
| `56 00 02 00` | `00020056` | a lamp (example) |

The app first sends `handshake` and `enum_slaves` to the master, then addresses
each node by the address it got back, or uses broadcast.

## Commands

`[n]` is the byte offset in the frame. Unused bytes are `00`.

| sub/cmd | name | len | parameters |
|---|---|---|---|
| 0/0 | generic_info | 16 | reply: name `C01-BR30 Link` from `[13]`; productID `[45..48]`: 1 single light, 2 audio box (Pulse Link, no lamp), others a lamp |
| 0/1 | handshake | 16 | reply `[13]` = `01` ok |
| 0/2 | enum_slaves | 16 | reply: count `[15..16]` (including master), addresses from `[17]`, 4 bytes each |
| 0/4 | set_name | 32 | `[13..25]` name, up to 13 bytes |
| 0/5 | node_name | 16 | query name |
| 0/7 | identify | 16 | "config start": the addressed lamp blinks |
| 0/8 | config_end | 16 | |
| 0/11 | set_config | 20 | `[13]` item, `[15]` value. Item 0 = light memory, 0 off / 1 on |
| 0/12 | query_config | 18 | `[13]` item; reply `[15]` value |
| 1/0 | lamp_onoff | 20 | `[15]` 0 on, 1 off |
| 1/1 | brightness | 20 | `[15]` group value, `[16]` own value, 0..100, `FF` = keep |
| 1/3 | lamp_state | 18 | reply `[15]` 0 on / 1 off, `[16]` brightness 0..100 %; `245` = node has no lamp (Pulse Link), `255 255` = dongle |
| 2/0 | channel | 19 | `[15]` 0 right, 1 left, 2 left+right |
| 2/1 | eq | 19 | `[15]` 0 normal, 1 pop, 2 jazz, 3 classic, 4 rock, 5 movie |
| 2/2 | mute | 19 | `[15]` 1 mute, 0 unmute |
| 2/3 | volume | 20 | `[15]` volume 0..100, `[16]` = `FF` |
| 2/4 | speaker_state | 18 | reply `[15]` channel, `[16]` eq, `[17]` mute, `[18]` volume % |
| 4/0 | connect_code | 16 | reply `[13..16]` network code = gateway SN (e.g. `00096CC0`) |
| 4/1 | set_connect_code | 20 | `[13..16]` code to write into a lamp; `00FFFFFF` unbinds it |
| 4/2 | set_id_address | 24 | gateway node table: `[13]` 0 add / 1 delete, `[17..20]` lamp address |
| 4/6 | audio_source | 19 | `[15]` 0 = 3.5 mm, 1 = Bluetooth |
| 4/7 | get_audio_source | 16 | reply `[13]` source |

The app's own constants name channel 0 "left" and 1 "right". The UI does not
use them, and the capture shows 0 = right, 1 = left, so the table follows the
capture.

### What the app does

- **Brightness, all lamps:** sends `brightness` to `all` with `[15]` = value,
  `[16]` = `FF` while the slider moves, then `FF 64` ("apply"), then
  `lamp_onoff` on.
- **Brightness, one lamp:** sends `[15]` = `FF`, `[16]` = value to its address.
- **Volume:** `volume` to `all`, then `mute` 0.
- **Add a lamp:** read the gateway code with `connect_code`, write it into the
  lamp with `set_connect_code`, `handshake` the lamp, add it with
  `set_id_address` (`[13]` = 0), then re-read `enum_slaves`.
- **Delete a lamp:** `handshake` the lamp, `set_connect_code` `00FFFFFF`,
  `set_id_address` (`[13]` = 1).

### Examples

```
lamp off, all     7E FE FF FF FF FF FF FF FF 01 00 00 00 00 00 01 00 F9 00 7E
volume 59%, all   7E FE FF FF FF FF FF FF FF 02 00 03 00 00 00 3B FF 36 00 7E
EQ movie, all     7E FE FF FF FF FF FF FF FF 02 00 01 00 00 00 05 FF 00 7E
blink 00020056    7E FE FF FF FF 56 00 02 00 00 00 07 00 5A 00 7E
lamp state reply  7E 56 00 02 00 FE FF FF FF 01 00 03 00 00 00 01 62 64 00 00 00 00 1E 00 7E
                  (off, 98 %)
```
