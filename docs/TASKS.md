# Задачи разработки

Обновлять статусы и результаты в этом файле по мере работы. `[ ]` — не выполнено, `[x]` — выполнено. У каждой завершённой задачи записывать коммит и фактическую проверку. Непроверенное в шлеме поведение так и отмечать.

## P0 — воспроизводимая база и нативный модуль

- [x] DEV-001: создать отдельный репозиторий, импортировать MIT-пакет v0.22 без изменений, сохранить лицензии и контрольные суммы. Коммит ef5b657; проверены 13 файлов в Git-объектах; main опубликована.
- [x] DEV-002: аудит Lua ↔ DLL сохранён в docs/ARCHITECTURE.md. Коммит f3c950a; разобраны protocol 3 / Lua 22, диапазоны слотов, позы/единицы, координаты, tick/update/render/draw, камера, инструмент, UI, физика. Три Lua 5.1 теста проходят, включая init и приём/отказ native snapshot. Внутреннее преобразование OpenXR и синхронизация DLL не подтверждены; ограничения явно записаны. В шлеме не проверено.
- [x] DEV-003: проверены публичные refs, обе версии дерева, два релиза и содержимое обоих ZIP; C++-исходников нет. Коммит f3c950a, точные URL/SHA/лицензия и выбранный путь — docs/NATIVE_SOURCE_AUDIT.md. Начато собственное математическое ядро под MIT; исходная DLL не считается пересобираемой. Автору сообщения не отправлялись.
- [ ] DEV-004: начата, коммит ae0f763. Найдены MSVC 19.44, CMake 3.31.6 и SDK 10.0.26100.0; scripts/Build-Native.ps1 воспроизводимо собирает собственную tdvr_math.dll и executable, CTest проходит. Проверены композиция поз, yaw/pitch/roll, масштаб/IPD, асимметричные frustum edges/depth и ошибочные входы. Зависимости/ABI/команды — docs/BUILD.md. Это рабочий математический срез, не DXGI proxy/VR renderer. Остаются OpenXR/D3D12 стенд, диагностика движка и проверяемые перехваты; в шлеме не проверено.
- [ ] DEV-005: начата статическая диагностика scripts/Inspect-Teardown.py; SHA-256, PE machine/версии, regular/delay imports и отказ всех неподтверждённых возможностей. Пять синтетических unittest проходят; установленный EXE разобран без запуска. PE-версия 1.0.0.1 отличается от пользовательской 2.0.0; найдены OpenGL/SwapBuffers, D3D12 import отсутствует. docs/ENGINE_COMPATIBILITY.md фиксирует доказательства и ограничения. Пока нет потребителя gate в игровом модуле и проверенных перехватов — задача открыта.
- [x] DEV-006: коммит 8e8ac4f, вызов DisableMotionBlur защищён проверкой type == function. Полный Lua загружается в Lua 5.1; updateActive доходит до камеры/гаптики при наличии и отсутствии API (engine-facing зависимости подменены). v1.1.0 не переносилась, игра не запускалась, в шлеме не проверено.

Продолжение 2026-11-16: DEV-005 начата коммитом 3348a09. DEV-004 получил
отдельный OpenXR/D3D12 preflight, статически собранный pinned loader и GPU
readback двух независимых targets. CTest 4/4, Python 8/8; WARP, RX 6900 XT и
реальный SteamVR/OpenXR 2.18.4 preflight прошли без пользовательского теста
в шлеме. Добавлен owned stereo packet с общим simulation id/time и отдельными
view/projection (коммит f8f64d2); это CPU-контракт, не синхронизация игрового рендера.
DEV-004 остаётся открытой: игровой render hook и интеграция frame loop отсутствуют.
Последующий срез добавил ручной `--session`: idle session и две swapchain
64×64 создаются на runtime-required D3D12 adapter и освобождаются. Один запуск
SteamVR/OpenXR подтвердил по 3 image на глаз, format 29, без кадров; затем
пользователь запретил обращения к шлему на время Roblox, runtime больше не запускать.
Fake session allocation/cleanup и callback exception tests проходят без runtime.
Ресурсный срез — коммит 6d28ff6. Дополнительно реализован отдельный
`run_stereo_frame`: WaitFrame/BeginFrame → один LocateViews → два acquire/wait →
один callback всей пары → releases → EndFrame с одной projection layer.
shouldRender=false, неотслеживаемая/невалидная поза не отправляют стереопару;
таймаут/ошибка ownership требуют пересоздания session. Алгоритм проверен только
fake XR, реального renderer callback, session event pump и игрового freeze ещё нет.
Итого CTest 6/6, Python 8/8, без повторного запуска runtime.

Следующий срез: tdvr_capture.dll и development launcher перехватывают реальный
DXGI/D3D12 Present в отдельном WARP host, читают backbuffer с точным hash,
сохраняют штатные HRESULT и поддерживают успешный/неуспешный resize. Проверена
загрузка DLL до main host без статического импорта. CTest 9/9, Python 8/8;
runtime/шлем/Teardown не запускались. Подробности и ограничения — CAPTURE.md.
До VR-прототипа остаётся подключить настоящий engine render/camera, два прохода
и OpenXR GPU submit. Захват плоского backbuffer сам по себе не закрывает VR-101/102.

Продолжение DEV-004/005: подготовлена отдельная development-копия с собственным
профилем и проверено перенаправление AppData/Documents/getenv. Пользователь
разрешил desktop-запуск, затем сообщил о завершении Roblox; прежнее ограничение
runtime на время Roblox больше не является текущей причиной не запускать стенд.
Desktop Teardown дошёл до меню/3D-сцены с собственным capture module; получен
реальный backbuffer 3440×1440. Отдельный camera observer допускается только по
SHA-256 + runtime signature: 183 valid samples, 0 rejected, view error ~1.58e-6.
Камера ещё не переопределяется, правильное VR-стерео этим не доказано.
Следующий шаг — gameplay camera selection/render boundary, отдельные eye targets
и подключение реального OpenXR lifecycle/GPU renderer.
Этот срез прошёл CTest 11/11 и Python 9/9; неизвестный host отвергается до
engine hook. Кандидаты update/camera-setup/frame root записаны в
ENGINE_COMPATIBILITY.md, но ещё не используются для повторного render pass.

Следующий срез DEV-004/VR-101: `tdvr_engine_camera` подготавливает сразу две
камеры в наблюдавшемся игровом формате (column-major float, view/projection,
near/far, 0x88 bytes) и восстанавливает исходный record после render scope,
включая исключения. Автоматически проверены frustum/depth/scale, отказ при
потере float precision и отсутствие частичного выхода/записи за границы.
Адаптер пока не вызывается в движке; world-render boundary и внешние camera
caches остаются неподтверждёнными. Разбор сохранённого кода исключил 0x7ac30
из render candidates: это цикл simulation steps, повторять его для второго
глаза нельзя. Проверки: CTest 12/12, Python 9/9; игра и runtime в этом срезе
не перезапускались, управление окнами не использовалось.

DEV-004/005: opt-in D3D12 recording trace перехватывает настоящие Draw/
DrawIndexed/Dispatch, ограниченно собирает EXE return RVAs и поддерживает
повторное окно наблюдения. CTest 13/13 (включая pixel-verified raster draws
на WARP), Python 9/9. Desktop-копия перезапущена отдельно; на вилле
подтверждён call path 0x6f770 → 0x865b0 → D3D12 draws/dispatches. Обнаружены
render-side счётчик/перестановка полей: безопасный stereo replay ещё не готов.
По новому явному разрешению пользователя выполнен OpenXR `--session`:
SteamVR 2.18.4, format 29, по 3 image на глаз; кадры не отправлялись.
Игра в шлем ещё не подключена, контроллеры ещё не телепортировались.

Следующий срез DEV-004/005: установлен проверяемый observational world-render
hook 0x865b0 с исходными четырьмя аргументами. На вилле: 780/780 calls completed,
49 valid / 0 invalid samples, 49 совпадений основной камеры, 0 её изменений,
49 шагов render serial и 49 перестановок history fields; один поток.
Camera setter продолжает совпадать с own math (~1.91e-6).
Это ещё не stereo replay: eye target layout, GPU ownership, отдельные histories
и OpenXR submit остаются работой следующего этапа. Unknown host/ABI gates,
CTest 13/13 и Python 9/9 проверяются без шлема.

DEV-004/VR-101: подключён ограниченный synthetic camera override к world-render
hook. Оба camera records проходят CPU validation до принятия запроса; выбранный
глаз подставляется только на время одного исходного вызова и восстанавливается
побайтно. Максимум 600 проходов / 10 секунд, явная отмена; native и CLI допускают
только development-копию под local/ с отдельным профилем и проверенной identity.
На desktop-вилле проверены локальный перенос 2 м и yaw 20°: изображение
изменилось, затем вернулось; 600 applied / 600 restored, 0 rejected. Ещё 600
проходов ранее проверены на 3D-фоне меню. Это та же основная камера: совпадение
с Game+0x240 само по себе не отличает gameplay от меню (Game+8: 6 / 2).
CTest 14/14, Python 9/9. Это один synthetic view, без XR tracking/submit,
без повторения world-render, перемещения контроллеров или исправления tools.
Инструмент на изображении остаётся в исходном мире; это не рабочая VR-рука.
Независимые GPU targets/histories и настоящее стерео ещё не реализованы.

DEV-004/005: добавлено opt-in наблюдение D3D12 RTV bindings и полных resource
descriptions с ограниченными EXE stacks. Desktop-процесс подтвердил все четыре
hooks (mask 15, HRESULT 0), MRT formats 29/31/28/34 размером 3440×1440 внутри
world-render. Обнаруженное частое переписывание descriptors потребовало
группировки по shape/caller без write serial; эта доработка пока проверена
только WARP, не перезагружалась в игру. WARP проверяет обе формы MRT, порядок,
descriptor copy invalidation, повторные записи, resize и неизменные пиксели.
Подробности/лимиты — CAPTURE.md. Это диагностика для выбора eye targets,
не GPU ownership, изоляция histories, stereo replay или OpenXR submit.
После пользовательского Escape управление окнами прекращено; дальнейшие
проверки этого среза выполняются без UI и шлема. Итог после исправления
группировки: CTest 14/14, Python 9/9; готового VR-рендера ещё нет.

DEV-004/VR-102: реализован reusable GPU eye-copy recorder с предварительной
проверкой COM device/format/extent и восстановлением resource states. Standalone
probe сохраняет red/green из последовательно переиспользуемого render source
в две committed textures, перезаписывает source blue и читает все три только
после submit/fence. Это строительный блок сохранения результатов проходов,
не два игровых глаза: реальные game resource states/queue ownership,
eye histories и XR release ещё не связаны с helper. Проверено: CTest 14/14,
Python 9/9 и отдельный hardware run на RX 6900 XT (221 pixels per eye,
row pitch 256, cross-device rejection=1). Runtime и UI не запускались.
Подробности — BUILD.md.

DEV-004/005: world-render observer расширен metadata output target; CLI
`--engine-targets [--stats-only]` не публикует pointers. В development-вилле
подтверждены 3440×1440 / один attachment, vtable resource в app-local Agility
D3D12Core. Первоначальный System32-only gate закономерно отказал (143 samples);
добавлен точный size/hash gate для Microsoft-signed game-local Core. Исправленная
DLL собрана отдельно и прошла unknown-host/ABI gate; её GetDesc в игре ещё
не подтверждён, старый module pinned до выхода процесса. Полный CTest 14/14
прошёл до этой коррекции gate, после неё повторён затронутый engine gate;
Python 9/9. UI/runtime не запускались. GPU copy/replay всё ещё не включены.

Последующее чтение уже существующих логов v0.22/v1.1.0 обнаружило игровое
D3D12 device/queue, backbuffer и TRendererD3D12. Поэтому рабочий путь — D3D12;
статические OpenGL imports не доказывают активный OpenGL. Следующий практический
этап: frame lifecycle и проверяемый перехват игрового D3D12 render path.
Наличие dxgi.dll или успешного отдельного стенда не доказывает hooks.
Подробности — BUILD.md и ENGINE_COMPATIBILITY.md.

DEV-005/HAND-203, 2026-11-17: REA/Ghidra установил конкретное несовпадение
сканера player link в v1.1.0 с имеющимся EXE. Hash-gated patcher меняет два
immediate в отдельной DLL под local/. Изолированная игра подтвердила hook
+0x447520, offsets 58/9f4/82c/a20; name 0/0. Вращение инструмента ещё не проверено.
Собственные capture hooks рядом с авторской DLL не запустились; добавлен
profile-only launcher без graphics hooks. OpenXR instance отказал -51, кадров
в шлем нет; вероятна недоступность OpenVR path registry в отдельном AppData,
это пока гипотеза. Mode 2 и per-eye history авторского мода недоступны на этой
игре. Проверки: CTest 15/15, Python 12/12. UI остановлен после Escape.
Подробности и REA evidence — V110_COMPATIBILITY.md. DEV-004/005 и HAND-203
остаются открыты; собственный XR renderer ещё не подключён.

DEV-004, собственный путь: добавлен opt-in --xr-panel в capture launcher.
Первый Present связывает XR с игровыми device/queue; текущий backbuffer копируется
в exact-format XR swapchain, восстанавливаются resource states, затем fence,
release и одна quad layer. Реализован session event lifecycle и прекращение
кадров при ошибке/resize, без RemoveDevice. Это плоская панель, не stereo world
и не игровые руки. CTest 16/16 (fake XR + реальные WARP copy/readback), Python
12/12; запуск нового режима на runtime/игре/шлеме после Escape не выполнялся.
Отдельная v1.1.0 development-копия всё ещё содержит её proxy: перед собственным
runtime-тестом его нужно отключить с сохранением копии. Смешивание запрещено.
Параметры, stats и ограничения — BUILD.md. DEV-004/VR-101/102 остаются открыты.

DEV-004, runtime-проверка 2026-11-17: отдельный OpenVR path registry устранил
-51, собственная game session создана. Exact backbuffer format 28 не поддержан
runtime; XR format 29 создаётся, но первая copy вернула E_INVALIDARG до submit.
WARP regression воспроизвёл отказ с typeless storage; SDR recorder теперь явно
проверяет declared SRGB format / DXGI color space и совместимую format family,
не ослабляя exact eye-copy. Полутона и typeless copy проходят автоматически.
Stats ABI 2 показывает format и failure stage. После закрытия Roblox
пользователем проверен реальный game → OpenXR submit: 12080/12080 кадров,
XR/GPU=0, quarantine=0, formats 28 → declared 29 / storage 27. Это плоская
панель; независимое стерео, руки и комфорт в шлеме не подтверждены.
Пользователь затем подтвердил видимость панели в шлеме и прислал снимок
SteamVR Mirror. Это визуальное подтверждение панели, не правильного VR-мира.
DEV-004 и VR-101/102 остаются открыты. Подробности — BUILD.md.

DEV-004/005: добавлен собственный development API без Computer Use: отдельная
DLL, named pipe и CLI/stdio MCP, команды ping/status/request-exit. Реальные
attach/status и штатное закрытие предыдущей копии проверены без курсора и
фокуса. Доступ только к development-копии; MCP не зарегистрирован автоматически.
CTest 17/17, Python 16/16. Протокол и ограничения — CONTROL_API.md.
Добавлена read-only команда engine-status: камера/world counters/target,
без установки hooks и передачи указателей. Сборка и локальные проверки пройдены;
новая команда пока не проверена сквозным вызовом в игре. Development-процесс
штатно закрыт перед миссией пользователя. До его сообщения о завершении игры
не запускать игру/OpenXR и не подключаться к живой установке.
В текущем игровом процессе observer также подтвердил output 3440×1440,
format 28, game_state=6, 170 valid / 0 rejected samples. Это наблюдение
ресурса и render boundary; второй проход и независимые eye targets ещё не связаны.

DEV-004/VR-103: собственная XR session теперь читает согласованную пару
LocateViews на predictedDisplayTime. Owned pose/FOV snapshot доступен через
TdvrGetXrViews и CLI/MCP xr-views, с обязательным available и XR frame id.
Fake XR проверяет asymmetric FOV, позы, loss/invalid/recovery/no-render/stop;
обычные capture hosts проверяют новый ABI и отсутствие запуска XR при чтении.
Это подготовка tracking source: камера игры не потребляет эти позы, вызов
пока на Present, stereo world rendering и руки не подключены. Проверка
новой сборки в настоящем runtime отложена на время пользовательской миссии.
Сборка build/native-trace и CTest 17/17 прошли; Python 16/16.
Подробности и ограничения — BUILD.md / CONTROL_API.md.

DEV-004/VR-101/103: общий adapter преобразует owned XR views в обе камеры
движка с отдельной simulation key текущего render context. Проверяет ожидаемые
XR id/time, flags/ABI, численные значения и float precision. Панель использует
его только для валидации; world-render hook пока не потребляет результат.
Новый CPU test проверяет аналитические точки обоих asymmetric frustum, масштаб
и IPD один раз, body/head yaw и отказ stale/invalid пары без частичной записи.
Игра/runtime не запускаются до окончания пользовательской миссии. Синхронные
два прохода, histories и перенос начала XR-кадра перед world render остаются
следующими частями; DEV-004 и VR-101/102/103 не закрыты. Проверки:
Build-Native.ps1, CTest 18/18, Python 16/16; PE imports capture не требуют
отдельной tdvr_math.dll.

DEV-004/VR-102: --xr-world-timing связывает gated engine world hook с собственным
capture export: Wait/Begin/LocateViews перед оригинальным world render,
copy/release/End на Present. Отдельный development-only opt-in; обычная панель
сохраняет прежний timing. Пока это один штатный world render и плоский quad,
без eye camera override, freeze или изоляции histories. Fake XR/WARP покрывает
split lifecycle, duplicate/no-render/stop/error/cleanup; runtime/game проверка
нового режима отложена до окончания пользовательской миссии. Команды и
ограничения — BUILD.md. Проверки: CTest 18/18, Python 16/16, launcher
отвергает отсутствие profile до создания процесса. VR-102 остаётся открытой.

DEV-005/VR-102: REA resource helper выявил 10 scene history buffers и 5 TAA
objects на глаз, descriptor comparison и idle-before-replace. Разбор snapshot
подтвердил три несовпадающих guard offsets/register forms. Добавлен read-only
Inspect-TemporalCompatibility.py с полными SHA gates, уникальностью patterns
(включая overlap) и проверкой rel32. На настоящих сохранённых файлах найдены
TAA create/release/frame и buffer-release candidates; никаких patches/hooks
или новых игровых запусков. Результаты/REA evidence — V110_COMPATIBILITY.md.
Allocator ABI/ownership/GPU states ещё не проверены: replay остаётся выключен.
Проверка аудитора на pinned файлах и Python 21/21 прошли; native в этом срезе
не менялся (последний CTest 18/18).

DEV-005/VR-102: read-only temporal observer подключён к sampled main-camera
world boundary и engine-status через TdvrGetTemporalStats. Перед/после читает
serial/history/reset, 10 descriptor prefixes и 5 TAA pointer slots; публикует
маски и fingerprints, не адреса. Никаких writes/COM/allocation/history swaps.
CPU fixture проверяет side effects в данных, неизменность памяти, mask failures
и bounds; неизвестный host не включает hooks. Реальная scene проверка новой
сборки отложена до сообщения пользователя о завершении миссии. Разделение GPU
history banks, allocator ABI и replay всё ещё не реализованы; VR-102 открыта.
Сборка и CTest 19/19, Python 21/21 прошли без игры/runtime.

DEV-004/VR-102: EyeHistoryResources выделяет независимые committed D3D12
textures для двух глаз без понижения format/precision. Atomic rebuild,
queue-owned fence и scope begin/seal предотвращают замену незавершённых ресурсов;
небезопасный cleanup удерживает их до process exit, не RemoveDevice.
WARP прочитал 836 pixels RGBA8/float16 red/green и проверил настоящий pending
queue gate, allocation rollback и resize. Build-Native.ps1, CTest 20/20,
Python 21/21. Engine wrappers, пять TAA objects на глаз и CPU history switching
ещё не подключены; это GPU storage component, не stereo renderer. Пользователь
ответил «Ещё играю» — игровые/XR-проверки по-прежнему не запускать.

DEV-004/005: после разрешения пользователя проверен --xr-world-timing в
development-игре без Computer Use. 12422/12422 XR frames, XR/GPU=0;
tracked poses/FOV обоих глаз доступны через API. World: 11245 completed,
703 valid samples, 0 invalid/thread changes; output target GetDesc подтверждён.
Добавлен проверяемый parser framebuffer descriptor и target_layout_mask:
в runtime все десять layouts валидны (0x3ff), пять TAA slots присутствуют.
Важное уточнение: scene banks состоят из framebuffer aggregates с вложенными
textures, поэтому отдельные D3D12 allocations ещё не являются engine banks.
CTest 20/20; стерео world, head-camera override и руки пока не подключены.
DEV-004/005 и VR-101/102/103 остаются открытыми.

DEV-004/VR-103: добавлена калибровка tracking origin относительно текущей
игровой точки обзора. Середина поз обоих глаз совмещается с game anchor;
обратная tracking pose учитывает масштаб только для translation, высота HMD
не прибавляется к игровой повторно. Quaternion midpoint учитывает q/-q.
CPU tests проверяют yaw и наклон/roll, relative roomscale, разные eye rotations,
неверные данные и неизменность результата при отказе. Build-Native.ps1
-BuildDirectory build/native: CTest 20/20; расширенный tracking test также прошёл.
Это расчёт привязки, пока не live head-camera override; игровой hook должен
передать гарантированно текущую пару и ограничить срок действия preview.

DEV-004/VR-103: bounded live-pose preview подключён к world hook. Текущий
XR pair возвращается вместе с успешным begin, затем один выбранный eye camera
record подставляется и восстанавливается RAII; synthetic/tracked tests взаимно
отменяются. 600 calls / 10 seconds, только gameplay state 6; loss/duplicate/
invalid pair прекращает preview. Fixed anchor пока без locomotion/caches/tools.
В отдельной development-игре после появления world render проверено:
600 applied / 600 restored / 0 rejected / 0 remaining; XR frames 3496/3496,
XR/GPU=0, quarantine=0. Без Computer Use, живой каталог не изменён.
Автоматически: Build-Native.ps1 build/native, CTest 20/20, Python 21/21.
Картинка в шлеме на этом срезе визуально не проверена; stereo world, руки и
headset comfort не подтверждены. DEV-004 и VR-101/102/103 остаются открытыми.

## P1 — правильная картинка и трекинг

Зависит от доступа к нативному рендеру (DEV-003/004).

- [ ] VR-101: CPU stereo packet уже строит отдельные асимметричные view/projection, масштаб/IPD и обратные позы; проверены мировые точки/frustum edges/height/разные ориентации глаз. OpenXR views проверены fake XR и настоящим runtime; игровой eye render pass не подключён.
- [ ] VR-102: CPU packet отвергает разные simulation id / predicted time и не публикует частичный результат. Источник общего состояния игры, два render pass и стерео GPU submit ещё не реализованы; синхронность движка не доказана.
- [ ] VR-103: согласовать голову, тело, roomscale, рост, наклон, recenter и snap-turn; исключить двойной поворот/перемещение.
- [ ] VR-104: сохранить качество графики игры по умолчанию; отдельно управлять разрешением VR и зеркала. Не компенсировать ошибку проекции ухудшением качества.
- [ ] VR-105: собрать время CPU/GPU на кадр и пропуски на Quest 2 / RX 6900 XT; подобрать частоту и разрешение по измерениям, не обещать FPS заранее.

## P1 — руки и инструменты

- [ ] HAND-201: оба контроллера 6DoF, независимые позиции и вращения; проверить координаты, yaw/pitch/roll, леворукость и пропадание трекинга.
- [ ] HAND-202: визуальные кисти и хват без постоянного сдвига, неправильного масштаба и дрожания.
- [ ] HAND-203: разместить игровые инструменты по реальной руке. Направление инструмента, прицела и действия должно совпадать; проверить все штатные инструменты и их переключение.
- [ ] HAND-204: независимый хват каждой рукой, перенос/бросок объектов с корректной скоростью, коллизии и устойчивость физики.
- [ ] HAND-205: двуручное удержание и освобождение инструмента без конфликта с интерфейсом и физикой.

## P2 — меню, интерфейс и удобство

- [ ] UI-301: главное меню, пауза и настройки в VR; наведение, клик, прокрутка и возврат с контроллеров.
- [ ] UI-302: HUD, инвентарь и карта с читаемым масштабом и глубиной; переходы между меню/уровнями без скачков камеры.
- [ ] UI-303: понятное переключение ввода между игрой и панелями; фокус не должен оставлять зажатую стрельбу/хват.
- [ ] UI-304: отдельное широкое зеркало для зрителя, не меняющее проекции шлема.

## P2 — установка и выпуск

- [ ] REL-401: отдельный тестовый стенд, установка с резервной копией и надёжным откатом; не смешивать dsound.dll v1.1.0 и dxgi.dll v0.22.
- [ ] REL-402: матрица проверенных версий игры/runtime/GPU; мини-сценарии для камеры, стерео, каждого инструмента, меню и физики.
- [ ] REL-403: воспроизводимый релиз с лицензиями, инструкцией сборки, установкой и перечнем оставшихся ограничений.

## Порядок проверок

Сначала сборка, статические проверки и диагностика без шлема; затем контролируемая проверка проекций/поз и зеркала. Короткая проверка пользователем в шлеме — только когда есть конкретный проверяемый результат. Ранее пользователь испытывал сильную тошноту; не просить терпеть дискомфорт или многократно проверять случайные настройки.
