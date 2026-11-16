# Аудит v0.22: Lua ↔ DLL

Проверено 2026-11-16 по `mod/main.lua` и оригинальному README v0.22.
Это контракт, наблюдаемый со стороны Lua. Исходников DLL нет: её сканер памяти,
порядок перехватов, барьеры памяти, пересчёт OpenXR-поз и GPU submit не проверены.
Ссылки Lua на `src/td_bridge_slots.h`, `input.cpp`, `docs/V*_PLAN.md` относятся
к отсутствующим авторским файлам, а не к исходникам в этом репозитории.

## Транспорт и версии

Текстовых команд или сокета нет. `init()` создаёт глобальную `TDVR_BRIDGE`:
ровно 256 чисел, без дополнительных ключей и изменения размера. Предположительно
DLL находит массив Lua TValue по сигнатуре и читает/пишет память напрямую.
Номер Lua = native index + 1. Ниже все диапазоны **с нуля**.

| Индексы | Владелец / назначение |
|---|---|
| 0–2 | Lua: magic 7340033.0625, -2621447.5, 12345.6875 |
| 3 | Lua: bridge protocol **3**, независимо от версии релиза |
| 4–5 | Lua: старшие/младшие 32 бита адреса таблицы из tostring(B) |
| 6 | Lua: размер 256 |
| 7 | резерв |
| 8–63 | Lua → DLL: heartbeat, подтверждение камеры, ввод, диагностика |
| 64–158 | DLL → Lua: согласованный блок поз, состояния, параметров |
| 159 | резерв |
| 160–255 | Lua → DLL: диагностические блоки L2…L8 |

`L_LUA_VERSION` (28) = **22**. Протокол 3 переиспользует слоты старых
диагностик: их смысл нужно брать именно из v0.22, а не из v0.15/v0.20.
v1.1.0 с `dsound.dll` — отдельная реализация, не поставщик этого ABI.
Не использовать `tdvr_math.dll` вместо vendor DLL: новое ядро имеет другой,
явный ABI для математики и пока вообще не подключается к игре.

`readNative()` принимает блок, когда 64 == 158 и heartbeat 65 != 0;
копирует 64…158, затем повторно проверяет 64. Предыдущее корректное состояние
сохраняется при отказе. **Нет** проверки чётности счётчика, NaN, нормализации
кватернионов или повторной проверки 158 после копирования. Это не доказательство
потокобезопасности: протокол записи DLL неизвестен. Нельзя переносить его
на новый поток C++ как обычный массив double без определения синхронизации.

## Команды/данные DLL → Lua

| Индексы | Содержимое |
|---|---|
| 64–68 | seq_begin, heartbeat, mode, pose sequence, eye (0 левый / 1 правый) |
| 69–75 / 76–82 | поза глаза / головы |
| 83 | запрашиваемый горизонтальный FOV, градусы |
| 84–90 / 91–97 | левый grip / aim |
| 98–104 / 105–111 | правый grip / aim |
| 112 | hands_valid: 1 левый grip, 2 левый aim, 4 правый grip, 8 правый aim |
| 113–116 | left/right stick x/y |
| 117–121 | left/right trigger, left/right squeeze, buttons |
| 122–126 | aim_mode, yaw_control, turn_mode, snap_angle, smooth_speed |
| 127–132 | world_scale, laser, left_handed, debug, recenter counter, deadzone |
| 133–134 | pair_id, hold_base |
| 135–138 | camera method, perf flags, aim_update, hand_style |
| 139–141 / 142–144 | смещения кисти: позиция / углы |
| 145–148 | сообщение, аргумент, hand_skin, fix_options |
| 149–153 | tool_frame, move_dir, tool_display, hand_physics, jump_assist |
| 154–158 | grab_max_mass, grip_latch, cam_opts, hand_body, seq_end |

Lua включает VR только для mode 2 (mono) / 3 (stereo), seq > 0 и живого
heartbeat. Другие режимы, включая плоский экран, обслуживаются DLL.
Heartbeat считается устаревшим после 30 **tick**, не после заданных миллисекунд.
`computeFlags()` блокирует VR-камеру в pause/map/screen/menu/photo/animation/
cinematic/dead/interactive UI. Контроллеры копируются в `ctl`; правый глаз
пары повторно использует снимок левого, если совпадает pair_id.

`hand_physics`: 1 grab, 2 push, 4 throw, 8 hand collision, 16 body collision,
32 * strength enum, 128 punch, 256 steady. `cam_opts`: 1 tick offset,
2 A связан с jump, 4 hide player, 8 show body, 16 body turn, 32 flat scope.
`fix_options`: 1 headbob=0, 2 toolsway=0. Это не разрешение снижать графику.
`perf_flags` — диагностические отключения aim/yaw/laser/tool/hands,
forced precise aim и static hands (1/2/4/8/16/32/64).

## Ответы Lua → DLL

8 heartbeat; 9–10 ack_seq/ack_eye; 11 state flags; 12 hfov_used;
13–14 измеренные tan(horizontal/vertical half FOV); 15–18 world base xyz/yaw;
19 world_scale. 20–22 и 23–25 — запросы haptic sequence/amplitude/duration
левой и правой руки. 26–30 — player yaw/pitch, Lua version, draw count, error.
31–34 — UI width/height, aim active, render call count; 35–41 — счётчики
действий, tool state/distance. 42–63 — engine/camera/aim/hands/options/perf
диагностика, hand proximity/holding и ввод игры.

160–199 (L2): камера, выбор eye/player frame, tool frame, bounds кистей,
options restore, jumps/grabs/replica/correction/grip latch.
200–215 (L3): aim checks, attach state/view error, movement yaw, v0.22 attach
gate/body copy. 216–226 (L4): чужие инструменты, hand query/rejections,
jump/ground. 227–235 (L5): расстояния, видимость engine tool/player и v0.22
hand snaps. 236–242 (L6): animator, hand collision, tool error, jump speed,
punch. 243–247 (L7): visible body, tool placement state/error/rotation.
248–255 (L8): actuator, body collision, physics probe, jitter, placement
position/rotation errors, base estimate, teleport predictor.
Ряд полей упаковывает несколько счётчиков десятичными разрядами: не читать их
как float32. Полные формулы — в декларациях L2…L8 и `writeDiag21()`.

## Координаты и единицы

Каждая поза — семь чисел `px py pz qx qy qz qw`, quaternion xyzw.
Lua использует +X вправо, +Y вверх, направление вперёд `(0,0,-1)`.
`localToWorld(base,p,ws)` вычисляет `T_world = T_base * T(p.xyz*ws, p.xyzw)`:
масштаб меняет только перенос. Ожидаемая входная единица — метр; это следует
из размеров/скоростей в Lua и world_scale=1, но преобразование в DLL неизвестно.
Входные позы уже должны быть относительно VR origin; Lua не вычитает standing
height из сырой абсолютной OpenXR STAGE-позы. Нельзя добавлять абсолютную
высоту шлема к высоте глаза игры повторно.

`computeBase()` на ногах берёт `attachEye()` либо `playerEyePos()` и bodyYaw;
в машине — eye игры и yaw машины + vehicleYawOffset. `yawPitchOf()`:
yaw=atan2(-dir.x,-dir.z), pitch=asin(dir.y), в градусах. Положительный yaw
поворачивает -Z в -X, положительный pitch поднимает взгляд. Смещения кистей
и yaw/pitch API — градусы; кватернионы безразмерны. Диагностика нередко
переводит метры в cm/mm и углы в градусы, не меняя позы протокола.

## Обновление и точки вмешательства

1. `init()` создаёт bridge; `tick()` повторяет init после quickload, если нужно.
2. `update(dt)` (по комментарию legacy 60 Hz, 0…n шагов за кадр): `holdHands`,
   `driveColliders`, `bodyCopyRing`, scope. Это физический шаг, не eye render.
3. `tick(dt)`: readNative → flags/heartbeat → heading/eye/controller snapshot →
   hands/replica/probes → `updateActive()` → body → collision/hand physics → status.
4. `updateActive()`: turn → camera target → aim/tool → `submitCamera()` →
   hands/laser/replica (если render callback не обнаружен) → haptic requests.
5. `render(dt)`: камера по post-physics позиции, повторная установка инструмента,
   камера и визуальные руки/body. Здесь **нет** readNative: используется tick snapshot.
6. `draw()`: фактическая камера сравнивается с hist, публикуются ack и измеренные
   frustum tangents через UiPixelToWorld. При отсутствии строгого совпадения
   используется последний hist: ack сам по себе не доказывает корректный рендер.

Камера: `computeCamera`, `pairedBase`, `submitCamera`.
Методы 1 SetCameraTransform; 2 SetCameraFov + SetCameraOffsetTransform;
3 SetPlayerCameraOffsetTransform; 4 AttachCameraTo + world-relative offset.
Метод 1 может лишать игрока штатной интерактивности; наличие API ещё не
подтверждает его правильный порядок относительно движка.

Инструмент: `updateAimAndTool`, `setPlayerAim`, `placeToolInHand`, `actPlace17`,
`actRender`, `actPark19`, `placeToolReplica`. Aim ray и видимая модель — разные
пути: попадание лазера не подтверждает 6DoF engine tool. В v0.22 часть времени
показывается реплика, engine tool возвращается при использовании.

Физика: `handQuery`, `updatePhysHand`, `updateHandPhysics`, `holdHands`,
`hcolTick`, `driveColliders`: выбор динамических тел, constraint/throw,
собственные коллайдеры, проверки collision masks. Новые features gated битами
и self-tests; это не проверка устойчивости на реальном движке.

UI: `computeFlags`, `drawMessage`, `draw` измеряют/передают состояние и размер.
Рендер плоского экрана и генерация mouse/keyboard input — в неизвестной DLL.
Lua не содержит законченного VR меню/HUD.

## Вывод для следующего этапа

`pairedBase()` прямо описывает левый глаз на одном игровом кадре и правый на
следующем; удержание base/controllers не замораживает остальную симуляцию.
Это **не синхронное стерео**. Один hfov не выражает асимметричные OpenXR FOV.
Исправить оба ограничения только Lua невозможно. До перехвата рендера нужны
проверяемые per-eye view/projection, один simulation snapshot, два render pass,
правильные D3D12 barriers/fences и OpenXR swapchains/session lifecycle.
Новое ядро проверяет только математику, не реализует эти перехваты.
