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

## REA: temporal history и текущий компилятор

Продолжение 2026-11-17, без нового запуска игры. REA dossiers:

- detector RVA 0x74470: `ev_829312bf3d34535eb68b905f285c72bcc5928c9b16815e0e256b7cc280c1a547`;
- history hook RVA 0x6d2d0: `ev_ff055cf881cd185c6e40f9a3bcdc4b83499d0a23482823c4e37cf9debe8f22c7`;
- resource helper RVA 0x6a610: `ev_23833c14bcfa8ca4828c51ab1b4de67e2e89c8b37e77c4e6fa3a71f9c431eb76`.

Detector требует точных позиций трёх блоков относительно world render:

| Блок | Требует DLL | В сохранённом коде игры | Дополнительное различие |
|---|---:|---:|---|
| serial/history swap | +0x50 | +0x43 | другое распределение регистров |
| previous matrix copy | +0x299 | +0x293 | тот же шаблон по другому адресу |
| TAA call / previous camera | +0x373 | +0x36d | R13 вместо RBX для temporary pointer |

Проверка reset flag по scene+0x23f1 также использует другую форму инструкции
(не тот register-based byte compare, которого требует DLL). Это доказывает
несовпадение guard с этой сборкой; не доказывает безопасность его отключения.

По REA resource helper недостаточно сохранить две CPU-матрицы. Для
переключаемого eye bank helper поддерживает 10 engine buffers; создаёт их
при отсутствии или различии descriptor prefix (0x38 bytes):
scene+0xa50/a58, aa8/ab0, ac0/ac8, ae0/ae8, af8/b00. Дополнительно выделяются
пять объектов TAA (scene+0xa30, поля +0x50/58/60/68/70). Helper сравнивает
descriptors/размеры, ждёт renderer idle при замене и пользуется engine-specific
allocator/destructor. Hook отдельно хранит history indices, previous matrices,
часть camera state и TAA counter; переключает dirty/reset между глазами.
Один bank этих полей — 0x108 bytes. Это факты реализации reference DLL,
не проверенный layout/ownership контракт для нашего engine backend.

Новый read-only scripts/Inspect-TemporalCompatibility.py требует те же полные
SHA игры/snapshot/reference, сверяет таблицу 10 fields в DLL и находит блоки
в сохранённом runtime .text. Он разрешает CALL rel32 и проверяет, что три
release call внутри TAA create ведут в один target. Получены кандидаты игры:
TAA frame 0xa3e60, TAA create 0xa41a0, TAA release 0x5dde10,
buffer release 0x5bd420. ABI, resource states, allocator ownership и lifecycle
в игре этим не проверены. Скрипт ничего не патчит/запускает и всегда сообщает
stereo_replay_enabled=false. Сырой REA output и файлы игры остаются под local/.

Проверки нового аудитора: Python unittest 21/21, включая неизвестные SHA всех
трёх входов, неоднозначные/перекрывающиеся patterns, signed rel32 и границы.
Нативный код этим срезом не изменялся; предыдущий CTest 18/18.

### Уточнение типа history buffers

Разбор pinned engine code уточнил resource helper: renderer vtable +0x138
ведёт в factory RVA 0x5a8610. Он создаёт framebuffer aggregate, используя
texture factory slot +0xe8 (0x5a9000) для вложенных color/depth textures.
Constructor 0x59cb90 копирует 0x38 descriptor bytes; color pointers находятся
в +0x38 (до восьми), depth в +0x78, auxiliary в +0x80. Wrapper size 0x88.
Buffer release 0x5bd420 через manager передаёт aggregate в 0x5cb600;
тот освобождает вложенные textures через 0x5dde10. Это не COM Release.

Следовательно, десять scene pointers нельзя заменять десятью сырыми D3D12
textures. Format codes descriptor также engine-specific, не DXGI_FORMAT.
Эти выводы получены из статического потока инструкций; engine factories и
destructors нашим кодом не вызывались, ownership/idle guarantee не доказаны.
Slot +0x1d0 (0x5accf0) обрабатывает fence/pending lists, но полного GPU idle
контракта пока нет. Следующий backend должен сохранять вложенную структуру
ресурсов и подтверждённый lifecycle, а не обходить guards reference DLL.
