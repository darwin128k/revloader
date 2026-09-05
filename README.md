# revloader

Замена `cstrike.exe` для No-Steam GoldSrc (RevEmu). Про внешние моды ничего не знает: либо поднимает Steam и запускает чужой `hl.exe`, либо при сборке `standalone` втягивает исходники репозитория `hl` и сама становится процессом игры.

## Режимы

**Загрузчик (по умолчанию).** Как RevLoader 2014: Steam IPC, `steam.dll` в этом процессе, `CreateProcess` на `ProcName` из `rev.ini` (обычно `hl.exe -game cstrike`), ждёт выхода `hl.exe`. Pid лоадера остаётся «живым Steam».

**Standalone.** Тот же Steam, затем `HlLauncher_Run` из репозитория `hl` в этом же процессе. Отдельный `hl.exe` не нужен.

## Сборка

32-bit MSVC.

Загрузчик:

```bat
build.bat
```

Standalone (путь к `hl` по умолчанию `..\hl`):

```bat
build.bat standalone
build.bat standalone D:\src\hl
```

CMake:

```bat
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DREVLOADER_STANDALONE=ON -DHL_DIR=C:/src/hl -B build
cmake --build build
```

Скрипт кладёт `cstrike.exe` в корень игры (ищет `hw.dll` на уровень или два выше).

## Запуск

```bat
cstrike.exe
```

## Лицензия

MIT. См. [LICENSE](LICENSE).
