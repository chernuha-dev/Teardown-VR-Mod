# Поиск нативных исходников (DEV-003)

Дата: 2026-11-16. Исходный автор: `markos12900-beep`, проект
[teardown-vr-mod](https://github.com/markos12900-beep/teardown-vr-mod).
Лицензия опубликованного пакета — MIT, copyright (c) 2026 markos12900-beep;
`LICENSE` и `THIRD-PARTY-NOTICES.txt` сохранены без изменений.

## Проверенные источники

- [Публичные ветки](https://api.github.com/repos/markos12900-beep/teardown-vr-mod/branches):
  только main, HEAD `59134ac4c1b549a88870492c1cb8fa540b1999b2`.
- `git ls-remote`: main и теги v0.20 / v0.22, других публичных refs не обнаружено.
- [История](https://api.github.com/repos/markos12900-beep/teardown-vr-mod/commits?per_page=100):
  два коммита, второй `089e676c8e87e0aae452aad4f33efa46de1962b7`.
  Рекурсивные деревья обоих коммитов содержат только LICENSE, README.md,
  THIRD-PARTY-NOTICES.txt, preview.jpg. Архивы GitHub Source code из этих
  деревьев не являются исходниками DLL.
- [Релизы](https://api.github.com/repos/markos12900-beep/teardown-vr-mod/releases):
  v0.20 и v0.22, по одному release ZIP. Оба скачаны в исключённую из Git
  `local/upstream-audit/`, списки файлов проверены без запуска DLL/установщиков.

| Релиз | SHA-256 ZIP | Содержимое |
|---|---|---|
| v0.20 | `7f1b3420329cc25d49b18843b91a977bd8f49a5693db62e44e5a8b9ca1ee17b5` | DLL, Lua, VOX/JPG, установщики, документы; нет C++/headers/build |
| v0.22 | `0b78b46b9bea5d89dbbe2b03e97b1c15397c918f47ab866bad8af6da0a14ed84` | то же + blocky hands; нет C++/headers/build |

Локальный оригинальный пакет `C:\Users\navalny\Desktop\3` также не содержит
исходников C++. Автору сообщения не отправлялись. Вывод ограничен доступными
публичными ветками/историей/релизами: нельзя утверждать, что исходников не
существует в частном виде. Локальная v1.1.0 не импортировалась и не смешивалась.

## Решение

Пересобрать vendor/v0.22/dxgi.dll по этим материалам нельзя. Продолжаем
минимальное собственное ядро под MIT в `native/`, без декомпилированного кода.
Первый вертикальный срез: C++ DLL с явным ABI → приложение, связанное с DLL →
проверки композиции поз и асимметричных per-eye проекций через CTest.
Он даёт воспроизводимый проверяемый фундамент, но пока не заменяет оригинал.

Дальше: отдельный диагностический OpenXR/D3D12 стенд, затем идентификация
версии движка и перехватов, затем один snapshot / два render pass и submit.
До реализации и проверки этих частей не собирать DLL под именем dxgi.dll
и не заявлять работоспособность рендера/инструментов Teardown 2.0.0.
