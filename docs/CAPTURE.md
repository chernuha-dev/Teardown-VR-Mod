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
