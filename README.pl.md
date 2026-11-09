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

## Adres żarówki

Każda żarówka ma stały 4-bajtowy adres (AD), np. `00020056`. Jest potrzebny
do poleceń dla pojedynczej żarówki i do parowania.

- **Nowa żarówka:** adres jest zapisany w kodzie QR na żarówce. Zeskanuj go
  dowolnym czytnikiem QR: tekst wygląda jak `SN:14411008EU01926;AD:00020056`,
  a adres to część po `AD:`. Aplikacja na telefonie przy dodawaniu żarówki
  odczytuje ten sam kod QR. `main.py` i ESPHome przyjmują zarówno sam adres,
  jak i cały tekst QR.
- **Żarówka już sparowana:** `python3 main.py mode=nodes` lub czujnik
  *Gateway nodes* w ESPHome.

Aby sparować nową żarówkę: włącz ją blisko bramki, potem
`python3 main.py mode=add dst=<adres>` albo *Add lamp* w ESPHome (zob. niżej).

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
python3 main.py mode=nodes                               # żarówki znane bramce
python3 main.py mode=add dst=00020056                    # sparuj żarówkę, ok. 20 s
python3 main.py mode=add dst="SN:14411008EU01926;AD:00020056"   # to samo, tekst QR
python3 main.py mode=remove dst=0001FEB6                 # rozparuj żarówkę
```

| klucz | domyślnie | znaczenie |
|---|---|---|
| `mode` | `sniff` | `sniff`, `send`, `build`, `decode`, `nodes`, `add`, `remove` |
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

## ESPHome

`components/sengled_pulse` to zewnętrzny komponent ESPHome: ESP32 podłączony
do wewnętrznego UART bramki udostępnia ją w Home Assistant. Jeden blok w
konfiguracji tworzy wszystkie encje, nie trzeba kopiować lambd:

```yaml
external_components:
  - source: github://kernelorg/sengled-pulse-link
    components: [sengled_pulse]

uart:
  id: pulse_uart
  tx_pin: GPIO17
  rx_pin: GPIO16
  baud_rate: 115200

sengled_pulse:
  uart_id: pulse_uart
  lamp_slots: 2          # ile żarówek pokazać, 1..8
  auto_mute:             # opcjonalnie
    media_player: speaker_player
    delay: 3s
```

Pełny przykład: `esphome/sengled-pulse.yaml`. Encje:

- światło: *All lamps* i sloty żarówek *Lamp 1*, *Lamp 2*, …; każdy slot ma
  też własne *volume*, *mute*, *channel*, *address* i przycisk *identify*
  (żarówka miga); wł./wył., jasność i dźwięk są odczytywane z żarówki;
- *Volume*, *Mute*, *Equalizer*, *Channel*, *Light memory*: wszystkie żarówki;
- *Gateway mute*, *Gateway equalizer*, *Gateway channel*: własne wyjście audio
  bramki, osobno od żarówek (głośności bramki nie da się ustawić: bramka
  zapamiętuje wartość, ale jej nie stosuje, aplikacja też jej nie ustawia);
  *Audio source*;
- *Gateway type*: nazwa bramki i productID z `generic_info`, np.
  `C01-BR30 Link (productID 2, audio box, no lamp)`;
- dodawanie i usuwanie żarówek: adres żarówki (`00020056` albo tekst QR
  `...;AD:00020056`) w *Lamp address*, potem *Add lamp* / *Remove lamp*.
  Procedura jest taka sama jak w aplikacji i trwa około 20 s; postęp widać w
  *Pairing status*, listę żarówek bramki w *Gateway nodes*. Jeśli dodawanie
  się nie uda, kod sieci bramki zostaje przywrócony.

Każdy klucz encji (`all_lamps`, `volume`, `gateway_mute`, `audio_source`, …)
przyjmuje zwykłe opcje encji, by zmienić jej nazwę lub ją ukryć; ustawienia
slotów są w `slots:`, np. `slots: [{light: {name: Kitchen}}]`. Etykiety opcji
select można zmienić przez `options:` (kolejność = wartość protokołu).

`auto_mute` wycisza wszystkie żarówki i bramkę, gdy podany odtwarzacz nic nie
gra dłużej niż `delay`, i wyłącza wyciszenie, gdy dźwięk wraca; głośniki
wyciszone ręcznie zostają wyciszone. Z lambdy: `id(pulse).auto_mute()` /
`id(pulse).auto_unmute()`.

Informacja zwrotna:

- stan jest odczytywany z bramki i żarówek przy starcie i co 30 s, więc zmiany
  z aplikacji też są widoczne;
- zmiana dla wszystkich żarówek (światło, głośność, wyciszenie, kanał,
  korektor) od razu pokazuje się we wszystkich slotach i jest potwierdzana
  następnym odczytem;
- *All lamps* pokazuje łączny stan żarówek jak grupa świateł w Home Assistant
  (wł., gdy świeci choć jedna, jasność = średnia włączonych); *Volume* to
  średnia głośność, *Mute* jest wł., gdy wyciszone są wszystkie żarówki,
  *Channel* / *Equalizer* to wspólna wartość żarówek.

Sloty są przypisywane do żarówek w trakcie pracy, w konfiguracji nie ma
adresów i nie trzeba wgrywać firmware'u przy zmianie żarówek:

- po starcie sloty są wypełniane z listy węzłów bramki;
- żarówka dodana przez *Add lamp* zajmuje pierwszy wolny slot, usunięta
  zwalnia swój; żarówka bez wolnego slotu jest zapisywana w logu — zwiększ
  `lamp_slots`;
- przypisanie jest zapisane we flashu, więc po restarcie żarówka zostaje w
  swoim slocie;
- *Lamp N address* pokazuje, która żarówka jest w slocie (`empty`, jeśli
  żadna).

Zwolniony slot zajmie następna nowa żarówka, więc po usunięciu żarówki
sprawdź nazwy nadane temu slotowi. Po zmniejszeniu `lamp_slots` usuń w Home
Assistant pozostałe encje, jeśli wiszą jako „niedostępne”.

| ESP32 | bramka |
|---|---|
| GPIO16 (RX) | linia CPU TX → wyprowadzenie 39 EH-MB05 (UART_RX) |
| GPIO17 (TX) | linia CPU RX ← wyprowadzenie 38 EH-MB05 (UART_TX) |
| GND | GND |

Linię CPU RX steruje już TX modułu, więc TX ESP nie wolno podłączać do niej
bezpośrednio. Można albo przeciąć ścieżkę od wyprowadzenia 38 (aplikacja na
telefonie przestanie działać), albo połączyć oba nadajniki diodowym AND:
przeciąć ścieżkę, podłączyć wyprowadzenie 38 i GPIO17 do linii CPU RX, każde
przez własną diodę Schottky'ego (katodą do nadajnika), plus rezystor
podciągający 4,7–10 kΩ do 3,3 V.

> **Uwaga:** C01-BR30 to żarówka BR30 z sieciowym zasilaczem w środku,
> najprawdopodobniej bez izolacji galwanicznej. Nie dotykaj płytki, gdy
> żarówka jest podłączona do sieci, i nie podłączaj ESP do komputera przez USB,
> gdy żarówka jest zasilana z sieci. ESP32 zasilaj z osobnego izolowanego
> źródła.

```
cd esphome
# utwórz secrets.yaml: api_key, ota_password, wifi_ssid, wifi_password
esphome run sengled-pulse.yaml
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
| 0/0 | generic_info | 16 | odpowiedź: nazwa `C01-BR30 Link` od `[13]`; productID `[45..48]`: 1 pojedyncza żarówka, 2 audio box (Pulse Link, bez żarówki), pozostałe — żarówka |
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
| 1/3 | lamp_state | 18 | odpowiedź `[15]` 0 wł. / 1 wył., `[16]` jasność 0..100 %; `245` — węzeł nie ma żarówki (Pulse Link), `255 255` — dongle |
| 2/0 | channel | 19 | `[15]` 0 prawy, 1 lewy, 2 lewy+prawy |
| 2/1 | eq | 19 | `[15]` 0 normal, 1 pop, 2 jazz, 3 classic, 4 rock, 5 movie |
| 2/2 | mute | 19 | `[15]` 1 wyciszenie, 0 dźwięk |
| 2/3 | volume | 20 | `[15]` głośność 0..100, `[16]` = `FF`; do `all` zmienia tylko żarówki, własne wyjście audio Pulse Link zostaje na swoim poziomie (przy ≤ 5 tylko milknie); do `master` bramka zapamiętuje i zwraca wartość, ale głośność jej wyjścia się nie zmienia |
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
