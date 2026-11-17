# Статическая диагностика движка — первый срез DEV-005

`scripts/Inspect-Teardown.py` читает PE-файл без загрузки DLL, запуска игры,
подключения к процессу или изменения установки. Нужен Python >= 3.10,
дополнительных пакетов нет. Это самостоятельная диагностика: игровой модуль
ещё не использует её результат для допуска перехватов.

```powershell
$gameExe = Read-Host 'Полный путь к teardown.exe'
python scripts/Inspect-Teardown.py --exe $gameExe --reported-game-version 2.0.0 --output local/diagnostics/engine.json
python -m unittest discover -s tests -p test_engine_diagnostic.py -v
```

Без `--output` JSON выводится в stdout. Файл отчёта разрешено создавать только
под исключённой из Git `local/` этого репозитория; существующий файл не
перезаписывается. Повторную проверку сохранять под новым именем. Отчёт содержит
только имя входного файла, без абсолютного пути. Коды возврата: `0` — разбор
успешен, `2` — ошибка чтения/структуры/вывода. **Нулевой код не означает
разрешение VR:** смотреть `compatibility.vr_mode_allowed` и capabilities.

## Что проверяет инструмент

- Полный SHA-256 и размер файла, COFF machine, PE32/PE32+ и COFF timestamp.
  Timestamp — сырые метаданные, не доказанная дата выпуска.
- Фиксированные FileVersion/ProductVersion из дерева RT_VERSION, с языком
  ресурса. Пользовательская версия передаётся отдельным полем и никогда
  не заменяет сведения PE.
- Обычные и современные RVA-based delay imports: имена DLL и symbols/ordinals.
  При bound IAT без lookup table список symbols явно недоступен.
- Границы заголовков, секций, RVA, импортов и ресурсов; отсутствующие
  terminators, пересечения секций, циклы дерева и чрезмерные размеры
  завершаются отказом. VA-based legacy delay imports пока не поддержаны:
  инструмент возвращает ошибку, а не угадывает адреса.
- Известная локальная идентичность имеет статус `observed_only`, неизвестная —
  `unknown`, повреждённый/неподдержанный PE — `invalid_pe`. Во всех случаях
  VR и все шесть engine capabilities остаются выключены с объяснением.

Это ограниченный статический анализатор нужных структур, не Windows loader,
не полная проверка допустимости PE и не проверка подписи Authenticode.
Совпадение локального SHA не доказывает происхождение официальной сборки.
Импорт DLL не доказывает её использование в активном рендерере; отсутствие
импорта не исключает `LoadLibrary`/`GetProcAddress`.

## Наблюдение 2026-11-16

Чтением единственной указанной пользователем установки получены:

| Поле | Результат |
|---|---|
| Пользовательская версия игры | 2.0.0; независимо не подтверждена PE-версией |
| Имя / размер | teardown.exe / 13 655 624 bytes |
| SHA-256 | `2733e37969fd6d45c5c2130872b0d4096147c304266b4b0adda01bd98caa2c4d` |
| PE machine / формат | `0x8664` / PE32+ |
| Fixed FileVersion / ProductVersion | 1.0.0.1 / 1.0.0.1 |
| Статические графические импорты | `opengl32.dll`, `gdi32.dll!SwapBuffers`, `dxgi.dll!CreateDXGIFactory1` |
| D3D12 | `d3d12.dll` не найден в regular/delay imports |
| Результат допуска VR | `false`, проверенных engine hooks нет |

OpenGL imports включают `wglCreateContext`, `wglMakeCurrent` и функции `gl*`.
Эти imports сами по себе не выбирают игровой backend. При последующем чтении
уже существующих локальных логов найдены более сильные исторические признаки:
v0.22 регистрирует устройство D3D12, DIRECT queue и совместимый формат
backbuffer, а v1.1.0 определяет `TRendererD3D12` и оборачиваемый `Present`.
Поэтому текущий путь разработки — D3D12. Это свидетельство прошлых запусков,
а не новый захват текущего процесса: игра при исследовании не запускалась.
OpenGL imports не доказывают активный OpenGL renderer. Отдельный стенд
по-прежнему не подтверждает возможность захвата игровых render targets.

Чтение существующего лога альтернативной v1.1.0 также подтверждает сообщения
о нераспознанном размещении инструмента и карте уровня. Найденные ею адреса
камеры и корректный прицел не являются проверкой вращения инструмента.
Лог, адреса и игровые файлы в репозиторий не копировались. Код v1.1.0 и её
протокол не переносились; версия `mod/` по-прежнему предназначена для v0.22.

## Проверка и следующие условия допуска

Пять unittest-проверок на синтетических PE проходят без установленной игры:
PE32/PE32+, обычные/delay/ordinal imports, RT_VERSION, отсутствие метаданных,
известная и неизвестная идентичности, повреждённые структуры и безопасный
вывод CLI. Входной файл после запуска CLI побайтно совпадает с исходным.
Реальный `teardown.exe` разобран успешно; JSON сохранён только в `local/`.
Игра, runtime и шлем этой проверкой не запускались.

DEV-005 остаётся открытой. До её закрытия собственному нативному модулю нужны:

1. Проверяемая точка перехвата D3D12 для этой идентичности движка с отказом
   при несовпадении; историческое свидетельство backend не является проверкой
   наших hooks. При будущей контролируемой проверке подтвердить активный backend.
2. Проверки обязательных функций отдельно: camera/view/projection, один
   simulation snapshot для двух глаз, tool transform и UI/map.
3. Потребление capability gate самим модулем; обязательный отказ режима при
   отсутствии функции, с диагностикой причины. Версия, hash или найденный
   адрес сами по себе ни одной функции не включают.
4. Автоматические проверки координат/проекций/порядка кадров и отдельная
   проверка в движке. Комфорт и поведение в шлеме пока не проверены.

Форматы разобраны по официальным описаниям Microsoft:
[PE/COFF](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format),
[VS_VERSIONINFO](https://learn.microsoft.com/en-us/windows/win32/menurc/vs-versioninfo),
[VS_FIXEDFILEINFO](https://learn.microsoft.com/en-us/windows/win32/api/verrsrc/ns-verrsrc-vs_fixedfileinfo).

## Первый desktop engine hook

В отдельном development-процессе той же идентичности проверен camera setter
RVA 0x5ded80: три аргумента object/position/quaternion, завершённая view matrix
по object+0x40 в column-major, projection в первых 64 bytes. Observational hook
всегда вызывает оригинал с исходными аргументами. Сравнение с собственным
inverse view дало ~1.58e-6 на 183 valid samples без rejected samples.
Это совместимость координат этого setter, не проверка всех camera modes/tools.

По runtime call references найдены кандидаты для дальнейшего анализа:
camera setup 0x6f770 (создаёт временную камеру и копирует её в game+0x240),
update 0x78670 и вызывающая их frame-функция 0x70930. Их ABI, границы симуляции
и возможность stereo replay ещё не подтверждены, hooks на них не установлены.
Другой camera caller 0x65970 работает с отдельным объектом и камерой +0x9fd8;
не смешивать её с основной камерой. Статические адреса из чужого лога сами
по себе не включают поддержку: native gate проверяет полный SHA и runtime prefix.

Дополнительный разбор сохранённого runtime-кода (без нового вмешательства в
процесс) показывает: projection setter 0x5dea00 пишет near/far в camera+0x80/
0x84; при копировании камеры движок переносит 0x88 bytes. Поэтому собственный
адаптер камеры включает обе матрицы и clip distances, а не только первые 0x80
bytes. Ветка projection setter зависит от backend; наблюдавшаяся D3D12-матрица
использует depth [0,1]. Адаптер поддерживает только эту перспективную форму.

Кандидат 0x7ac30 **не является render replay**: цикл сравнивает accumulator
game+0x214 с шагом game+0x200, вызывает 0x79dc0, вычитает шаг и записывает
интерполяцию в game+0x21c. Повторять его для правого глаза нельзя: это привело бы
к дополнительным simulation steps. 0x65970 также выполняет работу с файлами/
состоянием и остаётся неподходящим кандидатом для слепого повторного вызова.
Обе классификации получены из потока инструкций, не из runtime-трассы вызовов.
Нужен отдельный world-render boundary; он пока не подтверждён.

Прямые callers 0x70930 найдены в 0xa4850/0xa5900; после него обе ветки вызывают
0x78650. Последняя передаёт game+0x394 и нулевой третий аргумент виртуальному
методу renderer (game+0x30), slot +0xe0. Это кандидат завершения/present кадра,
а не подтверждённый world-render pass. Его фактический target и связь с DXGI
Present ещё не наблюдались; вызывать повторно или использовать как stereo
boundary до этого нельзя.

Новая runtime recording trace уточнила классификацию: на вилле реальные
Draw/Dispatch стеки проходят через return 0x6fd57 внутри 0x6f770. Инструкция
0x6fd52 вызывает 0x865b0 с четырьмя pointer arguments: scene (game+0x48),
target (game+0x888), player (game+0x50), camera (game+0x240). Значения и роль
аргументов выведены из call site; typed hook ещё не проверен. 0x6f770 поэтому
является camera setup + render/UI, а не чистой функцией настройки камеры.

Render candidate 0x865b0 уже при входе записывает player в scene+0x2200,
инкрементирует scene+0x2208 и меняет местами поля scene+0x2210/+0x2214.
Назначение последних полей (возможно temporal history) пока не доказано.
Даже настоящий world render имеет side effects: слепой двойной вызов не
доказывает независимые eye histories или синхронное стерео. Следующий этап —
наблюдаемый hook этой границы, проверка camera/target и побочных состояний.

Следующий срез установил observational hook 0x865b0 с ABI
`void(scene*, target*, player*, camera*)`, проверяя полный EXE SHA и runtime
prefix обоих entry points до установки hooks. В desktop-сцене виллы наблюдены
780 calls / 780 completed, 49 valid samples / 0 invalid. Все 49 camera pointers
совпали с global Game+0x240, camera record не изменился за render, serial вырос
ровно на один и оба поля history поменялись местами. Все samples были на одном
потоке. Near/far прочитаны непосредственно: 0.05 / 500 world units.

Это подтверждает вызов и аргумент основной камеры в данном сценарии; hook
ничего не подменяет и не повторяет. Значение history fields, их GPU ресурсы,
target layout и per-eye temporal isolation по-прежнему не подтверждены.

Следующий bounded diagnostic уже подставляет выбранный synthetic camera record
внутри этого hook и восстанавливает его после единственного исходного вызова.
На вилле заданные локальные 2 м / yaw 20° визуально изменили desktop-вид;
600/600 records восстановились, rejected=0, затем вернулся исходный вид.
Меню также использует Game+0x240: отдельная проверка Game+8 дала state 2 в меню
и 6 на вилле. Нельзя объявлять gameplay только по main-camera pointer.
Пока нет stereo replay, eye targets, GPU/history isolation и XR submit;
перенос камеры не переносит инструмент/контроллеры и не подтверждает комфорт.

RTV observer подтвердил D3D12 OMSetRenderTargets return 0x5ac513. Сохранённый
runtime-код показывает соответствующий backend entry 0x5ac070: сохраняет
target/attachment в renderer+0xd50/+0xd60, берёт размеры из target+0/+4,
число color attachments из target+0x30, pointers из +0x38+index*8,
depth pointer из +0x78. Для texture wrapper поле +0x38 передаётся как
ID3D12Resource* в CreateRTV, затем barrier и ClearRenderTargetView.
Это вывод из потока инструкций, не контракт для самостоятельного владения
ресурсами: layout ещё не проверялся typed hook/COM identity/readback, нельзя
подменять по нему объекты или повторять world render без дальнейшей проверки.

На работающей development-вилле прочитано Game+0x888: output target имеет
размер 3440×1440 и один color attachment. Его wrapper+0x38 указывает на объект
с vtable methods 0/7/10 в D3D12Core.dll. Это app-local Agility SDK, а не System32:
`D3D12/D3D12Core.dll`, 5617048 bytes, SHA-256
`8710110eccf43a32e5e1a04552cb344832922a0cd23caeecb764e18bd5cdffa8`.
Authenticode: Valid, Microsoft Corporation. DLL/код в Git не копируются.

Новый `--engine-targets` читает target layout на render thread после world render,
проверяет принадлежность vtable methods доверенному Core и только затем вызывает
QueryInterface(ID3D12Resource)/GetDesc. Не создаёт GPU work и не удерживает
ресурс между samples; known/matching masks не означают ownership или StateBefore.
Первая загруженная сборка отвергла app-local Core из-за System32-only допуска
(143 rejected samples), не вызвав COM. Исправление допускает указанный app-local
path только с точными size/hash, либо системный Core. Оно собрано и прошло gate
test отдельно. В следующем development-запуске исправленная DLL подтвердила
GetDesc: 703 valid samples, 0 rejected, state 6, 3440×1440, один attachment;
known/matching masks=1. Это чтение метаданных, не владение ресурсом.

В режиме --xr-world-timing подтверждены 11245 completed world calls,
703 sampled serial increments/history swaps, 0 invalid и 0 thread changes.
Все десять scene descriptors прошли target parser (mask=0x3ff); pointers и
descriptors не изменились внутри sampled render. TAA word в последнем sample
изменился 0→1. Не следует объявлять этот word монотонным frame counter.
Собственная XR session получила tracked views (flags=15) и отправила
12422/12422 кадров, XR/GPU errors=0, quarantine=0. Начало XR frame теперь
связано с world boundary; вывод остаётся quad, не независимым stereo world.

Одноразовая раздельная проба factory подтвердила ABI renderer slot +0x138:
scene+0x9c0 содержит renderer с entry 0x5a8610, принимающим descriptor и
NUL-terminated name. Два вызова на render thread создали два отдельных engine
targets и resources 64×64 RGBA8 UNORM (GetDesc, flags=1, samples=1).
Targets удерживаются до process exit; не рисовались и не заменяли scene slots.
Это ограниченное подтверждение creation path, не destructor/resource-state
контракт и не подтверждение full-size rendering или temporal isolation.

Следующая проба создала обе full-size framebuffer banks (20 targets) и оба TAA
набора (10 texture wrappers). Texture factory slot +0xe8 (0x5a9000) принимает
renderer, descriptor, optional initial data и C-string name; base constructor
0x5ddae0 копирует 0x1c bytes descriptor, а вычисленное mip state пишет в +0x1c.
TAA creation 0xa41a0 на D3D12-ветке использует engine formats 12/12/26/26/12
(hex), texture descriptor в stack и null initial data. Эти engine formats
соответствовали GetDesc 26/26/54/54/26 в проверенном запуске, не общей таблице.
Все 30 новых объектов имеют независимые ресурсы, source CPU state не изменился.
Estimate 486 MiB при исходных extents/precision; они удерживаются до process
exit и ещё не использовались в render passes. Destructor, reset/initialization,
CPU history switching и синхронный replay этим не подтверждаются.
