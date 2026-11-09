# sengled-pulse-link

[English](README.md) | **Русский** | [Українська](README.uk.md) | [Polski](README.pl.md)

Реверс-инжиниринг шлюза Sengled Pulse Link **C01-BR30** и ламп-колонок Sengled
Pulse.

Bluetooth-модуль работает прозрачным мостом SPP ↔ UART: телефон открывает
RFCOMM-сокет со стандартным SPP UUID `00001101-0000-1000-8000-00805F9B34FB`,
и записанные им кадры без изменений появляются на UART. Сам модуль добавляет
только текстовые строки состояния вида `\r\nSS=01,<BD-адрес>\r\n`, которые,
скорее всего, сообщают о состоянии соединения.

## Адрес лампы

У каждой лампы есть постоянный 4-байтовый адрес (AD), например `00020056`.
Он нужен для команд отдельной лампе и для привязки.

- **Новая лампа:** адрес записан в QR-коде на лампе. Отсканируйте его любым
  сканером QR: текст выглядит как `SN:14411008EU01926;AD:00020056`, адрес —
  часть после `AD:`. `main.py` и ESPHome принимают и сам адрес, и весь текст QR.
- **Лампа уже привязана:** `python3 main.py mode=nodes` или датчик
  *Gateway nodes* в ESPHome.

Чтобы привязать новую лампу: включите её рядом со шлюзом, затем
`python3 main.py mode=add dst=<адрес>` или *Add lamp* в ESPHome (см. ниже).

## main.py

Сборка, отправка, прослушивание и расшифровка кадров. Аргументы задаются
парами `ключ=значение`.

```
pip install pyserial

python3 main.py help                     # все ключи
python3 main.py help=commands            # все команды

python3 main.py                                          # прослушивание /dev/ttyUSB0
python3 main.py mode=sniff port=/dev/ttyUSB0 raw=1       # прослушивание + сырой hex
python3 main.py mode=build cmd=lamp_off dst=all          # только вывести кадр
python3 main.py mode=send port=/dev/rfcomm0 cmd=volume value=40 dst=all
python3 main.py mode=send cmd=lamp_state dst=00020056
python3 main.py mode=decode hex="7E 00 00 00 00 FE FF FF FF 00 00 01 00 01 FD 00 7E"
python3 main.py mode=nodes                               # лампы, известные шлюзу
python3 main.py mode=add dst=00020056                    # привязать лампу, около 20 с
python3 main.py mode=add dst="SN:14411008EU01926;AD:00020056"   # то же, текст QR
python3 main.py mode=remove dst=0001FEB6                 # отвязать лампу
```

| ключ | по умолчанию | значение |
|---|---|---|
| `mode` | `sniff` | `sniff`, `send`, `build`, `decode`, `nodes`, `add`, `remove` |
| `port` | `/dev/ttyUSB0` | последовательный порт (UART-адаптер или `/dev/rfcomm0` по Bluetooth) |
| `baud` | `115200` | скорость |
| `cmd` | | имя команды, см. `help=commands` |
| `dst` | `master` | `master`, `all` или адрес лампы из 8 hex-цифр (`00020056`) |
| `value`, `value2` | | параметры команды |
| `timeout` | `3` | сколько секунд ждать ответы в режиме `send` |
| `raw` | `0` | `1` — выводить сырой hex каждого кадра |
| `hex` | | кадр для `mode=decode` |

Подключение к шлюзу по Bluetooth из Linux:

```
bluetoothctl pair <MAC>
sdptool browse <MAC>                  # найти канал SPP
sudo rfcomm bind 0 <MAC> <канал>
python3 main.py mode=send port=/dev/rfcomm0 cmd=handshake
```

## ESPHome

`components/sengled_pulse` — внешний компонент ESPHome для ESP32, подключённого
к внутреннему UART шлюза. Минимальный конфиг:

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

Полный пример — `esphome/sengled-pulse.yaml`.

### Параметры

| параметр | по умолчанию | назначение |
|---|---|---|
| `id` | — | ID компонента, например для лямбд: `id(pulse).auto_mute()` / `id(pulse).auto_unmute()` |
| `uart_id` | единственный UART | UART, подключённый к шлюзу, 115200 бод |
| `lamp_slots` | `8` | число слотов ламп, 1..8; не меньше числа ламп, привязанных к шлюзу |
| `update_interval` | `30s` | как часто считывается состояние шлюза и каждой лампы |
| `auto_mute` | — | глушить все лампы и шлюз, пока медиаплеер ничего не играет |
| `auto_mute: media_player` | — | обязательно: ID медиаплеера |
| `auto_mute: delay` | `3s` | сколько плеер должен молчать, прежде чем всё заглушить |
| `slots` | — | настройки сущностей слотов, список начиная со слота 1 |
| ключи сущностей | — | настройки сущностей, см. таблицы ниже |

Каждый ключ сущности принимает обычные параметры сущности ESPHome (`name`,
`icon`, `internal`, `disabled_by_default`, `entity_category`, …). У select
есть ещё `options:` — переименовать подписи; их число и порядок фиксированы,
порядок — значение протокола.

Шлюз и все лампы:

| ключ | тип | имя по умолчанию | |
|---|---|---|---|
| `all_lamps` | light | All lamps | все лампы; вкл, если включена хоть одна, яркость — средняя по включённым |
| `volume` | number 0..100 | Volume | все лампы, снимает mute с них и со шлюза; показывает среднюю громкость ламп |
| `mute` | switch | Mute | все лампы и шлюз; вкл, если заглушены все |
| `equalizer` | select | Equalizer | один на всю систему (все лампы и шлюз): normal, pop, jazz, classic, rock, movie |
| `gateway_mute` | switch | Gateway mute | собственный аудиовыход шлюза |
| `gateway_channel` | select | Gateway channel | собственный аудиовыход шлюза |
| `audio_source` | select | Audio source | 3.5mm, bluetooth |
| `light_memory` | switch | Light memory | память света ламп (параметр 0) |
| `lamp_address` | text | Lamp address | адрес лампы для *Add lamp* / *Remove lamp* |
| `add_lamp` | button | Add lamp | привязать лампу, около 20 с |
| `remove_lamp` | button | Remove lamp | отвязать лампу |
| `refresh_nodes` | button | Refresh node list | заново прочитать список ламп шлюза |
| `pairing_status` | text sensor | Pairing status | ход добавления / удаления |
| `gateway_type` | text sensor | Gateway type | имя и productID, например `C01-BR30 Link (productID 2, audio box, no lamp)` |
| `gateway_nodes` | text sensor | Gateway nodes | лампы, известные шлюзу |

Громкость шлюза задать нельзя: он запоминает значение, но не применяет его.

Каждый слот лампы (ключи в `slots:`, N — номер слота):

| ключ | тип | имя по умолчанию | |
|---|---|---|---|
| `light` | light | Lamp N | вкл/выкл и яркость |
| `volume` | number 0..100 | Lamp N volume | снимает mute с лампы |
| `mute` | switch | Lamp N mute | |
| `channel` | select | Lamp N channel | right, left, left+right |
| `address` | text sensor | Lamp N address | лампа в слоте, `empty`, если никакой |
| `identify` | button | Identify lamp N | лампа мигает |

Со всеми параметрами:

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
    options: [3.5mm, network]   # только подписи, порядок — значение протокола
  light_memory:
    internal: true              # скрыть сущность
  slots:
    - light: { name: Kitchen }  # слот 1
    - identify: { internal: true }
```

### Автоматизации

У каждой сущности есть обычные триггеры ESPHome: `on_value` (select,
number), `on_turn_on` / `on_turn_off` (switch, light) и т. д. В `on_value` у
select `x` — подпись варианта, `i` — его номер. Они срабатывают только при
изменении значения, в том числе при смене на самом шлюзе, а не при каждом
чтении:

```yaml
sengled_pulse:
  audio_source:
    on_value:
      then:
        - logger.log:
            level: INFO
            format: "Audio source: %s"
            args: [x.c_str()]
        - if:
            condition:
              lambda: return i == 1;   # second option
            then:
              - media_player.stop: speaker_player
```

### Поведение

- Изменение для всех ламп сразу показывается во всех слотах и подтверждается
  следующим чтением.
- Слоты привязываются к лампам во время работы по списку узлов шлюза, адресов
  в конфиге нет. Новая лампа занимает первый свободный слот, удалённая
  освобождает свой, привязка хранится во flash. Лампа без свободного слота
  пишется в лог — увеличьте `lamp_slots`.
- Добавление лампы: включить её рядом со шлюзом, ввести адрес (или текст QR) в
  *Lamp address*, нажать *Add lamp*, дождаться `added` в *Pairing status*. Если
  добавление сорвалось, код сети шлюза восстанавливается.
- `auto_mute`: заглушённое вручную остаётся заглушённым, когда звук
  возвращается.

### Подключение

| ESP32 | шлюз |
|---|---|
| GPIO16 (RX) | линия CPU TX → вывод 39 EH-MB05 (UART_RX) |
| GPIO17 (TX) | линия CPU RX ← вывод 38 EH-MB05 (UART_TX) |
| GND | GND |

Линию CPU RX держит TX модуля: перед подключением GPIO17 снимите модуль или
перережьте дорожку от вывода 38.

## Формат кадра

```
смещение  размер  поле
0         1       7E                 флаг начала
1         4       адрес источника, little-endian
5         4       адрес получателя, little-endian
9         1       подсистема         ([10] = 00)
11        1       команда            ([12] = 00)
13        ..      данные
n-3       1       checksum = sum(bytes[1 .. n-4]) & 0xFF
n-2       1       00
n-1       1       7E                 флаг конца
```

Внутри кадра `7D` передаётся как `7D 5D`, а `7E` — как `7D 5E` (экранирование
в стиле HDLC). Контрольная сумма считается до экранирования.

### Адреса

| адрес (LE в кадре) | значение | смысл |
|---|---|---|
| `FE FF FF FF` | `FFFFFFFE` | client: приложение на телефоне / сторона Bluetooth |
| `00 00 00 00` | `00000000` | master: сам шлюз |
| `FF FF FF FF` | `FFFFFFFF` | рассылка всем узлам |
| `56 00 02 00` | `00020056` | лампа (пример) |

Приложение сначала отправляет мастеру `handshake` и `enum_slaves`, затем
обращается к каждому узлу по полученному адресу или шлёт рассылку.

## Команды

`[n]` — смещение байта в кадре. Неиспользуемые байты равны `00`.

| подсист./ком. | имя | длина | параметры |
|---|---|---|---|
| 0/0 | generic_info | 16 | ответ: имя `C01-BR30 Link` с `[13]`; productID `[45..48]`: 1 одиночная лампа, 2 аудиобокс (Pulse Link, без лампы), остальное — лампа |
| 0/1 | handshake | 16 | ответ `[13]` = `01` — OK |
| 0/2 | enum_slaves | 16 | ответ: число узлов `[15..16]` (вместе с мастером), адреса с `[17]` по 4 байта |
| 0/4 | set_name | 32 | `[13..25]` имя, до 13 байт |
| 0/5 | node_name | 16 | запрос имени |
| 0/7 | identify | 16 | «начало настройки»: выбранная лампа мигает |
| 0/8 | config_end | 16 | |
| 0/11 | set_config | 20 | `[13]` параметр, `[15]` значение. Параметр 0 — память света, 0 выкл / 1 вкл |
| 0/12 | query_config | 18 | `[13]` параметр; ответ `[15]` значение |
| 1/0 | lamp_onoff | 20 | `[15]` 0 вкл, 1 выкл |
| 1/1 | brightness | 20 | `[15]` групповое значение, `[16]` своё, 0..100, `FF` — не менять |
| 1/3 | lamp_state | 18 | ответ `[15]` 0 вкл / 1 выкл, `[16]` яркость 0..100 %; `245` — у узла нет лампы (Pulse Link), `255 255` — донгл |
| 2/0 | channel | 19 | `[15]` 0 правый, 1 левый, 2 левый+правый |
| 2/1 | eq | 19 | `[15]` 0 normal, 1 pop, 2 jazz, 3 classic, 4 rock, 5 movie |
| 2/2 | mute | 19 | `[15]` 1 без звука, 0 со звуком |
| 2/3 | volume | 20 | `[15]` громкость 0..100, `[16]` = `FF`; на `all` меняет только лампы, собственный аудиовыход Pulse Link остаётся на своём уровне (при ≤ 5 он лишь замолкает); на `master` шлюз запоминает и возвращает значение, но громкость его выхода не меняется |
| 2/4 | speaker_state | 18 | ответ `[15]` канал, `[16]` eq, `[17]` mute, `[18]` громкость % |
| 4/0 | connect_code | 16 | ответ `[13..16]` код сети = SN шлюза (напр. `00096CC0`) |
| 4/1 | set_connect_code | 20 | `[13..16]` код для записи в лампу; `00FFFFFF` отвязывает её |
| 4/2 | set_id_address | 24 | таблица узлов шлюза: `[13]` 0 добавить / 1 удалить, `[17..20]` адрес лампы |
| 4/6 | audio_source | 19 | `[15]` 0 = 3,5 мм, 1 = Bluetooth |
| 4/7 | get_audio_source | 16 | ответ `[13]` источник |

В константах самого приложения канал 0 назван «левым», а 1 — «правым». Интерфейс
эти константы не использует, а в записи UART 0 = правый, 1 = левый, поэтому
таблица следует записи.

### Что делает приложение

- **Яркость всех ламп:** пока двигается ползунок, шлёт `brightness` на `all`
  с `[15]` = значение, `[16]` = `FF`, затем `FF 64` («применить»), затем
  `lamp_onoff` вкл.
- **Яркость одной лампы:** шлёт на её адрес `[15]` = `FF`, `[16]` = значение.
- **Громкость:** `volume` на `all`, затем `mute` 0.
- **Добавить лампу:** прочитать код шлюза через `connect_code`, записать его
  в лампу через `set_connect_code`, сделать `handshake` лампе, добавить её
  через `set_id_address` (`[13]` = 0), затем заново прочитать `enum_slaves`.
- **Удалить лампу:** `handshake` лампе, `set_connect_code` `00FFFFFF`,
  `set_id_address` (`[13]` = 1).

### Примеры

```
лампы выкл, все    7E FE FF FF FF FF FF FF FF 01 00 00 00 00 00 01 00 F9 00 7E
громкость 59%, все 7E FE FF FF FF FF FF FF FF 02 00 03 00 00 00 3B FF 36 00 7E
EQ movie, все      7E FE FF FF FF FF FF FF FF 02 00 01 00 00 00 05 FF 00 7E
мигнуть 00020056   7E FE FF FF FF 56 00 02 00 00 00 07 00 5A 00 7E
ответ lamp_state   7E 56 00 02 00 FE FF FF FF 01 00 03 00 00 00 01 62 64 00 00 00 00 1E 00 7E
                   (выкл, 98 %)
```
