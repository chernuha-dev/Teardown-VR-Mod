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
