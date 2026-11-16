# Изолированный эксперимент v1.1.0 / Teardown 2.0.0

2026-11-17. По просьбе пользователя применён [REA](https://rea.tools/),
rea-agents 6.9.0 + Ghidra 12.1.4. Это отдельный путь исследования совместимости,
а не замена собственной native-сборки или перенос протокола в `mod/`.
Исходники авторской DLL по-прежнему отсутствуют. Декомпилированные функции,
DLL, snapshot игры и сырые отчёты не публикуются.

## Причина отказа tool placement

Идентичности SHA-256:

- Исходная dsound.dll: `c315341efe9d1ec8825f79d8c6ed6e1a9ba1e61ab233d26c3b97c13b004478be`.
- EXE: `2733e37969fd6d45c5c2130872b0d4096147c304266b4b0adda01bd98caa2c4d`.
- Снятый ранее runtime .text, первый RVA 0x1000:
  `b2b804daa22e4c65ffeb8786f2947ff9d91a679079089bc375a07a98c0e10ccf`.

REA function dossier для detector RVA 0x78a00:
`ev_2bad7a696d00b4f2646c766eb77a152a4d4d36bae460a86216acca1754c62e68`.
Сигнатура копирования transform находит игру +0x448712, пролог кандидата
+0x447520 совпадает. Но поиск player link ожидает `48 8b f1 48 8b 49 ??`;
игра +0x447583 использует `48 8b f1 48 8d 79 58 48 8b 0f`.
В последнем случае LEA адресует поле +0x58, затем MOV разыменовывает его.

Обработчик RVA 0x6efb0 читает тот же указатель по извлечённому offset, сохраняет
source transform, вызывает оригинал и пишет destination transform. Досье:
`ev_ec029164491f0c8effafcdcb6d4633f13b646cb4d0e4294e39cd82c466ac99d93`.
Неопределённое имя инструмента даёт пустую строку и базовую ветку копирования
позы; специальные offsets/анимации по имени этим не подтверждены. Name reader:
`ev_506deac0db1ce72c68ae5991dd01d463b1d5a9a4947121fbecce874bfd0a6deb`.

`scripts/Patch-V110-ToolDetection.py` меняет только два сравниваемых immediate:
DLL RVA 0x78b32: 8b → 8d; RVA 0x78b38: 49 → 79. Это допуск именно новой формы,
не универсальный сканер для старых игр. Извлечение offset +6 остаётся прежним.
Требуются все три полных hash, точные исходные bytes, новый файл dsound.dll
только под `local/`. Исходник не перезаписывается; скрипт ничего не запускает.
Результат: `d4520c9d0c77d615712234ea97ecaaf5d2778c55c1197c0474c2a3ac86d8eeb3`.

## Фактическая проверка

В отдельную development-копию после резервной копии её профиля помещены только
полный Lua/native пакет v1.1.0 и экспериментальная DLL. Живая установка не
менялась. Лог теперь подтверждает `first-person tool placement hooked`
на +0x447520: player 58, source 9f4, destination 82c, flag a20, anim 68,
name 0/0. Это доказательство установки hook, **не** проверка вращения руки,
геометрии инструментов, physics или комфорта в шлеме.

Собственный capture startup рядом с этим пакетом отказал. Поэтому добавлен
`--profile-only`: только перенаправление папок/окружения, без собственных DXGI
hooks. Рендер принадлежит авторской DLL, а не нашей реализации.

OpenXR не создал instance: -51 (runtime unavailable), кадров в шлем нет.
SteamVR процессы работают, ActiveRuntime указывает на существующий manifest.
В изолированном профиле отсутствует `openvr/openvrpaths.vrpath`; это вероятная
причина: [код Valve](https://github.com/ValveSoftware/openvr/blob/master/src/vrcore/vrpathregistry_public.cpp)
читает этот файл из Local AppData либо `VR_PATHREG_OVERRIDE`. Изоляция изменила
AppData, поэтому наличие runtime manifest само по себе недостаточно.
Причина ещё требует проверки с отдельной копией path registry; глобальные
ALVR/SteamVR settings не менялись. После Escape UI остановлен.

## Ограничения стерео

REA render-only helper RVA 0xb7330, evidence
`ev_f41929bec3b3211426852d481e5a1c0c854af14c2ebbe84ffa4ab07f4d3029e0`,
проверяет mode 2, состояние пары, game state 6 и replay requests. При отказе
возвращает управление обычному update. В данном запуске лог прямо сообщил,
что mode 2 недоступен (time scale / camera requests), активен fallback mode 1;
per-eye history также недоступен. Сообщение `synchronised stereo ready` поэтому
не доказывает синхронность. Эти ограничения не устранены двухбайтовой правкой.

Автоматически проверено: 12 Python tests, включая отказы patcher при неизвестных
hash/bytes/RVA, выходе за local/ и существующем output; native CTest 15/15,
включая отдельный profile-only child без XR и graphics capture.
