# Локальный API development-игры

`tdvr_control.dll` обслуживает named pipe `\\.\pipe\tdvr-dev-<PID>` в отдельном
потоке. CLI и stdio MCP используют один протокол. Модуль не двигает мышь,
не меняет фокус окна и сам не запускает OpenXR. Computer Use для этих операций
больше не требуется.

## Запуск и команды

Сначала запустить отдельную development-копию через наш launcher с отдельным
профилем. Затем подключить собранный модуль:

```powershell
python scripts/Game-Control.py --pid <development-pid> --attach ping
python scripts/Game-Control.py --pid <development-pid> status
python scripts/Game-Control.py --pid <development-pid> engine-status
python scripts/Game-Control.py --pid <development-pid> xr-views
python scripts/Game-Control.py --pid <development-pid> request-exit
```

По умолчанию DLL берётся из `build/native-trace/Release/tdvr_control.dll`;
`--dll` позволяет выбрать другое дерево сборки внутри repo/build.
Loader проверяет полный путь и SHA-256 известного EXE. Native startup требует
teardown.exe под local/ того же репозитория и TDVR_DEV_PROFILE под local/.
Живая установка не проходит эти ограничения. DLL закреплена до конца процесса;
для обновления требуется новый development-процесс.

`ping` подтверждает доступность протокола 1. `status` возвращает счётчики
capture и панель OpenXR: stage, submitted, frames, XR/GPU result, format и
quarantine. Нулевой captured при включённой панели ожидаем: CPU readback
в этом режиме отключён. Ошибка panel_hresult означает, что соответствующие
panel stats недоступны; её нельзя считать отсутствием ошибок рендера.

`engine-status` читает позу наблюдаемой камеры, счётчики world render,
изменения history/serial, thread changes и размеры output target. Сначала
отдельно подключить engine observer через Probe-DevelopmentCamera.py.
При его отсутствии возвращается engine_probe_not_loaded; команда сама не
устанавливает hooks. Блоки являются отдельными наблюдениями, не атомарным
snapshot симуляции. На этом срезе команда собрана и проходит локальные проверки;
её сквозная проверка в настоящей игре отложена, пока пользователь играет.

В новой сборке engine-status также содержит temporal: до/после одного sampled
world render читаются serial/history/reset, первый word TAA и маски доступных
CPU slots/descriptor prefixes. Показываются изменения scene/TAA pointers и
descriptor bytes, без публикации адресов. Descriptor hashes — hex strings,
а не числа JSON, чтобы сохранять все 64 bits. При last_sample_valid=0 старые
before/after не выдаются. Masks сообщают только об успешном чтении CPU memory,
не подтверждают COM identity, GPU states или lifetime/ownership ресурсов.
В настоящей игре этот новый observer ещё не проверен.

`xr-views` читает последний owned snapshot поз/FOV обоих глаз. `available=1`
означает, что на последнем обработанном XR-кадре обе позы valid/tracked и
прошли численные проверки. `xr_frame_id` не является simulation id игры.
Время — непреобразованный XrTime; это не Unix timestamp. Порядок FOV:
left, right, up, down (радианы); позиции в метрах, quaternion xyzw, LOCAL
space, +X вправо / +Y вверх / -Z вперёд. Никакой IPD повторно не добавляется.
При available=0 блок eyes отсутствует. Остановка Present не обнаруживается
самим getter: потребитель обязан проверять продвижение frame_id и время
жизни кадра перед использованием. Это наблюдение, не канал управления камерой.

`request-exit` отправляет WM_CLOSE только собственному видимому TeardownWindow.
Ответ exit_requested не подтверждает завершение: процесс проверяется отдельно.
Игра может завершиться до получения ответа клиентом. Команда не вызывает
TerminateProcess и не синтезирует клавиши.

## MCP

```powershell
python scripts/Game-Control.py --mcp
```

Это stdio сервер JSON-RPC с сообщением на строку. Инструменты `tdvr_ping`,
`tdvr_status`, `tdvr_engine_status`, `tdvr_xr_views`, `tdvr_request_exit`
принимают единственный аргумент `pid`
(положительное целое число). Перед использованием требуется CLI `--attach`.
Initialize, tools/list и protocol ping не обращаются к игре. Сервер реализован,
но автоматически в настройки Codex не зарегистрирован; CLI уже работает.

Pipe доступен только текущему Windows SID, запрещает удалённые подключения.
Клиент сверяет настоящий PID сервера; запросы и ответы ограничены по размеру,
ожидание клиентского I/O — 5 секунд. Ожидание запроса/ACK на сервере ограничено
2 секундами и не удерживает игровой render thread. Свободного выполнения
shell/Lua и передачи указателей в протоколе нет.

## Проверено 2026-11-17

В настоящей development-игре проверены attach, ping и status без управления
окном. Прочитано 12080/12080 успешных XR submissions, XR/GPU=0, formats
28 → declared 29 / storage 27. В предыдущем development-процессе request-exit
вернул ответ, затем отсутствие процесса подтверждено через CIM.

CTest development gate отвергает неизвестный host. Python проверяет MCP
discovery без доступа к игре, routing, ошибки/timeout и некорректный PID.
Итого на этом срезе CTest 17/17, Python 16/16. Это интерфейс диагностики,
а не управление игровым персонажем, сценою или VR-контроллерами. Стереорендер
мира и комфорт в шлеме этим не подтверждены.

Позднее пользователь прислал снимок SteamVR Mirror с игровой панелью и
подтвердил, что панель видна в шлеме. Это подтверждает появление изображения,
но не независимые проекции глаз или комфорт. Development-процесс закрыт
через API перед пользовательской миссией; новые игровые/XR-запуски ждут
его сообщения о завершении игры, а не истечения десяти минут.
