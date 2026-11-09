# sengled-pulse-link

[English](README.md) | [Русский](README.ru.md) | **Українська** | [Polski](README.pl.md)

Реверс-інжиніринг шлюзу Sengled Pulse Link **C01-BR30** і ламп-колонок Sengled
Pulse.

Bluetooth-модуль працює прозорим мостом SPP ↔ UART: телефон відкриває
RFCOMM-сокет зі стандартним SPP UUID `00001101-0000-1000-8000-00805F9B34FB`,
і записані ним кадри без змін з'являються на UART. Сам модуль додає лише
текстові рядки стану на кшталт `\r\nSS=01,<BD-адреса>\r\n`, які, найімовірніше,
повідомляють про стан з'єднання.

## Адреса лампи

Кожна лампа має сталу 4-байтову адресу (AD), наприклад `00020056`. Вона
потрібна для команд окремій лампі та для прив'язки.

- **Нова лампа:** адресу записано в QR-коді на лампі. Відскануйте його будь-яким
  сканером QR: текст має вигляд `SN:14411008EU01926;AD:00020056`, адреса —
  частина після `AD:`. `main.py` і ESPHome приймають і саму адресу, і весь текст QR.
- **Лампа вже прив'язана:** `python3 main.py mode=nodes` або датчик
  *Gateway nodes* в ESPHome.

Щоб прив'язати нову лампу: увімкніть її поруч зі шлюзом, потім
`python3 main.py mode=add dst=<адреса>` або *Add lamp* в ESPHome (див. нижче).

## main.py

Збирання, надсилання, прослуховування й розшифрування кадрів. Аргументи
задаються парами `ключ=значення`.

```
pip install pyserial

python3 main.py help                     # усі ключі
python3 main.py help=commands            # усі команди

python3 main.py                                          # прослуховування /dev/ttyUSB0
python3 main.py mode=sniff port=/dev/ttyUSB0 raw=1       # прослуховування + сирий hex
python3 main.py mode=build cmd=lamp_off dst=all          # лише вивести кадр
python3 main.py mode=send port=/dev/rfcomm0 cmd=volume value=40 dst=all
python3 main.py mode=send cmd=lamp_state dst=00020056
python3 main.py mode=decode hex="7E 00 00 00 00 FE FF FF FF 00 00 01 00 01 FD 00 7E"
python3 main.py mode=nodes                               # лампи, відомі шлюзу
python3 main.py mode=add dst=00020056                    # прив'язати лампу, близько 20 с
python3 main.py mode=add dst="SN:14411008EU01926;AD:00020056"   # те саме, текст QR
python3 main.py mode=remove dst=0001FEB6                 # відв'язати лампу
```

| ключ | за замовчуванням | значення |
|---|---|---|
| `mode` | `sniff` | `sniff`, `send`, `build`, `decode`, `nodes`, `add`, `remove` |
| `port` | `/dev/ttyUSB0` | послідовний порт (UART-адаптер або `/dev/rfcomm0` через Bluetooth) |
| `baud` | `115200` | швидкість |
| `cmd` | | назва команди, див. `help=commands` |
| `dst` | `master` | `master`, `all` або адреса лампи з 8 hex-цифр (`00020056`) |
| `value`, `value2` | | параметри команди |
| `timeout` | `3` | скільки секунд чекати відповіді в режимі `send` |
| `raw` | `0` | `1` — виводити сирий hex кожного кадру |
| `hex` | | кадр для `mode=decode` |

Підключення до шлюзу через Bluetooth з Linux:

```
bluetoothctl pair <MAC>
sdptool browse <MAC>                  # знайти канал SPP
sudo rfcomm bind 0 <MAC> <канал>
python3 main.py mode=send port=/dev/rfcomm0 cmd=handshake
```

## ESPHome

`components/sengled_pulse` — зовнішній компонент ESPHome для ESP32,
підключеного до внутрішнього UART шлюзу. Мінімальний конфіг:

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
  lamp_slots: 2
```

Повний приклад — `esphome/sengled-pulse.yaml`.

### Параметри

| параметр | за замовчуванням | призначення |
|---|---|---|
| `id` | — | ID компонента, наприклад для лямбд: `id(pulse).auto_mute()` / `id(pulse).auto_unmute()` |
| `uart_id` | єдиний UART | UART, підключений до шлюзу, 115200 бод |
| `lamp_slots` | `8` | кількість слотів ламп, 1..8; не менше за кількість ламп, прив'язаних до шлюзу |
| `update_interval` | `30s` | як часто зчитується стан шлюзу й кожної лампи |
| `auto_mute` | — | глушити всі лампи й шлюз, поки медіаплеєр нічого не грає |
| `auto_mute: media_player` | — | обов'язково: ID медіаплеєра |
| `auto_mute: delay` | `3s` | скільки плеєр має мовчати, перш ніж усе заглушити |
| `slots` | — | налаштування сутностей слотів, список від слота 1 |
| ключі сутностей | — | налаштування сутностей, див. таблиці нижче |

Кожен ключ сутності приймає звичайні параметри сутності ESPHome (`name`,
`icon`, `internal`, `disabled_by_default`, `entity_category`, …). У select є
ще `options:` — перейменувати підписи; їхня кількість і порядок фіксовані,
порядок — значення протоколу.

Шлюз і всі лампи:

| ключ | тип | ім'я за замовчуванням | |
|---|---|---|---|
| `all_lamps` | light | All lamps | усі лампи; увімк, якщо увімкнена хоч одна, яскравість — середня по увімкнених |
| `volume` | number 0..100 | Volume | усі лампи, знімає mute з них і зі шлюзу; показує середню гучність ламп |
| `mute` | switch | Mute | усі лампи та шлюз; увімк, якщо заглушені всі |
| `equalizer` | select | Equalizer | один на всю систему (усі лампи та шлюз): normal, pop, jazz, classic, rock, movie |
| `gateway_mute` | switch | Gateway mute | власний аудіовихід шлюзу |
| `gateway_channel` | select | Gateway channel | власний аудіовихід шлюзу |
| `audio_source` | select | Audio source | 3.5mm, bluetooth |
| `light_memory` | switch | Light memory | пам'ять світла ламп (параметр 0) |
| `lamp_address` | text | Lamp address | адреса лампи для *Add lamp* / *Remove lamp* |
| `add_lamp` | button | Add lamp | прив'язати лампу, близько 20 с |
| `remove_lamp` | button | Remove lamp | відв'язати лампу |
| `refresh_nodes` | button | Refresh node list | знову прочитати список ламп шлюзу |
| `pairing_status` | text sensor | Pairing status | перебіг додавання / видалення |
| `gateway_type` | text sensor | Gateway type | ім'я та productID, наприклад `C01-BR30 Link (productID 2, audio box, no lamp)` |
| `gateway_nodes` | text sensor | Gateway nodes | лампи, відомі шлюзу |

Гучність шлюзу задати не можна: він запам'ятовує значення, але не застосовує
його.

Кожен слот лампи (ключі в `slots:`, N — номер слота):

| ключ | тип | ім'я за замовчуванням | |
|---|---|---|---|
| `light` | light | Lamp N | увімк/вимк і яскравість |
| `volume` | number 0..100 | Lamp N volume | знімає mute з лампи |
| `mute` | switch | Lamp N mute | |
| `channel` | select | Lamp N channel | right, left, left+right |
| `address` | text sensor | Lamp N address | лампа в слоті, `empty`, якщо жодної |
| `identify` | button | Identify lamp N | лампа блимає |

З усіма параметрами:

```yaml
sengled_pulse:
  id: pulse
  uart_id: pulse_uart
  lamp_slots: 2
  update_interval: 60s
  auto_mute:
    media_player: speaker_player
    delay: 3s
  audio_source:
    options: [3.5mm, network]   # лише підписи, порядок — значення протоколу
  light_memory:
    internal: true              # сховати сутність
  slots:
    - light: { name: Kitchen }  # слот 1
    - identify: { internal: true }
```

### Поведінка

- Зміна для всіх ламп одразу показується в усіх слотах і підтверджується
  наступним зчитуванням.
- Слоти прив'язуються до ламп під час роботи за списком вузлів шлюзу, адрес у
  конфігу немає. Нова лампа займає перший вільний слот, видалена звільняє
  свій, прив'язка зберігається у flash. Лампа без вільного слота пишеться в
  лог — збільште `lamp_slots`.
- Додавання лампи: увімкнути її поруч зі шлюзом, ввести адресу (або текст QR)
  у *Lamp address*, натиснути *Add lamp*, дочекатися `added` у
  *Pairing status*. Якщо додавання зірвалося, код мережі шлюзу
  відновлюється.
- `auto_mute`: заглушене вручну лишається заглушеним, коли звук повертається.

### Підключення

| ESP32 | шлюз |
|---|---|
| GPIO16 (RX) | лінія CPU TX → вивід 39 EH-MB05 (UART_RX) |
| GPIO17 (TX) | лінія CPU RX ← вивід 38 EH-MB05 (UART_TX) |
| GND | GND |

Лінію CPU RX тримає TX модуля: перед підключенням GPIO17 зніміть модуль або
переріжте доріжку від виводу 38.

## Формат кадру

```
зсув  розмір  поле
0     1       7E                 прапорець початку
1     4       адреса джерела, little-endian
5     4       адреса отримувача, little-endian
9     1       підсистема         ([10] = 00)
11    1       команда            ([12] = 00)
13    ..      дані
n-3   1       checksum = sum(bytes[1 .. n-4]) & 0xFF
n-2   1       00
n-1   1       7E                 прапорець кінця
```

Усередині кадру `7D` передається як `7D 5D`, а `7E` — як `7D 5E` (екранування
у стилі HDLC). Контрольна сума рахується до екранування.

### Адреси

| адреса (LE у кадрі) | значення | зміст |
|---|---|---|
| `FE FF FF FF` | `FFFFFFFE` | client: застосунок на телефоні / сторона Bluetooth |
| `00 00 00 00` | `00000000` | master: сам шлюз |
| `FF FF FF FF` | `FFFFFFFF` | розсилка всім вузлам |
| `56 00 02 00` | `00020056` | лампа (приклад) |

Застосунок спершу надсилає майстру `handshake` і `enum_slaves`, потім звертається
до кожного вузла за отриманою адресою або робить розсилку.

## Команди

`[n]` — зсув байта в кадрі. Невикористані байти дорівнюють `00`.

| підсист./ком. | назва | довжина | параметри |
|---|---|---|---|
| 0/0 | generic_info | 16 | відповідь: назва `C01-BR30 Link` з `[13]`; productID `[45..48]`: 1 одиночна лампа, 2 аудіобокс (Pulse Link, без лампи), решта — лампа |
| 0/1 | handshake | 16 | відповідь `[13]` = `01` — OK |
| 0/2 | enum_slaves | 16 | відповідь: кількість вузлів `[15..16]` (разом з майстром), адреси з `[17]` по 4 байти |
| 0/4 | set_name | 32 | `[13..25]` назва, до 13 байтів |
| 0/5 | node_name | 16 | запит назви |
| 0/7 | identify | 16 | «початок налаштування»: вибрана лампа блимає |
| 0/8 | config_end | 16 | |
| 0/11 | set_config | 20 | `[13]` параметр, `[15]` значення. Параметр 0 — пам'ять світла, 0 вимк / 1 увімк |
| 0/12 | query_config | 18 | `[13]` параметр; відповідь `[15]` значення |
| 1/0 | lamp_onoff | 20 | `[15]` 0 увімк, 1 вимк |
| 1/1 | brightness | 20 | `[15]` групове значення, `[16]` власне, 0..100, `FF` — не змінювати |
| 1/3 | lamp_state | 18 | відповідь `[15]` 0 увімк / 1 вимк, `[16]` яскравість 0..100 %; `245` — у вузла немає лампи (Pulse Link), `255 255` — донгл |
| 2/0 | channel | 19 | `[15]` 0 правий, 1 лівий, 2 лівий+правий |
| 2/1 | eq | 19 | `[15]` 0 normal, 1 pop, 2 jazz, 3 classic, 4 rock, 5 movie |
| 2/2 | mute | 19 | `[15]` 1 без звуку, 0 зі звуком |
| 2/3 | volume | 20 | `[15]` гучність 0..100, `[16]` = `FF`; на `all` змінює лише лампи, власний аудіовихід Pulse Link лишається на своєму рівні (при ≤ 5 він лише замовкає); на `master` шлюз запам'ятовує й повертає значення, але гучність його виходу не змінюється |
| 2/4 | speaker_state | 18 | відповідь `[15]` канал, `[16]` eq, `[17]` mute, `[18]` гучність % |
| 4/0 | connect_code | 16 | відповідь `[13..16]` код мережі = SN шлюзу (напр. `00096CC0`) |
| 4/1 | set_connect_code | 20 | `[13..16]` код для запису в лампу; `00FFFFFF` відв'язує її |
| 4/2 | set_id_address | 24 | таблиця вузлів шлюзу: `[13]` 0 додати / 1 видалити, `[17..20]` адреса лампи |
| 4/6 | audio_source | 19 | `[15]` 0 = 3,5 мм, 1 = Bluetooth |
| 4/7 | get_audio_source | 16 | відповідь `[13]` джерело |

У константах самого застосунку канал 0 названо «лівим», а 1 — «правим».
Інтерфейс ці константи не використовує, а в записі UART 0 = правий, 1 = лівий,
тому таблиця відповідає запису.

### Що робить застосунок

- **Яскравість усіх ламп:** поки рухається повзунок, надсилає `brightness` на
  `all` з `[15]` = значення, `[16]` = `FF`, потім `FF 64` («застосувати»),
  потім `lamp_onoff` увімк.
- **Яскравість однієї лампи:** надсилає на її адресу `[15]` = `FF`,
  `[16]` = значення.
- **Гучність:** `volume` на `all`, потім `mute` 0.
- **Додати лампу:** прочитати код шлюзу через `connect_code`, записати його
  в лампу через `set_connect_code`, зробити `handshake` лампі, додати її через
  `set_id_address` (`[13]` = 0), потім знову прочитати `enum_slaves`.
- **Видалити лампу:** `handshake` лампі, `set_connect_code` `00FFFFFF`,
  `set_id_address` (`[13]` = 1).

### Приклади

```
лампи вимк, усі     7E FE FF FF FF FF FF FF FF 01 00 00 00 00 00 01 00 F9 00 7E
гучність 59%, усі   7E FE FF FF FF FF FF FF FF 02 00 03 00 00 00 3B FF 36 00 7E
EQ movie, усі       7E FE FF FF FF FF FF FF FF 02 00 01 00 00 00 05 FF 00 7E
блимнути 00020056   7E FE FF FF FF 56 00 02 00 00 00 07 00 5A 00 7E
відповідь lamp_state 7E 56 00 02 00 FE FF FF FF 01 00 03 00 00 00 01 62 64 00 00 00 00 1E 00 7E
                    (вимк, 98 %)
```
