# SparRAW-driver

Драйвер wil6210 (Qualcomm/Wilocity Sparrow, 60 ГГц 802.11ad) для OpenWrt —
форк проекта SparRAW: IBSS/PBSS без ролей, FTM, инструментовка прошивки
и ucode, приём параметров BSS станцией по стандарту.

## Устройство репозитория

Драйвер — собственный форк wil6210, одна открытая версия для всех прошивок
SparRAW (деление на open/pro есть только у прошивки).

| путь | что |
|---|---|
| `drivers/net/wireless/ath/wil6210/` | исходник драйвера — **каноничная форма**; правки — коммитами прямо сюда |
| `openwrt/install.sh` | кладёт драйвер в дерево OpenWrt одним патчем `900-wil6210-sparraw.patch` (разница с backports из `dl/`) и оверлей `files/` |
| `openwrt/files/` | оверлей образа: `rc.local` (уровни лога, сборщик ucode), `modules.d`, `sysctl.d` |
| `openwrt/mac80211.Makefile` | Makefile пакета, с которым собиралось |

Прежняя серия патчей 900–922 — в истории репозитория; больше не ведётся.

## Что в драйвере сверх upstream

| что |
|---|
| интерфейс ADHOC, `join_ibss`/`leave_ibss` поверх PBSS, `ibss_creator`, BSSID ячейки (`WMI_IBSS_BSSID`), соединение с соседом ячейки |
| `WMI_PBSS_JOINED_EVENTID` (0x15) → connect; `roleless_link` (флаг в `WMI_PCP_START`, нужен `oob_mode=1`) |
| запасное имя образа прошивки; PS выключен по умолчанию |
| лог прошивки (`fw_log_level`, debugfs), сторож тихого перезапуска `fw_watchdog_ms`; лог и трасса ucode (`uc_trace_ms`, debugfs `uc_trace`); debugfs `mem_write` |
| сброс TX-колец при сбросе устройства |
| станция принимает BI и `dot11MaxLostBeacons` (1..32) из BSS (802.11-2020 11.1.3.3.1, 11.1.3.1) |
| роуминг без FT — переассоциация; переассоциация связанной станции на месте (`WMI_REASSOC_INPLACE_CFG`) |
| split MAC: `WMI_NEW_STA` только после отправленного ответа на ассоциацию |
| FTM-инициатор через cfg80211 peer measurement, калибровка TOF |
| исправления upstream: ожидание событий WMI без взаимной блокировки, проверка длин и адресов от прошивки, восстановление AP, BlockAckReq по 802.11-2020 10.25.6.6.3 и др. — см. историю `drivers/` |

## Сборка в OpenWrt

```sh
make package/kernel/mac80211/download      # если нет dl/backports-*.tar.*
openwrt/install.sh /путь/к/openwrt
make package/kernel/mac80211/{clean,compile} V=s
```

## Связанные репозитории

* `SparRAW-firmware` — прошивки 4.1/6.2 из исходников;
* `SparRAW-tools` — хостовые и стендовые скрипты (лог прошивки, трасса ucode);
* `SparRAW-docs` — описание чипа.

## Лицензия

Драйвер — ISC ([LICENSE](LICENSE)), как исходный wil6210;
`openwrt/mac80211.Makefile` — GPL v2 (OpenWrt); документация — CC BY 4.0.
Подробно — [COPYING.md](COPYING.md).
