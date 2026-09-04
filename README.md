# revloader

Замена `cstrike.exe` для No-Steam GoldSrc (RevEmu): сам процесс не является игрой. Он поднимает эмулятор Steam и запускает `hl.exe`.

Исходный `cstrike.exe` в таких сборках — это RevLoader (2014), не стаб Valve. Игра живёт в `hl.exe` + `hw.dll`. Двойной клик по `hl.exe` без этого лоадера обычно падает на `SteamAPI_Init` (`Failed to initialize authentication interface`).

## Что делает

1. Читает командную строку (`-launch`, `-appid`) и `rev.ini` (`[Loader] ProcName`, по умолчанию `hl.exe -game cstrike`).
2. Берёт AppId из `-appid` или `steam_appid.txt`, выставляет `SteamAppId` / `SteamGameId`.
3. Создаёт Steam IPC: `Local\SteamStart_SharedMemFile`, `Local\SteamStart_SharedMemLock`.
4. Грузит `steam.dll` **в процесс лоадера** (x86).
5. Пишет `HKCU\Software\Valve\Steam\ActiveProcess`: живой `pid` лоадера и путь к `steamclient.dll` (интерфейс `SteamClient012`).
6. `CreateProcess` на ProcName и ждёт, пока `hl.exe` не завершится.

Лоадер должен оставаться запущенным, пока идёт игра: `steam_api` считает Steam живым, пока pid в реестре — существующий процесс.

## Сборка

32-bit MSVC (у `steam.dll` нет x64).

```bat
revloader\build.bat
```

Нужны Visual Studio 2022 (vcvars32), CMake и Ninja. Скрипт кладёт `cstrike.exe` в родительскую папку (корень игры) и один раз копирует прежний exe в `orig\`.

Сборка руками:

```bat
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build
```

## Запуск

Из корня игры, рядом с `hl.exe`, `steam.dll`, `steamclient.dll`, `rev.ini`, `steam_appid.txt`:

```bat
cstrike.exe
```

## Лицензия

MIT. См. [LICENSE](LICENSE).
