# SparRAW-driver

Драйвер wil6210 (Qualcomm/Wilocity Sparrow, 60 ГГц 802.11ad) для OpenWrt
с патчами проекта SparRAW: IBSS/PBSS без ролей, инструментовка прошивки
и ucode, приём параметров BSS станцией по стандарту.

## Устройство репозитория

| путь | что |
|---|---|
| `drivers/net/wireless/ath/wil6210/` | исходник драйвера: первый коммит — чистый backports-7.2 (OpenWrt master), дальше по коммиту на каждый патч серии |
| `openwrt/patches/ath/9xx-wil6210-*.patch` | та же серия в виде патчей quilt для `package/kernel/mac80211` — **каноничная форма** |
| `openwrt/files/` | оверлей образа: `rc.local` (уровни лога, сборщик ucode), `modules.d`, `sysctl.d` |
| `openwrt/mac80211.Makefile` | Makefile пакета, с которым собиралось |
| `openwrt/install.sh` | копирует серию и оверлей в дерево OpenWrt |

## Серия патчей

| № | что |
|---|---|
| 900 | интерфейс ADHOC |
| 901 | `join_ibss`/`leave_ibss` поверх PBSS |
| 902 | запасное имя образа прошивки |
| 904 | PS выключен по умолчанию |
| 905–907 | лог прошивки: `fw_log_level`, debugfs, сторож тихого перезапуска `fw_watchdog_ms` (признак — обнулённые байты уровней журнала) |
| 908 | сброс TX-колец при сбросе устройства |
| 909–910 | лог и трасса ucode (`uc_trace_ms`, debugfs `uc_trace`) |
| 911 | debugfs `mem_write` |
| 912 | `ibss_creator`: создать ячейку или присоединиться |
| 913 | `WMI_PBSS_JOINED_EVENTID` (0x15) → connect |
| 914 | станция принимает BI из BSS (`wmi_set_sta_bcon_int`, 802.11-2020 11.1.3.3.1) |
| 915 | `dot11MaxLostBeacons` из элемента DMG Operation (11.1.3.1) |
| 916 | `roleless_link`: безролевой линк между PCP (флаг в `WMI_PCP_START`, прошивка 6.4, нужен `oob_mode=1`) |
| 917 | BSSID ячейки IBSS прошивке (`WMI_IBSS_BSSID` 0x85a, прошивка 6.4 mesh; создатель без BSSID — случайный локальный) |
| 918 | роуминг без FT — переассоциация (`WMI_CONNECT` с `SEND_REASSOC`, `cfg80211_roamed`) |
| 919 | переассоциация связанной станции на месте (точка, `WMI_REASSOC_INPLACE_CFG` 0x85b) |
| 920 | split MAC: `WMI_NEW_STA` только после отправленного ответа на ассоциацию |
| 921 | IBSS: соединение с соседом, заведённое прошивкой mesh после SLS в DTI (802.11-2020 10.42.6), — новая станция ячейки |

## Правка патчей

Только через quilt в дереве OpenWrt:

```sh
make package/kernel/mac80211/{clean,prepare} QUILT=1
cd build_dir/target-*/linux-*/mac80211-regular/backports-7.2
quilt push -a
quilt new ath/9NN-wil6210-что-делает.patch   # или quilt push до нужного
quilt add drivers/net/wireless/ath/wil6210/<файл>
# правка
quilt refresh
cd -; make package/kernel/mac80211/update
```

Затем скопировать обновлённый патч в `openwrt/patches/ath/` и повторить его
отдельным коммитом в `drivers/…` (`git apply` + `git commit -s`).

## Связанные репозитории

* `SparRAW-firmware` — прошивки 4.1/6.2 из исходников;
* `SparRAW-tools` — хостовые и стендовые скрипты (лог прошивки, трасса ucode);
* `SparRAW-docs` — описание чипа.

## Лицензия

Драйвер и патчи — ISC ([LICENSE](LICENSE)), как исходный wil6210;
`openwrt/mac80211.Makefile` — GPL v2 (OpenWrt); документация — CC BY 4.0.
Подробно — [COPYING.md](COPYING.md).
