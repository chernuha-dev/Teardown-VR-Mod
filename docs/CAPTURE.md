# DXGI capture: отдельный процесс, без OpenXR

Собственный `tdvr_capture.dll` перехватывает системные CreateDXGIFactory/1/2,
CreateSwapChain и CreateSwapChainForHwnd, связывает swapchain с переданной
DIRECT queue D3D12 и наблюдает Present/Present1/ResizeBuffers. MinHook v1.3.4
зафиксирован по SHA-256, его полная лицензия включена. DllMain не устанавливает
hooks; явный TdvrStartCapture вызывается до создания factory на обычном потоке.
Модуль закрепляется до завершения процесса: динамическая выгрузка не поддержана.

При первом обычном Present берётся текущий backbuffer через IDXGISwapChain3,
в той же очереди выполняются PRESENT → COPY_SOURCE → readback → PRESENT,
fence и проверяемое чтение с учётом row pitch. Вычисляется FNV-1a по пикселям,
не включая padding. Каждый размер читается один раз; успешный ResizeBuffers
сбрасывает состояние capture. DXGI_PRESENT_TEST не инициирует копирование,
неудачный resize не меняет состояние. HDR/MSAA, неоднозначная реализация
COM-метода и неподдерживаемые форматы отвергаются; никаких догадок о stereo.

Ожидание ограничено 2 секундами. При таймауте ресурсы уже отправленной GPU
работы сохраняются до конца процесса, дальнейшее копирование отключается.
RemoveDevice на устройстве игры **не вызывается**. Это диагностическая
задержка, неприемлемая для постоянного VR-рендера; нужен асинхронный GPU путь.
Прямое чтение backbuffer не создаёт независимые проекции или два render pass.

## Автоматическая проверка

В CTest выполняются `dxgi_capture_hwnd`, `dxgi_capture_legacy` и
`dxgi_capture_launcher`. Реальные WARP swapchain создаются в скрытых окнах
отдельных тестовых процессов. Проверяется точный hash 19×11 красных пикселей,
после resize — 23×17 зелёных; оба метода Present/Present1, padded readback,
refusal DXGI resize при удержанном host backbuffer, ABI и повторный startup.
Тесты не запускают игру, OpenXR loader/runtime или шлем.

`tdvr_capture_launcher <development-exe> <tdvr_capture.dll>` создаёт только
собственный дочерний процесс с приостановленным primary thread. Перед main
загружается DLL и явно вызывается startup. Никакого attach к уже запущенным
играм/Roblox. Удалённые адреса системных функций вычисляются относительно
фактических mapping/module base; x64 HMODULE не читается из усечённого DWORD.
При startup failure завершается только созданный этим launcher процесс.
Тестовый injected host не импортирует capture DLL и проверяет её наличие и
готовность к моменту main. Существующая установка игры не изменяется.

## Границы допуска

Capture DLL не использует legacy Lua bridge и не подключает runtime. Она
отказывается накладывать hooks поверх локальных dxgi/dsound proxy, чтобы не
смешивать v0.22/v1.1.0. Проверка на настоящем Teardown ещё не выполнена.
Перед игровым тестом нужен отдельный development layout с отдельным профилем
и подтверждённой изоляцией сохранений/настроек. Не запускать launcher из live
установки как обход правила о согласованном развёртывании.

Остаются: игровой camera/render hook, два render pass из одного состояния,
OpenXR session lifecycle/input и безопасный GPU submit. Этот capture-срез
не является играбельным VR-прототипом.

## Отдельная development-копия и desktop-проверка

`scripts/Stage-DevelopmentGame.py` создаёт обычные копии файлов под ignored
local/, без hardlink/junction. Требует наблюдаемый SHA-256 EXE, не перезаписывает
существующий destination и исключает оба legacy VR DLL, mods, пользовательские
VR INI/логи. Опционально копирует options.xml в отдельный профиль, не меняя
исходные quality settings. Это не установка мода в live game.

У launcher есть необязательный третий аргумент development-profile. До main
он настраивает LOCALAPPDATA/APPDATA/USERPROFILE дочернего процесса и перехватывает
SHGetSpecialFolderPathA для AppData/Documents/Profile. Синтетический host
проверяет и Win32 path, и CRT getenv. Это изоляция известных путей Teardown,
не универсальная OS sandbox: произвольный DLL может пользоваться другими API.

После явного разрешения пользователя desktop-копия была запущена без VR:
получен D3D12 backbuffer 3440×1440, меню и 3D-сцена выводятся. Новый savegame,
options и log находятся под local/game-dev/profile; live options.xml сохранил
прежний размер и LastWriteTime. Первый запуск остановился на зависимости без
её INI; имеющаяся конфигурация зависимости скопирована без изменения, никаких
проверок DRM/лицензирования код не обходит. Игровые файлы/настройки не публикуются.

## Наблюдатель камеры Teardown

`tdvr_engine_probe.dll` — отдельный observational module. Проверяет полный
SHA-256/размер EXE и префикс инструкции в уже загруженной игре до установки
единственного camera hook. Неизвестный EXE или несовпадение сигнатуры дают
ERROR_NOT_SUPPORTED. В наблюдаемом EXE дисковые байты этого RVA отличаются
от байтов после обычного запуска; механизм преобразования не исследован.
Проверять такой префикс по диску и считать его runtime-функцией нельзя.
Никаких действий по обходу защиты не выполняется.

Camera setter вызывает оригинал с исходными аргументами, затем наблюдает
позицию/кватернион и engine view matrix. Статическая math library строит
inverse view, сравниваемый с column-major матрицей движка. В desktop-сцене
проверено 183 valid samples / 0 rejected, максимальная разница элементов
последней наблюдаемой view ~1.58e-6. Это подтверждает соответствие соглашения
поз этой функции; projection override, выбор игровой камеры, stereo replay
и VR input этим не реализуются.

`scripts/Probe-DevelopmentCamera.py --pid <development-pid>` загружает module
только в teardown.exe внутри ignored local/ с наблюдаемой идентичностью;
`--stats-only` читает статистику. Live-процесс/Roblox отвергаются до загрузки
DLL. Модуль закрепляется до конца процесса, поэтому для пересборки/замены
нужно завершить соответствующую development-копию. Локальные memory snapshots
для исследования хранятся только под local/ и не входят в Git.

Автоматический итог этого среза: CTest 11/11, Python unittest 9/9.
engine_identity_gate отвергает собственный test host до любых engine hooks.

## Поиск world-render boundary через GPU recording

`tdvr_capture_launcher ... [development-profile] --trace` явно включает
перехват DrawInstanced / DrawIndexedInstanced / Dispatch. По умолчанию эти
hooks не устанавливаются. В момент association создаётся незапускаемый
command list на фактическом game device для получения методов backend.
Все реальные команды вызываются с исходными аргументами, без повторения.
Поддержан один backend implementation за процесс; hook_mask показывает
частичный/полный успех независимо от capture stats.

Сохраняются только return RVAs основного EXE: первые 64 команды, затем
каждая 64-я до 65536 команд, не более 64 уникальных стеков по 24 RVA.
`observations` — число выборочных наблюдений, не число GPU draws. Нет symbol
download, адресов сторонних модулей, путей пользователя или автоматической
записи файлов. Перехват остаётся до выхода процесса. Он предназначен для
диагностики и имеет CPU overhead; для измерения FPS его нужно выключить.

Для уже запущенной development-копии (не live) можно прочитать/перевооружить
окно наблюдения, обязательно указав **тот же файл DLL**, что использовал launcher:

```powershell
python scripts/Probe-DevelopmentCamera.py --pid <development-pid> --dll build/native/Release/tdvr_capture.dll --render-trace --reset-trace
python scripts/Probe-DevelopmentCamera.py --pid <development-pid> --dll build/native/Release/tdvr_capture.dll --render-trace
```

Trace mode не загружает capture поздно и не меняет позы. Проверяются путь
процесса под local/, SHA EXE и точный путь загруженной DLL; несовпавшие export
offsets не вызываются. Reset меняет generation и не переустанавливает hooks.

CTest `d3d12_recording_trace` выполняет настоящие WARP draws: левую половину
кадра через DrawInstanced, правую через DrawIndexedInstanced, с проверкой всех
пикселей до/после resize; дополнительно выполняет no-op compute dispatch.
Проверяются все три вида стеков, opt-in, ABI и reset. Итог: CTest 13/13,
Python 9/9. Для параллельной сборки без замены загруженной DLL поддержан
`scripts/Build-Native.ps1 -BuildDirectory build/native-trace`.

В desktop development-копии действительно собраны Draw/Dispatch stacks в
меню и на вилле. Они показывают вызов world renderer 0x865b0 из 0x6fd52
(return 0x6fd57), внутри функции 0x6f770. Следовательно, 0x6f770 включает
рендер после настройки камеры, а не только camera setup. Стерео ещё не
повторяется; собственные кадры в OpenXR не отправляются.

`tdvr_engine_probe` теперь наблюдает также world render 0x865b0. Оба runtime
prefix и EXE SHA проверяются до установки обоих hooks одной queued операцией.
Новые `TdvrGetWorldRenderStats` / CLI `--world-render [--stats-only]` показывают
завершённые вызовы, валидность camera record, совпадение с Game+0x240,
изменения camera/serial/history и поток. Выборка — каждый 16-й вызов;
original вызывается один раз с исходными аргументами, без удержания stats mutex.
В desktop-сцене подтверждены 780 завершённых вызовов и 49 корректных samples;
подробные ограничения и side effects — ENGINE_COMPATIBILITY.md.

## Наблюдение render targets

Opt-in `--trace` дополнительно устанавливает hooks CreateRenderTargetView,
CopyDescriptors/CopyDescriptorsSimple и OMSetRenderTargets. Исходные вызовы
всегда передаются один раз с прежними аргументами; GPU work не добавляется.
Наблюдение рассчитано на один device/backend за процесс. Сохраняется таблица
до 512 CPU descriptors с GetDesc ресурса и serial последней записи. Копирование
дескриптора инвалидирует метаданные назначения: такой binding отмечается
unknown, а не получает устаревший формат. Null resource также unknown.
Ресурсы не удерживаются по descriptor и не разыменовываются после CreateRTV.

```powershell
python scripts/Probe-DevelopmentCamera.py --pid <development-pid> --dll build/native/Release/tdvr_capture.dll --target-trace --reset-trace
python scripts/Probe-DevelopmentCamera.py --pid <development-pid> --dll build/native/Release/tdvr_capture.dll --target-trace
```

Окно наблюдения: первые 32 bindings, затем каждый 64-й до 65536. До 32
уникальных записей, 8 RT attachments и 12 EXE return RVAs; dropped показывает
переполнение. Группировка использует shape/caller, исключая write serial:
движок может переписывать один descriptor каждый проход. Поле creation относится
к первому sample группы, это **не** токен владения или lifetime ресурса.
Reset очищает samples/counters, сохраняя текущую descriptor table и serial.

Width/height — полные размеры ресурса, не viewport/mip extent. view_format
содержит исходный Format (0 = DXGI_FORMAT_UNKNOWN, наследуется от ресурса),
либо resource Format при null pDesc; правило наследования описано в
[Microsoft D3D12_RENDER_TARGET_VIEW_DESC](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_render_target_view_desc).
Depth target, subresource, resource state, command-list submission/fences и
соответствие конкретному eye здесь не отслеживаются.

WARP test проверяет contiguous/array MRT, порядок attachments, обе формы
descriptor copy, повторные записи, resize, opt-in/ABI/reset и точные пиксели.
В desktop development-процессе предыдущая сборка этого среза дала hook_mask=15,
HRESULT=0; после reset видны четыре RT 3440×1440 (DXGI formats 29/31/28/34)
на stack 0x86f46 → 0x6fd57 и последующие одноцелевые проходы. Начальное окно
заполнилось повторными descriptor writes; группировка без serial исправлена
и проверена на WARP отдельно, но обновлённая DLL ещё не загружалась в игру.
Нет eye GPU copy, stereo replay или XR submit; per-eye histories не изолированы.
