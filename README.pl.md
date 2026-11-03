# sengled-pulse-link

[English](README.md) | [Русский](README.ru.md) | [Українська](README.uk.md) | **Polski**

Inżynieria wsteczna bramki Sengled Pulse Link **C01-BR30** oraz żarówek
z głośnikiem Sengled Pulse.

Poniższy protokół odtworzono z dwóch źródeł, które zgadzają się co do bajtu:

- zapis analizatorem stanów logicznych UART między modułem Bluetooth
  (Ehong EH-MB05) a procesorem bramki, 115200 8N1;
- zdekompilowana aplikacja na Androida *Sengled Pulse* 4.2.1
  (`com.sengled.stspeaker`, pakiet `com.sengled.stspeaker.service.command`).

Moduł Bluetooth działa jako przezroczysty most SPP ↔ UART: telefon otwiera
gniazdo RFCOMM ze standardowym UUID SPP `00001101-0000-1000-8000-00805F9B34FB`,
a zapisane przez niego ramki pojawiają się na UART bez zmian. Sam moduł dodaje
jedynie tekstowe linie stanu w rodzaju `\r\nSS=01,<adres BD>\r\n`, które
najprawdopodobniej informują o stanie połączenia.

## main.py

Budowanie, wysyłanie, podsłuchiwanie i dekodowanie ramek. Argumenty podaje się
jako pary `klucz=wartość`.

```
pip install pyserial

python3 main.py help                     # wszystkie klucze
python3 main.py help=commands            # wszystkie polecenia

python3 main.py                                          # podsłuch /dev/ttyUSB0
python3 main.py mode=sniff port=/dev/ttyUSB0 raw=1       # podsłuch + surowy hex
python3 main.py mode=build cmd=lamp_off dst=all          # tylko wypisz ramkę
python3 main.py mode=send port=/dev/rfcomm0 cmd=volume value=40 dst=all
python3 main.py mode=send cmd=lamp_state dst=00020056
python3 main.py mode=decode hex="7E 00 00 00 00 FE FF FF FF 00 00 01 00 01 FD 00 7E"
```

| klucz | domyślnie | znaczenie |
|---|---|---|
| `mode` | `sniff` | `sniff`, `send`, `build`, `decode` |
| `port` | `/dev/ttyUSB0` | port szeregowy (adapter UART lub `/dev/rfcomm0` przez Bluetooth) |
| `baud` | `115200` | prędkość transmisji |
| `cmd` | | nazwa polecenia, zob. `help=commands` |
| `dst` | `master` | `master`, `all` lub adres żarówki jako 8 cyfr hex (`00020056`) |
| `value`, `value2` | | parametry polecenia |
| `timeout` | `3` | ile sekund czekać na odpowiedzi w trybie `send` |
| `raw` | `0` | `1` — wypisuj surowy hex każdej ramki |
| `hex` | | ramka dla `mode=decode` |

Połączenie z bramką przez Bluetooth z Linuksa:

```
bluetoothctl pair <MAC>
sdptool browse <MAC>                  # znajdź kanał SPP
sudo rfcomm bind 0 <MAC> <kanał>
python3 main.py mode=send port=/dev/rfcomm0 cmd=handshake
```

## Format ramki

```
przes.  rozmiar  pole
0       1        7E                 znacznik początku
1       4        adres nadawcy, little-endian
5       4        adres odbiorcy, little-endian
9       1        podsystem          ([10] = 00)
11      1        polecenie          ([12] = 00)
13      ..       dane
n-3     1        checksum = sum(bytes[1 .. n-4]) & 0xFF
n-2     1        00
n-1     1        7E                 znacznik końca
```

Wewnątrz ramki `7D` jest przesyłane jako `7D 5D`, a `7E` jako `7D 5E`
(escapowanie w stylu HDLC). Sumę kontrolną liczy się przed escapowaniem.

### Adresy

| adres (LE w ramce) | wartość | znaczenie |
|---|---|---|
| `FE FF FF FF` | `FFFFFFFE` | client: aplikacja na telefonie / strona Bluetooth |
| `00 00 00 00` | `00000000` | master: sama bramka |
| `FF FF FF FF` | `FFFFFFFF` | rozgłoszenie do wszystkich węzłów |
| `56 00 02 00` | `00020056` | żarówka (przykład) |

Aplikacja najpierw wysyła do mastera `handshake` i `enum_slaves`, a potem
zwraca się do każdego węzła pod otrzymanym adresem albo używa rozgłoszenia.

## Polecenia

`[n]` to przesunięcie bajtu w ramce. Nieużywane bajty mają wartość `00`.

| podsys./pol. | nazwa | dł. | parametry |
|---|---|---|---|
| 0/0 | generic_info | 16 | odpowiedź: nazwa `C01-BR30 Link` od `[13]`, dalej identyfikatory |
| 0/1 | handshake | 16 | odpowiedź `[13]` = `01` — OK |
| 0/2 | enum_slaves | 16 | odpowiedź: liczba węzłów `[15..16]` (łącznie z masterem), adresy od `[17]` po 4 bajty |
| 0/4 | set_name | 32 | `[13..25]` nazwa, do 13 bajtów |
| 0/5 | node_name | 16 | zapytanie o nazwę |
| 0/7 | identify | 16 | „początek konfiguracji”: wskazana żarówka miga |
| 0/8 | config_end | 16 | |
| 0/11 | set_config | 20 | `[13]` parametr, `[15]` wartość. Parametr 0 — pamięć światła, 0 wył. / 1 wł. |
| 0/12 | query_config | 18 | `[13]` parametr; odpowiedź `[15]` wartość |
| 1/0 | lamp_onoff | 20 | `[15]` 0 wł., 1 wył. |
| 1/1 | brightness | 20 | `[15]` wartość grupowa, `[16]` własna, 0..100, `FF` — bez zmian |
| 1/3 | lamp_state | 18 | odpowiedź `[15]` 0 wł. / 1 wył., `[16]` jasność % |
| 2/0 | channel | 19 | `[15]` 0 prawy, 1 lewy, 2 lewy+prawy |
| 2/1 | eq | 19 | `[15]` 0 normal, 1 pop, 2 jazz, 3 classic, 4 rock, 5 movie |
| 2/2 | mute | 19 | `[15]` 1 wyciszenie, 0 dźwięk |
| 2/3 | volume | 20 | `[15]` głośność 0..100, `[16]` = `FF` |
| 2/4 | speaker_state | 18 | odpowiedź `[15]` kanał, `[16]` eq, `[17]` mute, `[18]` głośność % |
| 4/0 | connect_code | 16 | odpowiedź `[13..16]` kod sieci = SN bramki (np. `00096CC0`) |
| 4/1 | set_connect_code | 20 | `[13..16]` kod do zapisania w żarówce; `00FFFFFF` ją odłącza |
| 4/2 | set_id_address | 24 | tablica węzłów bramki: `[13]` 0 dodaj / 1 usuń, `[17..20]` adres żarówki |
| 4/6 | audio_source | 19 | `[15]` 0 = 3,5 mm, 1 = Bluetooth |
| 4/7 | get_audio_source | 16 | odpowiedź `[13]` źródło |

W stałych samej aplikacji kanał 0 nazywa się „lewy”, a 1 „prawy”. Interfejs
z nich nie korzysta, a w zapisie UART 0 = prawy, 1 = lewy, więc tabela
podąża za zapisem.

### Co robi aplikacja

- **Jasność wszystkich żarówek:** podczas przesuwania suwaka wysyła
  `brightness` do `all` z `[15]` = wartość, `[16]` = `FF`, następnie `FF 64`
  („zastosuj”), a potem `lamp_onoff` wł.
- **Jasność jednej żarówki:** wysyła na jej adres `[15]` = `FF`,
  `[16]` = wartość.
- **Głośność:** `volume` do `all`, potem `mute` 0.
- **Dodanie żarówki:** odczytać kod bramki przez `connect_code`, zapisać go
  w żarówce przez `set_connect_code`, wykonać `handshake` z żarówką, dodać ją
  przez `set_id_address` (`[13]` = 0), po czym ponownie odczytać `enum_slaves`.
- **Usunięcie żarówki:** `handshake` z żarówką, `set_connect_code` `00FFFFFF`,
  `set_id_address` (`[13]` = 1).

### Przykłady

```
żarówki wył., wszystkie   7E FE FF FF FF FF FF FF FF 01 00 00 00 00 00 01 00 F9 00 7E
głośność 59%, wszystkie   7E FE FF FF FF FF FF FF FF 02 00 03 00 00 00 3B FF 36 00 7E
EQ movie, wszystkie       7E FE FF FF FF FF FF FF FF 02 00 01 00 00 00 05 FF 00 7E
mignij 00020056           7E FE FF FF FF 56 00 02 00 00 00 07 00 5A 00 7E
odpowiedź lamp_state      7E 56 00 02 00 FE FF FF FF 01 00 03 00 00 00 01 62 64 00 00 00 00 1E 00 7E
                          (wył., 98 %)
```
