# Нативная сборка и диагностический стенд DEV-004

`tdvr_math.dll` — собственное математическое ядро, **не игровой VR-модуль**.
Оно не экспортирует DXGI, не сканирует память, не создаёт OpenXR session,
не перехватывает игру и не исправляет живую v1.1.0.

## Зависимости и запуск

Windows x64, Visual Studio 2022 Build Tools с C++ x64/x86, Windows SDK,
CMake >= 3.24 и CTest. У математического ядра внешних библиотек нет.
Стенд использует OpenXR SDK release-1.1.53 с проверяемым SHA-256 архива
(cmake/OpenXR.cmake); первая сборка требует сети. Loader собирается статически,
JsonCpp берётся из того же архива. Лицензии копируются к executable;
оригинальные LICENSE и THIRD-PARTY-NOTICES.txt сохранены.
GPU-проверка требует ID3D12Device5::RemoveDevice для ограниченного по времени
аварийного завершения: Windows build >= 20348; здесь проверено на 22631.
Для сборки одной математики: `-DTDVR_BUILD_DIAGNOSTICS=OFF`.

```powershell
powershell -NoProfile -File scripts/Build-Native.ps1
python -m pip install -r tests/requirements.txt
python -m unittest discover -s tests -v
```

Скрипт находит CMake в PATH либо через vswhere; генератор фиксирован на
Visual Studio 17 2022, платформа x64, конфигурация Release. При ошибке
configure/build/test возвращается ошибка. Результаты в исключённой из Git
`build/native/Release/`. Для чистой независимой сборки можно использовать:

```powershell
cmake -S . -B build/native-clean -G "Visual Studio 17 2022" -A x64
cmake --build build/native-clean --config Release
ctest --test-dir build/native-clean -C Release --output-on-failure
```

Проверено здесь 2026-11-16: MSVC 19.44.35228 (toolset directory 14.44.35207),
CMake 3.31.6-msvc6, MSBuild 17.14.51, Windows SDK 10.0.26100.0,
Python 3.12 и lupa 2.6 / Lua 5.1. Зависимости системы не устанавливались;
для Lua-тестов установлена указанная версия lupa.

## Реализованное и проверка

- Явный ABI с double, поза xyz + quaternion xyzw, +Y вверх / -Z вперёд.
- Композиция world = base * scaled tracking pose: масштаб только переноса.
- Асимметричный frustum из четырёх signed FOV в радианах; near/far в метрах.
  Row-major матрица умножается на column vector; right handed D3D depth [0,1].
  Разрешение монитора не является входом. Viewport и ориентация текстуры при
  реальном рендере ещё не определены.
- Проверка finite, единичного quaternion, положительного масштаба, порядка
  углов/плоскостей. При ошибке выход не изменяется.
- CTest выполняет Release executable, связанный с реальной собранной DLL:
  yaw+pitch composition, масштаб/перенос, четыре края frustum обоих глаз на
  трёх глубинах, near/far depth, ошибочные входы. Проверки не используют assert,
  поэтому работают при NDEBUG.
- Lua-тест загружает полный `mod/main.lua` в Lua 5.1; проверяет bridge init и
  `updateActive` с присутствующим/отсутствующим DisableMotionBlur, подменяя
  только engine-facing зависимости. Это не эмуляция Teardown physics/render.

DEV-004 остаётся открытой для интеграции OpenXR/D3D12/движка. Никаких
измерений FPS, комфортности, stereo timing или работы инструментов этот
срез не даёт. Следующие проверки проводить без повторных тестов пользователя
в шлеме до появления конкретного измеримого результата.

`scripts/Verify-Baseline.ps1` сравнивает рабочие файлы с первоначальным
импортом и после изменения Lua закономерно сообщает отличие main.lua.
Оригинальные SHA-256 и commit ef5b657 сохраняются, манифест не переписывается.

## Диагностика без шлема (2026-11-16)

```powershell
build/native/Release/tdvr_diagnostic.exe --gpu --warp
build/native/Release/tdvr_diagnostic.exe --gpu
build/native/Release/tdvr_diagnostic.exe --runtime
build/native/Release/tdvr_d3d12_tests.exe --hardware
```

Без аргументов / с `--help` выводится справка. `--gpu` не вызывает OpenXR.
Создаются direct queue и два отдельных RGBA8 render targets 17×13; каждый
очищается своим цветом, переводится barrier в COPY_SOURCE и копируется в свой
readback buffer. После fence проверяются все 221 пиксель каждого глаза с учётом
RowPitch=256. Это проверка операций GPU, а не рендер сцены/проекций.
Ожидание fence ограничено 5 секундами; при отказе незавершённая работа
прерывается RemoveDevice перед освобождением ресурсов. Этот код допускается
только в отдельном диагностическом процессе, не внутри игрового рендерера.

`--runtime` может запустить установленный runtime. Он создаёт OpenXR instance,
проверяет XR_KHR_D3D12_enable, HMD system, primary stereo и размеры двух views,
получает обязательные adapter LUID и minFeatureLevel. Затем GPU-проверка
выбирает точно этот адаптер, без подмены WARP/другой видеокартой. Instance
освобождается и при промежуточных ошибках. Коды: 0 — preflight/GPU проверка
успешна, 2 — runtime/HMD/возможность недоступны, 1 — ошибка. Код 0 не означает
готовность игрового VR. Режим `--runtime` не создаёт session/swapchains.

Фактически проверено: SteamVR/OpenXR 2.18.4, HMD system `oculus`, требование
FL 12_0; RX 6900 XT поддерживает FL 12_2 и проходит readback. Рекомендованные
runtime размеры 3664×4028 для каждого глаза только прочитаны; настройки
разрешения/качества не менялись. Результат не измеряет FPS и комфорт.

CTest: 4/4 (исходная математика, stereo packet, fake OpenXR, WARP readback).
Python: 8/8 (3 Lua + 5 PE). Fake OpenXR проверяет успех, отсутствие runtime/HMD/
extension/stereo, неправильные view sizes/count, null proc, ошибки требований/
cleanup и ограниченные повторы при изменении enumeration count. Он не загружает
реальный runtime, поэтому обычная сборка не требует шлема и SteamVR.

## Контракт stereo packet

`tdvr_build_stereo_frame` строит принадлежащий вызывающему коду пакет с двумя
независимыми view/projection. Оба входных глаза обязаны иметь общий simulation
id и predicted display time; требуется валидное отслеживаемое положение и
ориентация. Ошибка одного глаза оставляет весь выход неизменным. Кватернионы
нормализуются в пределах допуска, масштаб применяется к переносу и clip planes.
Tracking origin задаётся явно: нельзя повторно прибавлять абсолютную высоту
шлема к уже заданной игровой высоте глаз. Тесты проверяют известные мировые
точки, асимметричные frustum edges, inverse view, разные ориентации глаз, IPD,
масштаб, высоту и отказ при смешивании меток кадров.

Общий id — проверяемый контракт производителя данных, а не доказательство
заморозки игры. Публикация между потоками, реальный OpenXR locate/predict и
два render pass из одного состояния движка ещё не реализованы.

Описание требований API:
[OpenXR D3D12 requirements](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrGetD3D12GraphicsRequirementsKHR.html),
[copy footprints](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getcopyablefootprints),
[RemoveDevice](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device5-removedevice).

## Проверка выделения session/swapchains

`tdvr_diagnostic.exe --session` — отдельная **ручная runtime-проверка**,
не входящая в CTest. Не запускать, когда пользователь занят другим VR-приложением
или попросил тестировать без обращения к шлему. Обычная сборка и fake XR тесты
не вызывают OpenXR runtime.

После graphics requirements callback сохраняет instance/system живыми,
создаёт D3D12 device на точном LUID и DIRECT queue, передаёт binding в
xrCreateSession. Из runtime formats выбирается RGBA8 sRGB (либо UNORM),
затем создаются две независимые swapchain 64×64, sampleCount=1. Перечисляются
ненулевые D3D12 image pointers, затем освобождаются обе swapchain, session
и instance в обратном порядке. BeginSession, acquire/wait/release images,
locate views, кадры и GPU-запись в runtime images этим режимом не вызываются.

Фактический единичный запуск 2026-11-16 успел закончиться при поступлении
пользовательского запрета runtime-проверок: SteamVR/OpenXR 2.18.4, format 29,
по 3 image на глаз, два успешных destroy swapchain и destroy session/instance.
Кадры не отправлялись. После запрета выполнялись только локальные проверки.
Это подтверждает выделение ресурсов стенда, не игровое VR и не комфорт.

Fake session tests проверяют D3D12 binding, порядок lifetime, форматы,
ошибку создания правого глаза, null/duplicate handles/images, ограничения
enumeration и ошибки cleanup. Callback test также проверяет освобождение
instance при исключении и отсутствие callback до graphics requirements.

API: [xrCreateSession](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrCreateSession.html),
[xrCreateSwapchain](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrCreateSwapchain.html).

## Локальная проверка алгоритма кадра

`tdvr_frame_loop` принимает dispatch, уже работающую stereo session, reference
space и два живых target. Он не запускается из tdvr_diagnostic и не создаёт
session самостоятельно. CTest `openxr_stereo_frame_fake` подменяет все XR вызовы;
реальный loader/runtime в этот executable не линкуется.

За кадр вызывается ровно один LocateViews на predictedDisplayTime, оба глаза
переводятся в принадлежащий вызывающему коду stereo packet с единым simulation
id/time. Callback отрисовки вызывается один раз после успешного acquire/wait
обоих targets. Он обязан завершить GPU работу и вернуть изображения в состояние,
требуемое runtime, до возврата/исключения. Затем release обоих images и EndFrame
отправляют одну projection layer с двумя views в исходном reference space.

При shouldRender=false или потере tracking кадр закрывается без слоя. Невалидные
позы не подменяются старыми. Positive XR_TIMEOUT_EXPIRED не считается успехом:
ожидание image ограничено 5 ms, не дождавшийся image не release-ится незаконно,
а report требует прекратить кадры и уничтожить session вместе с её ресурсами.
Ошибка правого глаза/renderer/release не отправляет левый глаз отдельно.

Fake tests проверяют порядок всех вызовов, метки времени, отдельные FOV/targets,
no-render/tracking-loss, частичный acquire, timeout обоих глаз, неверный индекс,
исключение renderer, ошибки release/end, XR_FRAME_DISCARDED и session loss.
Итог: CTest 6/6 и Python 8/8. Ни один из этих XR tests не использует шлем.

Остаются session event lifecycle (READY/STOPPING/LOSS_PENDING), реальные spaces/
input, GPU renderer с barriers/fences, источник одного состояния игры и hook.
Наличие алгоритма не доказывает синхронное игровое стерео или комфорт.

## Перехват DXGI

Сборка дополнительно создаёт tdvr_capture.dll, tdvr_capture_launcher и два
WARP host executable. MinHook v1.3.4 загружается по hash из cmake/MinHook.cmake,
лицензии копируются к DLL. CTest теперь 9/9, Python 8/8; детали — CAPTURE.md.
Все capture-тесты используют свой WARP-процесс, без OpenXR и шлема.

## Адаптер игровой камеры

`tdvr_engine_camera` преобразует готовый stereo packet в два camera records
наблюдавшегося формата Teardown: column-major float projection/view и near/far
по +0x80/+0x84 (всего 0x88 bytes). Преобразование атомарно относительно выходного
пакета: сначала проверяются оба глаза, включая float range и сохранение finite
near/far depth. При отказе выход остаётся прежним. Near/far восстанавливаются
из исходной double projection, уже с world scale; повторный scale не применяется.

`TdvrEngineCameraScope` сохраняет и восстанавливает ровно этот record, включая
выход по исключению. Это строительный блок будущего render hook: он сам не
выбирает игровую камеру, не обновляет внешние frustum/temporal caches, не
синхронизирует потоки/симуляцию. Теперь вызывается ограниченным opt-in
диагностическим world-render hook, описанным ниже.

CTest `engine_camera_layout` проверяет независимые asymmetric FOV, аналитические
world points с yaw и переносом, все frustum edges на трёх глубинах, depth [0,1],
scaled clip fields, отказ при переполнении float/потере depth precision,
неизменность пары при ошибке и побайтное восстановление камеры с guard bytes
при обычном выходе/исключении. Эти проверки не запускают игру или OpenXR.

Если development-процесс удерживает DLL, не пытаться переписывать загруженный
модуль: `powershell -NoProfile -File scripts/Build-Native.ps1 -BuildDirectory build/native-trace`
создаёт независимое дерево сборки внутри repo/build. Параметр допускает
относительный или абсолютный путь только в этом каталоге. Для новой DLL
нужен следующий запуск development-копии. Opt-in `--trace` и команды чтения
runtime stacks описаны в CAPTURE.md; обычные CTest не вызывают OpenXR.

## Ограниченная подмена камеры в development-копии

После запуска отдельной копии через capture launcher с отдельным профилем:

```powershell
python scripts/Probe-DevelopmentCamera.py --pid <development-pid> --world-render
python scripts/Probe-DevelopmentCamera.py --pid <development-pid> --camera-test 2 0 0 --test-yaw 20
python scripts/Probe-DevelopmentCamera.py --pid <development-pid> --override-stats
python scripts/Probe-DevelopmentCamera.py --pid <development-pid> --cancel-camera-test
```

`--camera-test X Y Z` задаёт локальный перенос в метрах относительно последней
наблюдавшейся позы. `--test-eye left|right` выбирает одну камеру из проверенной
пары; `--test-fov LEFT RIGHT UP DOWN` задаёт signed angles в градусах (по
умолчанию сохраняется наблюдавшийся frustum). Это synthetic input, не OpenXR.
CLI проверяет путь/identity EXE и точный путь загруженной DLL; native дополнительно
требует EXE под local/ того же repo и TDVR_DEV_PROFILE. Запрос живёт не более
10 секунд / 600 render calls. При ошибке обоих eye packets прежний запрос
не заменяется; отмена не требует валидного tracking packet.

Hook подменяет только writable основную камеру на наблюдавшемся render thread,
вызывает оригинал ровно один раз и проверяет побайтное восстановление record.
Он не повторяет симуляцию/world render, не изолирует temporal histories и не
подключает XR session. 3D-фон меню также использует основную камеру: диагностика
не является определением gameplay state. Проверено на desktop-вилле:
перенос 2 м / yaw 20°, 600 applied / 600 restored, 0 rejected, возврат исходного
вида после исчерпания бюджета. CTest 14/14, Python 9/9; в шлем не отправлялось.

## Сохранение результатов GPU-проходов

`tdvr_eye_copy::record_eye_copy` записывает копию всей 2D texture в другой
ресурс на уже открытом DIRECT command list. До записи проверяет COM identity,
общий device списка/обоих ресурсов, одинаковые extent/format, один mip/layer/
sample и допустимые declared states. Поддержаны только RGBA/BGRA8 UNORM/SRGB
с точным совпадением format: нет scaling или gamma conversion. Source переводится
в COPY_SOURCE, destination — в COPY_DEST, затем оба возвращаются в указанные
исходные состояния. Владелец обязан знать настоящие состояния и исключить
overlapping placed allocations: helper не ведёт global resource-state tracker.

Helper не выполняет ExecuteCommandLists, не ждёт fence и не передаёт владение
OpenXR. Вызывающий код удерживает ресурсы и ждёт завершения на фактической
queue до повторного использования или XR release. Эти обязанности ещё не
подключены к игровому engine hook.

Standalone D3D12 probe теперь отрисовывает red/green последовательно в одном
источнике, копирует каждый результат в отдельную committed texture, затем
перезаписывает источник blue. Только после этого записывает readback всех
трёх ресурсов, submit/fence и проверяет каждый пиксель с row-pitch padding.
Проверяются отказы для alias, null list, неверного state, extent и format.
Hardware run дополнительно проверяет отказ копии AMD → WARP. Повторное
D3D12CreateDevice для одного adapter не считается другим device — это
[per-adapter singleton](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12createdevice).
WARP-only run честно выводит cross-device rejection=0 (этот случай пропущен).
Это проверка сохранения synthetic images, не синхронности игрового стерео.

Проверено: CTest 14/14, Python 9/9 и
`build/native/Release/tdvr_d3d12_tests.exe --hardware` на RX 6900 XT:
221 pixels per eye, row pitch 256, cross-device rejection=1. Ни один из этих
запусков не создаёт OpenXR session и не управляет игровым окном.

## Opt-in плоская OpenXR-панель

Собственная `tdvr_capture.dll` теперь содержит OpenXR loader и `tdvr_xr_panel`.
Загрузка DLL и обычные CTest не вызывают runtime. Только явный `--xr-panel`
в launcher включает XR на первом подходящем игровом Present:

```powershell
build/native-trace/Release/tdvr_capture_launcher.exe <development-exe> <absolute-tdvr_capture.dll> <development-profile> --xr-panel
```

Это ручной режим с обращением к шлему. Не запускать рядом с Roblox или другим
VR-приложением и не совмещать с dsound/dxgi proxy. Для собственной проверки
development-копия должна содержать только наш capture module, без авторской DLL.
Живая установка не является целью этой команды. После разрешения пользователя
проверено в отдельном development-процессе; управление мышью не используется.

Путь: текущий backbuffer / PRESENT на известной DIRECT queue → собственный
command list → SDR byte copy в XR color image → восстановление PRESENT /
RENDER_TARGET → submit на той же queue → CPU fence wait → XR release → EndFrame
с одним quad для обоих глаз. Нет масштабирования или gamma conversion.
XR binding использует игровой device/queue и требует совпадения runtime LUID
и feature level. LOCAL space, панель шириной 3 м на расстоянии 2.5 м, высота
по aspect ratio источника. Это одна плоская текстура, не независимые глаза,
не head/controller tracking в игре и не синхронный stereo world-render.

Обрабатываются READY/STOPPING/EXITING/LOSS_PENDING, shouldRender=false,
XR_FRAME_DISCARDED. Ожидание image ограничено 5 ms, GPU fence — 2 секундами.
При timeout/ошибке панель прекращает новые кадры; незавершённые GPU allocations,
source reference и XR handles удерживаются до конца процесса. RemoveDevice
не вызывается. Обычный game Present сохраняется. Resize/смена queue требуют
нового запуска этого диагностического режима; автоматического recovery пока нет.
xrWaitFrame остаётся синхронным runtime-вызовом на Present thread.

`TdvrGetPanelStats` (size/ABI=2) сообщает stage, frames/submitted, XR/HRESULT,
quarantine; launcher печатает изменения стадии и каждые 60 submissions.
Stage: 0 idle, 1 init, 2 events, 3 frame, 4 copy, 5 submit, 6 stopped, 7 failed.
Успешный submission не равен визуальной проверке в шлеме.

CTest `openxr_flat_panel_fake_warp` не линкует loader. Подменённый XR проверяет
binding/точные format и extent, кадры, shouldRender=false, STOPPING/EXITING,
отказ runtime/LUID/format, timeout, неверный image index, EndFrame failure,
resize, ошибку cleanup и отсутствие автоматического повтора. WARP выполняет
настоящую копию 19×11 и readback всех 209 пикселей на границе fake release.
Это проверка копирования и порядка вызовов; она не моделирует очередь compositor
или зависание физического GPU. Обычные capture hosts дополнительно проверяют
нулевые panel stats по умолчанию, ABI и запрет позднего включения XR.

Первоначальный срез: сборка build/native-trace, CTest 16/16, Python 12/12.
Результат последующего реального запуска описан ниже.
Правило resource states:
[Khronos D3D12 swapchain state](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_D3D12_enable-swapchain-image-state.html).

### Первый игровой запуск и SDR/typeless (2026-11-17)

Через VR_PATHREG_OVERRIDE на отдельную копию openvrpaths.vrpath игра создала
OpenXR instance и D3D12 session на SteamVR 2.18.4. Исходный файл не менялся.
Это устранило наблюдавшийся -51 без изменения ALVR/SteamVR settings. Первый
запуск отказал на format 28 (-26); следующий создал XR swapchain format 29
3440×1440 и начал один кадр, но recorder вернул E_INVALIDARG до GPU submit.
На этих двух неудачных запусках кадров не отправлено. Затем управление
окнами остановлено Escape; последующая проверка использует собственный API.

Обнаружен дефект: XR D3D swapchain имеет конкретный declared format, но его
ресурс возвращается typeless при наличии такого аналога — это прямо описано в
[Khronos rendering spec](https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/rendering.adoc).
WARP regression с TYPELESS target воспроизвёл отказ старого recorder, затем
прошёл после исправления. Последующий игровой запуск подтвердил storage 27
(R8G8B8A8_TYPELESS), source 28 и declared XR format 29.

Отдельный record_sdr_display_copy требует DXGI SDR/sRGB color space и явный
SRGB format, с которым создана XR swapchain. Он допускает только ту же RGBA8 /
BGRA8 format family, включая typeless storage; bytes переносятся без повторного
кодирования gamma. record_eye_copy сохраняет прежнюю строгую проверку exact
format. Capture наблюдает успешные SetColorSpace1 (default 8-bit DXGI — sRGB);
HDR/linear/неизвестные пространства отвергаются. Ничего не устанавливает через
SetColorSpace1 сам. Обоснование default:
[Microsoft Advanced Color](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range),
совместимость copy formats:
[CopyTextureRegion](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion).

WARP readback проверяет полутона 64/128/191/255, typeless target и отсутствие
RGBA/BGRA swizzle; также отказ HDR/linear и сохранение строгого eye-copy gate.
Добавлены source / declared XR / storage formats, color space и failure stage
в panel stats ABI 2. После сообщения пользователя о закрытии Roblox новая
сборка проверена с настоящей игрой и SteamVR: через собственный named pipe
получено 12080 frames / 12080 submitted, stage 5, XR_SUCCESS, GPU S_OK,
quarantined=0, color space=0. Это успешный приём плоской панели runtime,
не визуальная проверка, не независимое стерео и не тест комфорта.
Итоговые локальные проверки: CTest 17/17, Python 16/16.


### Позы глаз в собственной XR session

XrPanel получает два XrView одним xrLocateViews после BeginFrame, до acquire,
на predictedDisplayTime текущего кадра и в LOCAL space. Позы и независимые
signed FOV проверяются существующим tdvr_build_stereo_frame; невалидные числа,
quaternion или FOV не публикуются как доступный tracking. При valid, но
untracked flags весь tracking packet недоступен; плоская панель продолжает
работать. No-render, STOPPING и terminal error сбрасывают доступность.
Ошибка LocateViews/неверное число views завершает frame без слоя и останавливает
панель. Подробности API: [Khronos xrLocateViews](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrLocateViews.html).

TdvrGetXrViews принимает size/ABI=1 (216 bytes), ничего не запускает и копирует
snapshot под тем же mutex, который сериализует Present. CLI/MCP xr-views
предоставляет эти данные без управления мышью. Frame id относится к XR,
не доказывает синхронность симуляции; getter не является freshness monitor.

Новые fake XR/WARP проверки покрывают точное время/space, один LocateViews
на пару, отдельные положения и asymmetric FOV, потерю tracking, NaN/FOV,
восстановление, no-render/STOPPING, session loss и неверное число views.
Capture host дополнительно проверяет null/size/ABI и отсутствие поз по умолчанию.
Собственная камера пока не использует эти позы: LocateViews выполняется на
Present после игрового world render. Для полноценного VR нужно перенести
начало XR-кадра перед render boundary, связать два прохода/targets и histories
с одним состоянием симуляции. Режим остаётся плоской панелью.
Новая сборка проверяется без реальной игры/runtime, пока пользователь играет
обычную миссию; ранее подтверждённая в шлеме панель была предыдущей сборкой.

Итог среза: Build-Native.ps1 -BuildDirectory build/native-trace, CTest 17/17,
Python 16/16. Staging unittest теперь подменяет disk_usage и отдельно проверяет
отказ до создания destination при недостатке места; реальный disk-space gate
не ослаблен (0ef867e). Новый XR path не запускался в игре или шлеме.


### Преобразование located XR pair в камеры движка

Общий tdvr_prepare_tracked_cameras принимает owned TdvrXrViewSnapshot и
контекст текущего render frame. Контекст отдельно задаёт ожидаемые XR frame id /
predicted time, настоящую метку simulation, world_from_tracking и clip/scale.
Adapter не принимает недоступный/ошибочный tracking, другой кадр или timestamp,
нулевое XR time и неверный ABI. Обе камеры готовятся атомарно, output при отказе
не меняется. XR frame number не подменяет game simulation id.

Позиции глаз уже включают IPD; преобразование не добавляет его повторно.
World origin задаётся явно, а не выводится из игровой позиции глаза плюс
стоящая высота HMD. Adapter сам не реализует recenter, не подтверждает freshness
на часах, не замораживает игру и не даёт владение render frame. Если вызывающий
код бездумно скопирует expected id/time из старого snapshot, проверка равенства
не обнаружит возраст: lifecycle остаётся обязанностью владельца XR-кадра.

XrPanel использует тот же adapter для проверки доступности обеих камер в
float-формате движка, с identity tracking origin и diagnostic clip distances.
Результат там не устанавливается в игру. Это validation-only вызов, без
настоящей simulation identity. Никакой второй render pass этим не запускается.
Код adapter/engine conversion статически включён в capture DLL; новая зависимость
на tdvr_math.dll для игрового процесса не добавляется.

CPU test tracked_engine_camera_pair проверяет реальные world points на границе
frustum для обоих FOV, yaw origin 90° + head 60°, положение/высоту/масштаб 2,
scaled clips, отдельную simulation key и 13 отказов, включая stale frame/time,
NaN справа и потерю depth precision при переводе в float. Ни один из этих
тестов не обращается к OpenXR runtime или игре.

Проверено: Build-Native.ps1 -BuildDirectory build/native-trace, CTest 18/18,
Python 16/16. PE import audit собранной tdvr_capture.dll подтвердил отсутствие
зависимости на tdvr_math.dll. Игра/OpenXR/runtime в этом срезе не запускались.


### Opt-in XR begin перед world render

Новый режим --xr-world-timing требует development profile. Запускать только
после разрешения пользовательских runtime-проверок, без другой VR-игры:

```powershell
build/native-trace/Release/tdvr_capture_launcher.exe <development-exe> <absolute-tdvr_capture.dll> <development-profile> --xr-world-timing
python scripts/Probe-DevelopmentCamera.py --pid <development-pid> --dll build/native-trace/Release/tdvr_engine_probe.dll --world-render
```

Это подготовка timing, а не включение stereo world rendering. Первый Present
только создаёт device-bound XR session/swapchain и обрабатывает READY. Без
подключённого engine observer кадры не начинаются. Child-only environment flag
TDVR_XR_WORLD_TIMING=1 устанавливается собственным capture configuration export;
настройки Windows/SteamVR/ALVR и родительский процесс не меняются.

Hash/signature-gated engine hook дополнительно требует development_process,
валидную основную camera record и наблюдаемый render thread. Он вызывает
TdvrBeginXrWorldFrame перед оригинальным world render, без engine stats mutex.
Capture под своим mutex выполняет Wait/Begin/LocateViews один раз. Оригинальная
игровая функция вызывается по-прежнему один раз, камера/history не меняются.
Present затем копирует обычный backbuffer в плоскую XR-панель и делает EndFrame
для уже начатого кадра; нового Wait/Begin на Present в этом режиме нет.

Дублирующий begin при открытом кадре возвращает S_FALSE, не открывая новый.
Present без world begin не отправляет новый кадр. Begin до graphics initialization
не запускает runtime. World begin и Present должны быть на том же потоке,
иначе режим останавливается до начала XR-кадра. STOPPING не обрабатывается
между Begin и End: события проверяются на следующей границе. Незавершённый
CPU frame при cleanup закрывается пустым EndFrame, сохраняя GPU quarantine.

Это ещё не привязка глаза к игровой камере, не simulation freeze и не два
прохода с отдельными histories. Не доказано, что один main-camera world call
на конкретной сцене соответствует единственному игровому кадру: дополнительный
вызов до Present просто пропускает begin, и camera consumer не должен считать
его новым XR frame. Resize/смена queue по-прежнему останавливают режим.

Fake XR/WARP тесты проверяют pre-world LocateViews → copy/release/End на Present,
повторный begin/Present, no-render, несовпадение потока, STOPPING после Begin,
copy timeout и cleanup открытого frame. Обычный capture host проверяет отказ
включения timing без profile и отсутствие XR begin в обычном режиме.
Новый режим в настоящей игре/runtime пока не запускался: пользователь играет
миссию, и запрет остаётся до его сообщения, независимо от прошедшего времени.

Проверки среза: Build-Native.ps1 -BuildDirectory build/native-trace, CTest 18/18,
Python 16/16; launcher дополнительно отклоняет --xr-world-timing без profile
до чтения EXE/создания дочернего процесса (exit 1). Ни одна проверка нового
режима не запускала настоящую игру/runtime/шлем.


### Offline temporal compatibility audit

```powershell
python scripts/Inspect-TemporalCompatibility.py --game local/game-dev/game/teardown.exe --snapshot local/engine-audit/runtime-text.bin --reference local/reference/v1.1.0/dsound.dll
```

Только чтение трёх файлов с известными полными SHA; процесс игры не открывается.
Скрипт выдаёт JSON в stdout и ничего не пишет. Исходные файлы не входят в Git.
Реальные сохранённые файлы прошли аудит: world offsets 0x43/0x293/0x36d,
TAA create 0xa41a0, три release CALL → 0x5dde10, buffer release 0x5bd420.
Это адреса-кандидаты для дальнейшего анализа ABI, не разрешение вызывать их.
Подробности resource contract reference DLL — V110_COMPATIBILITY.md.


### Read-only temporal observer

Engine probe собирает before/after CPU observations вокруг каждого 16-го
валидного main-camera world render. TdvrGetTemporalStats: size/ABI=1, 296 bytes.
Базовые serial/history/reset должны читаться полностью; иначе sample rejected.
Опциональные 10 scene slots, opaque descriptor prefix 0x38 и 5 TAA slots имеют
раздельные readable/presence masks. TAA first word не объявляется доказанным
счётчиком кадра. Scene/TAA pointer changes сравниваются только при читаемых
slots с обеих сторон; descriptor changes — точным сравнением bytes при обоих
readable descriptors. Наружу выходят лишь маски и fingerprints, без указателей.

Helper получает функцию безопасного чтения и проверяет overflow адресов до её
вызова. Engine использует ReadProcessMemory внутри собственного процесса;
не разыменовывает TAA resources, не вызывает COM, allocator, destructor или
renderer idle и не пишет memory. Чтение может совпасть с изменением памяти
другим потоком: это наблюдение, не atomic state или GPU ownership proof.
Потребитель обязан проверять last_sample_valid и individual masks; сериализация
engine-status не выдаёт before/after после отказа последнего sample.

CPU test моделирует 10 descriptor regions, 5 opaque TAA handles, serial/history
swap, pointer replacement, изменение последнего descriptor byte, исчезновение
optional objects, отказ basic read и address overflow. Проверяется неизменность
исходной памяти и отсутствие чтения opaque GPU handles. Engine gate проверяет
null/size/ABI и нулевые stats неизвестного EXE без установки hooks.
Observer встроен в audited engine hook, но новая сборка в настоящей игре ещё
не загружалась: пользовательская миссия продолжается, runtime не запускается.

Проверено: Build-Native.ps1 -BuildDirectory build/native-trace, CTest 19/19,
Python 21/21. Настоящая игра/runtime/шлем в этом срезе не запускались.


### Owned GPU storage для истории двух глаз

EyeHistoryResources создаёт два независимых набора (до 10 textures в каждом)
через CreateCommittedResource на device конкретной node-0 DIRECT queue.
Поддерживает 2D / single mip/layer/sample; сохраняет format, extent и flags,
не уменьшает precision/качество. Новые allocations стартуют в COMMON с
неопределённым содержимым: caller обязан сделать initialization/TAA reset,
подготовить нужные descriptors/views и вести реальные resource states.

rebuild выделяет всю новую пару до замены старой. Ошибка allocation не меняет
действующий bank. begin_use возвращает borrowed textures только в scope до
seal_use; повторный begin/rebuild в этом scope отвергается. Caller должен
сначала отправить ВСЕ uses в указанную queue, затем seal_use ставит собственный
GPU fence. Пока fence pending, begin/rebuild возвращают WAS_STILL_DRAWING,
не блокируя CPU. Это только один известный queue timeline: неизвестные uses на
других queues, будущие submissions после seal и reuse borrowed pointers вне
scope не покрываются; для игрового backend их нужно исключить/синхронизировать.

При Signal error/device loss bank останавливается. Destructor при незавершённом
scope/fence или terminal error удерживает queue/fence/allocations до конца
процесса. RemoveDevice не вызывается. Подтверждение завершения использует
собственный, не переданный извне fence: семантика
[queue synchronization](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization),
[GetCompletedValue / device removal](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue).
Настоящий Signal failure / зависание физического GPU в этом тесте не вызывались.

CTest per_eye_history_gpu_lifetime создаёт настоящий WARP device, independent
RGBA8 UNORM и RGBA16 FLOAT banks, clear red/green и readback всех 836 pixels.
GPU Wait на отдельном fence намеренно удерживает queue: pending bank не может
быть заменён/повторно выдан. После CPU release gate и bounded completion
проверяются pixels, invalid descriptor, частичная ошибка allocation с rollback
и успешная замена extent. Ни OpenXR loader, ни игра в этот test не линкуются.
Итог: Build-Native.ps1, CTest 20/20, Python 21/21.

Это реальное GPU storage, пока не подключённое к engine texture wrappers/TAA
objects. Оно не создаёт пять engine-specific TAA objects, не переключает CPU
history state и не запускает stereo replay. Соответствие настоящих history
formats/queues/ownership ещё нужно проверить через engine observer; пользователь
подтвердил, что всё ещё играет, поэтому новая game/runtime проверка не выполнялась.

### Проверка world timing и framebuffer descriptors, 2026-11-17

После явного разрешения пользователя запущена отдельная development-копия
с --xr-world-timing, затем подключены engine probe --world-render и control.
Управление через Game-Control.py, без захвата мыши. В одном запуске получены
12422/12422 submitted XR frames, XR/GPU=0, quarantine=0; xr-views available=1,
flags=15, отдельные asymmetric FOV/poses обоих глаз. Это runtime-проверка
нового timing, не headset comfort test и не stereo world render.

Temporal observer: 703/703 valid samples, все десять descriptors читаются и
проходят parser, пять TAA slots присутствуют. Parser принимает ровно 0x38 bytes,
проверяет extent 1..16384, color count 0..8 и наличие color либо depth.
target_layout_mask отделяет распознанную структуру от scene_descriptor_mask
(успешного чтения bytes). Engine-specific format codes не равны DXGI_FORMAT.
CPU tests покрывают depth-only, неверные размеры/count/length/null и отсутствие
частичной записи результата при отказе. Сборка и CTest 20/20 прошли.

Десять scene objects — framebuffer aggregates с вложенными texture wrappers,
а не десять ID3D12Resource. Поэтому EyeHistoryResources нельзя напрямую
подставлять в scene slots: engine wrappers, attachments, TAA objects и lifecycle
пока не реализованы. Изображение в runtime по-прежнему плоская игровая панель.
