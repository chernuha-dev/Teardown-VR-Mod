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
