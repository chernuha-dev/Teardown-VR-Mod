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

`request-exit` отправляет WM_CLOSE только собственному видимому TeardownWindow.
Ответ exit_requested не подтверждает завершение: процесс проверяется отдельно.
Игра может завершиться до получения ответа клиентом. Команда не вызывает
TerminateProcess и не синтезирует клавиши.

## MCP

```powershell
python scripts/Game-Control.py --mcp
```

Это stdio сервер JSON-RPC с сообщением на строку. Инструменты `tdvr_ping`,
`tdvr_status`, `tdvr_request_exit` принимают единственный аргумент `pid`
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
