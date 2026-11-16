# Нативная сборка: первый срез DEV-004

`tdvr_math.dll` — собственное математическое ядро, **не игровой VR-модуль**.
Оно не экспортирует DXGI, не сканирует память, не создаёт OpenXR session,
не перехватывает игру и не исправляет живую v1.1.0.

## Зависимости и запуск

Windows x64, Visual Studio 2022 Build Tools с C++ x64/x86, Windows SDK,
CMake >= 3.24 и CTest. Внешних библиотек у ядра нет.

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
