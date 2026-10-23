# Obelisk launch path of a Windows Epic game (Rocket League)

Date: 2026-10-02. Repository `obelisk`, branch `main`, HEAD `04efbff` (only `docs/` is untracked). Everything was read or measured read-only: no game, no EAC, no Obelisk, no Wine was launched for this report.

Conventions.
- READ: found in the code or in a cited document (`file:line`).
- MEASURED: found on the disk of this machine (listing, size, hash, static analysis of a file without running it).
- INFERRED: inference on my part, never presented as a fact.
- Path abbreviations: `GR` = `crates/game-runtime/src`, `EK` = `crates/storefronts/epic-kit/src`, `SC` = `crates/obelisk-core/src`, `SN` = `crates/steam-native/src`, `LIB` = `~/Library/Application Support/com.zagrios.obelisk`, `EXP` = `build/experiments/rocket-wine-fex-darwin-20261002`. The citations `rocket-league-macos.md:N` refer to `docs/research/rocket-league-macos.md`. `voile/` and `voile-wine/` are the sibling repositories in `Zagrios/`.

## 0. Limits of this reading

- I did not read `EpicSessions/`, `SteamSessions/`, `SteamCloud*`, the prefix registry files (`system.reg`, `user.reg`, `userdef.reg`), nor the prefix EAC logs. The "registry settings" of section 4 therefore come from the code, not from the actual registry.
- The permission classifier refused the reading of the log `~/Library/Logs/Obelisk/Epic/wine-Sugar.log` (297 bytes). I did not work around this refusal: its content is unknown, only its name, size, mode and date (directory listing) are known.
- `docs/architecture.md` and `docs/epic-protocol.md` no longer exist in the tree: commit `7d4d22f` (2026-09-24, "remove docs") deleted them. I reread them through `git show 7d4d22f^:docs/...` (read-only). They predate the Wine wiring of September 26 and carry the old name "Forge".
- Contradictions found in the repository: see section 8.1.

## 1. The facts that matter for the architecture

1. Launching a Windows Epic game through Wine is already wired end to end at HEAD (`GR/epic/content.rs:357-416`, `GR/epic/windows.rs:10-44`, `GR/epic/launch.rs:258-274`, `GR/wine.rs:161-199`). The paragraph `README.md:74` that says the opposite is obsolete, as the timeline already notes (`docs/research/rocket-league-macos.md:15`). Traces of a real launch from September 26 exist on disk (MEASURED: `wine-Sugar.log`, `anticheatlauncher.log`, `service.log`, names and dates only) and the timeline reports that it reaches the EAC bootstrapper then stops on launcher code 206 (`rocket-league-macos.md:19-25`).
2. Three processes take turns: the application (Swift interface and Rust library in the same process), a detached supervisor `obelisk-steam-host epic-supervise`, then an arm64 helper `obelisk-steam-host game-exec` that does an `exec` of the `bin/wine` binary (x86-64, under Rosetta). The neutral contract between supervisor and engine is the `Target` structure (`GR/supervisor/mod.rs:41-50`) and the `exec::Admission` message (`GR/supervisor/exec.rs:13-22`).
3. For Rocket League, Obelisk launches `Binaries/Win64/Launcher.exe` (MEASURED in the receipt), with `Binaries/Win64` as working directory. It is a small .NET program from Psyonix (79,184 bytes) that, by default, launches `RocketLeague_EAC.exe` with the same arguments, then waits (decoded IL, section 3.7). So Wine Mono is needed, or `RocketLeague_EAC.exe` must be launched directly.
4. There are two distinct Epic exchange codes: one opens the supervisor's own Epic session (passed through a private pipe), the other is the `-AUTH_PASSWORD=` of the game command line (therefore visible in the `argv` of the Wine process once launched, section 3.5).
5. There is no launch CLI. The existing test seams are the `Services` trait (`GR/epic/launch.rs:18-36`), the `Launch` trait (`GR/supervisor/session.rs:117-181`) and the `Admission` message of the `game-exec` helper (usage example: `crates/steam-native/tests/exec_game.rs:15-33`).
6. The smallest insertion point for a "Linux userland under FEX" path is the function that produces a `Target` (today `wine::target`, `GR/wine.rs:181-199`), plus the prefix preparation (`wine::prepare`, `GR/wine.rs:161-176`), plus the Rosetta gate check (`GR/supervisor/session.rs:581`).
7. D3DMetal is enabled by two combined means: Apple's DLLs placed in `lib/wine/x86_64-windows` of the runtime and two environment variables that Obelisk sets only if `lib/external/libd3dshared.dylib` exists (`GR/wine.rs:80-83,134-140`). `OBELISK_GRAPHICS` is read by no Obelisk code: it exists only in `voile/scripts/wine-dev.sh`.
8. The installed library contains `Voile/current` (development runtime, takes precedence, without `runtime.json`) and `Voile/v26.3.0-4`, while the repository pins `v26.3.0-6` (MEASURED, `GR/voile.rs:65-73`, `voile-release.json`).

## 2. Launch pipeline, in the real order

The real order differs from the requested order: the prefix and the prerequisites are prepared BEFORE any authentication, because a prerequisite can take minutes and an exchange code is short-lived (`GR/epic/windows.rs:7-9`, `GR/epic/content.rs:377-378`).

Process tree of a launch (diagram of my own: READ for the first three levels, INFERRED from the IL of `Launcher.exe` and from the timeline for the last one):

```
Obelisk.app                       Swift interface + Rust library, a single process
 `- obelisk-steam-host epic-supervise     detached (setsid), env = PATH only, one JSON Request on stdin
     `- obelisk-steam-host game-exec      arm64, env = Target.environment, one JSON Admission on stdin
         = exec  <Voile>/bin/wine Launcher.exe <args>        x86-64, translated by Rosetta
             `- wineserver, then Launcher.exe (Mono) -> RocketLeague_EAC.exe -> RocketLeague.exe
```

| # | Step | Function | File:line |
|---|---|---|---|
| 0 | Launch click on the interface side | `GameInstallations.launch` calls `core.launch(installation:verify:)` | `apps/macos/Obelisk/Models/GameInstallations.swift:507-508`, bridge `crates/obelisk-bridge/src/installations.rs:506-508` |
| 1 | Admission: the Reservation is taken before any wait | `Installations::launch` then `Sessions::admit` | `GR/installations.rs:841-865`, `GR/sessions.rs:590-612` |
| 2 | File verification (Epic network), only if `verify` or if the receipt requires it | `shared.perform(&verification)`; the request comes from `Epic::update_request` | `GR/installations.rs:849,897-901`, `GR/epic/mod.rs:303-305,343-352` |
| 3 | Handover to the supervisor | `Content::launch` then `Admission::hand_off(Handover)` | `GR/epic/content.rs:248-266`, `GR/sessions.rs:841-864` |
| 4 | Selection and installation of the Voile runtime | `Handover::voile()` (Some if Windows build) then `Canceller::install_runtime`; resolution by `voile::runtime` | `GR/epic/content.rs:369-374`, `GR/sessions.rs:784-810`, `GR/voile.rs:61-73,116-186` |
| 5 | Prefix preparation: valid program, `wineboot`, Mono, folders, prerequisites | `Handover::prepare` calls `prepare_windows` then `wine::prepare` (blocking thread) | `GR/epic/content.rs:375-392`, `GR/epic/windows.rs:10-44`, `GR/wine.rs:161-176` |
| 6 | Authentication on the interface side: account then exchange code then account recheck | `account_id()`, `exchange_code()`, `account_id()` | `GR/epic/content.rs:393-399` |
| 7 | Saving the renewed credentials before the game starts | `Handover::before_begin` | `GR/epic/content.rs:403-406`, `GR/sessions.rs:843,432-434` |
| 8 | Creation of the supervisor and handover of the request over a private socketpair | `epic::start` then `Client::start_with` | `GR/epic/client.rs:8-43`, `GR/supervisor/client.rs:384-450` |
| 9 | The supervisor becomes a detached process | `obelisk-steam-host epic-supervise` (`setsid`) then `epic::serve` | `SN/bin/obelisk-steam-host.rs:18,90-95`, `SN/native.rs:243-264`, `GR/epic/launch.rs:301-303` |
| 10 | Opening of the log, the control socket, the owner lock | `lifecycle::serve` | `GR/supervisor/session.rs:404-492` |
| 11 | Begin gate: waits for the go-ahead from the interface (120 s max) | `Session::begun` | `GR/supervisor/session.rs:36-37,357-372,569-571` |
| 12 | Supervisor authentication: its own session from the exchange code | `Launch::authenticate` then `Client::from_exchange_code` | `GR/epic/launch.rs:136-140`, `EK/client.rs:78-82`, `EK/auth.rs:137-146` |
| 13 | EGS Cloud: opening and preparation before the game | `Launch::cloud` then `CloudSync::prepare` | `GR/epic/launch.rs:142-156`, `GR/supervisor/session.rs:573-575` |
| 14 | Launch plan: second exchange code, optional ownership token, Epic arguments | `Launch::target` then `launch_plan` then `windows_target` | `GR/epic/launch.rs:158-173,258-274`, `EK/client/launch.rs:150-304` |
| 15 | Rosetta gate (aarch64 build and Intel architecture) | `wait_for_rosetta` | `GR/supervisor/session.rs:577-594`, `GR/supervisor/architecture.rs:143-172` |
| 16 | Host preparation (helper, subcommand, environment) | `Launch::prepare_host` | `GR/epic/launch.rs:175-190` |
| 17 | Creation of the helper and admission: a single JSON message on its stdin | spawn `game-exec`, `exec::send` | `GR/supervisor/session.rs:596-657`, `GR/supervisor/exec.rs:27-35` |
| 18 | The helper becomes the game (`exec` of `bin/wine`) | `game_exec` | `GR/supervisor/exec.rs:73-86`, `SN/bin/obelisk-steam-host.rs:19-23` |
| 19 | Exec confirmation, process identity, Running phase, Epic presence | `confirm_execution`, `wait_identity`, `Presence::start` | `GR/supervisor/child.rs:28-118`, `GR/supervisor/session.rs:675-695` |
| 20 | Supervision: log, control socket, observation, stop, revocation | `listen`, `respond`, `Session::update` | `GR/supervisor/session.rs:731-889,334-345` |
| 21 | Exit: wait for the process, Finalizing phase, exit Cloud sync, final phase | `wait_for_exit`, `LaunchCloud::finished`, `update` | `GR/supervisor/session.rs:696-728`, `GR/supervisor/child.rs:16-24` |
| 22 | Cleanup: presence stopped, Epic session closed, ownership token deleted, socket and lock released | `Presence::stop`, `Launch::close`, drop of `Host` | `GR/supervisor/session.rs:523-557`, `GR/epic/launch.rs:88-91,206-210` |

Details that weigh on a new path.

- Runtime selection (`GR/voile.rs:61-73`): `LIB/Voile/current` if it contains `bin/wine` (installed by hand by `voile/scripts/install-dev.sh`, it takes precedence); otherwise `LIB/Voile/<pinned version>` if `bin/wine` exists AND if its `runtime.json` carries that version; otherwise no runtime (`WineUnavailable`). If no runtime exists, `install_runtime` downloads the pinned archive (`voile-release.json`: URL, size, SHA-256) over HTTPS with size and SHA-256 check, extracts it with `/usr/bin/tar -xJf` into a hidden folder, checks `bin/wine` and the version, renames, then deletes the other versions except `current` (`GR/voile.rs:116-226`). A Debug build reads `OBELISK_VOILE_ARCHIVE` (local copy of the archive, SHA-256 checked) instead of the download (`GR/sessions.rs:813-822`).
- The Reservation (`Sessions::admit`, `GR/sessions.rs:590-612`) is exclusive per storefront: a single Epic supervisor at a time, also guarded by `start.lock` and `owner.lock` (`GR/supervisor/client.rs:394-403`, `GR/supervisor/session.rs:412-419`).
- The order of the supervisor steps is dictated by the `Launch` trait (`GR/supervisor/session.rs:112-181`): authentication, Cloud, target and architecture, Rosetta gate (then target again after Rosetta installation), host preparation, exec admission.
- The exec confirmation succeeds at the EOF of the helper's CLOEXEC pipe, or, failing that, when the executable of the inspected process equals the canonical `target.executable` after 3 s (`GR/supervisor/child.rs:72-79`). A new engine whose final process does not have this executable path will therefore be confirmed by the pipe EOF or not at all.
- The recorded exit code is that of the tracked `wine` process (`GR/supervisor/child.rs:16-24`). If the exec was never confirmed, the session ends `Failed` (`GR/supervisor/session.rs:715-727`).
- Stop: SIGTERM to the supervisor (when Obelisk quits) calls `Session::terminated` (`GR/supervisor/session.rs:279-289,497-506`): `kill -TERM` of the game, `wineserver -k` of the prefix, 5 s of grace, `kill -KILL` (`GR/supervisor/session.rs:294-320`), then `wine::end_orphans` (`GR/wine.rs:466-490`) which kills through `lsof` any process running a file under the given folder: the runtime folder at stop (`GR/supervisor/session.rs:286-288`), the `Voile` folder at application startup (`AppLifecycle.swift:27`).
- INFERRED, supervision risk: EAC's `Settings.json` contains `"wait_for_game_process_exit": "false"` (MEASURED). If the bootstrapper returns before the game ends, the `wine` process tracked by Obelisk (the one of `Launcher.exe`) ends before the game. The session would then move to Finalizing/Finished while `RocketLeague.exe` still runs in the prefix. To be checked at the first protected attempt.

## 3. Exact form of the launch of a Windows Epic game

### 3.1 Processes, arguments, working directory

READ, assembled from `GR/wine.rs:181-199`, `GR/epic/launch.rs:258-274`, `GR/supervisor/session.rs:596-657`, `GR/supervisor/exec.rs:77-86`.

```
helper  : <app>/Contents/Helpers/SteamNative/obelisk-steam-host game-exec   (arm64, not translated)
          env_clear, then exactly Target.environment; stdin = socketpair; stdout = pipe; stderr = /dev/null
admission (one JSON on the helper's stdin):
  executable        = <runtime>/bin/wine                       (x86-64, so Rosetta)
  arguments         = [ <canonical Mac path of Launcher.exe>,
                        <receipt args: launch_command>, <catalog args: AdditionalCommandLine>,
                        -AUTH_LOGIN=unused, -AUTH_PASSWORD=<code>, -AUTH_TYPE=exchangecode,
                        -epicapp=<app_name>, -epicenv=Prod, -EpicPortal,
                        -epicusername=<display name>, -epicuserid=<account id>,
                        -epiclocale=<locale>, -epicsandboxid=<namespace>,
                        [-epicdeploymentid=<id>], [-epicovt=Z:\...\ownership.bin] ]
  working_directory = <install>/Binaries/Win64                 (parent of the receipt executable)
  log               = ~/Library/Logs/Obelisk/Epic/wine-<app_name>.log
then the helper does exec(executable, arguments) with cwd = working_directory,
  stdin = /dev/null, stdout = /dev/null, stderr = the log (created, truncated, mode 0600)
```

Details.
- Wine's argument 1 is the canonical Unix path of the `.exe`, checked as located in the installation and in PE format (`GR/wine.rs:202-211,555-566`). Wine translates it to `Z:\Users\...`. The path contains a space (`Application Support`).
- The argument order is: executable, receipt arguments, catalog arguments, the nine fixed Epic arguments, `-epicdeploymentid` if it exists, then `-epicovt` added last by `windows_target` (`EK/client/launch.rs:194-217`, `GR/epic/launch.rs:266-272`).
- The working directory and the executable are confined to the installation, symbolic links resolved (`SC/launch.rs:119-128`).
- The `game-exec` helper refuses to run translated (`sysctl.proc_translated`) and requires its arm64 state (`SN/bin/obelisk-steam-host.rs:11-13`, `SN/native.rs:186-198`). It carries the entitlements `allow-dyld-environment-variables` and `disable-library-validation` (MEASURED with `codesign -d --entitlements` on the Debug build of `build/xcode/...`).

### 3.2 Full environment of the Wine process

READ: base `native::environment(home)` (`GR/native.rs:69-102`) then `Config::environment()` (`GR/wine.rs:115-142`), the latter overriding the base (`GR/wine.rs:190-191`).

| Variable | Value | Origin |
|---|---|---|
| `HOME` | the player's home folder (`Locations.home`) | `GR/native.rs:75` |
| `PATH` | `/usr/bin:/bin:/usr/sbin:/sbin` | `GR/native.rs:76-79` |
| `TMPDIR` | `TMPDIR` of the interface, otherwise `std::env::temp_dir()` | `GR/native.rs:80-83` |
| `USER`, `LOGNAME` | taken from the interface if they exist | `GR/native.rs:85-88` |
| `LANG`, `LC_ALL`, `LC_CTYPE` | taken if they exist and are not empty, otherwise `LANG=en_US.UTF-8` | `GR/native.rs:89-100` |
| `WINEPREFIX` | `LIB/Prefixes/<name of the installation folder>` | `GR/wine.rs:52-55,117-120` |
| `WINEDLLOVERRIDES` | `mshtml=;winemenubuilder.exe=d` (runtime with Wine Mono), otherwise `mscoree,mshtml=;winemenubuilder.exe=d` | `GR/wine.rs:97-103` |
| `WINEDEBUG` | first line of `<runtime>/winedebug`, otherwise `err+all,fixme-all` | `GR/wine.rs:107-113` |
| `WINEMSYNC` | `1` (Mach semaphores; the server and all the prefix processes must agree) | `GR/wine.rs:126-129` |
| `WINE_LARGE_ADDRESS_AWARE` | `1` | `GR/wine.rs:130-132` |
| `CX_APPLEGPTK_LIBD3DSHARED_PATH` | `<runtime>/lib/external/libd3dshared.dylib`, only if this file exists | `GR/wine.rs:80-83,134-138` |
| `CX_ACTIVE_GRAPHICS_BACKEND` | `d3dmetal`, only if this file exists | `GR/wine.rs:139` |

No `DYLD_*` variable is set for Wine. The supervisor itself starts with `env_clear` and `PATH=/usr/bin:/bin` only (`GR/supervisor/client.rs:371-378`), hence `std::env::temp_dir()` = `/tmp` in the supervisor (INFERRED: `TMPDIR` absent). The `Request.environment` request is built in the interface process (`GR/epic/client.rs:24`).

MEASURED on this machine: neither `LIB/Voile/current/winedebug` nor `LIB/Voile/v26.3.0-4/winedebug` exist, so `WINEDEBUG=err+all,fixme-all` is the effective value. Both runtimes have `lib/external/libd3dshared.dylib`, so the `CX_*` variables are set.

### 3.3 Wine debug channels

- By default `err+all,fixme-all` (`GR/wine.rs:112`). A `winedebug` file at the root of the runtime replaces this value for ALL of Obelisk's Wine commands (game, `wineboot`, prerequisites), because `environment()` rereads it at each call (`GR/wine.rs:107-113`). Documented in `voile/README.md` (example `err+all,+steamclient`).
- For a run with credentials, do not enable `+process`, `+relay`, `+server` nor `+all`: voile-wine traces the command line under the `process` channel (`voile-wine/dlls/kernelbase/process.c:529`, `voile-wine/dlls/ntdll/unix/process.c:1248`). INFERRED: `+relay` and `+server` can also display it.
- Output: only the stderr of the game process goes to the log; that of `wineboot` is discarded (`GR/wine.rs:247-250`); that of the prerequisites goes to `<prefix>/obelisk-prerequisites.log` (`GR/wine.rs:380-427`).

### 3.4 Epic arguments, one by one

READ in `EK/client/launch.rs` unless stated otherwise. No value is reproduced here.

| Argument | Where the value comes from | Line |
|---|---|---|
| executable | `launch_executable` field of the `.obelisk-install.json` receipt, from the Epic build manifest (MEASURED: `Binaries/Win64/Launcher.exe`) | 129-147 |
| receipt arguments | `launch_command` field, split according to Windows rules (MEASURED: empty for Rocket League) | 139-145, `SC/launch.rs:164-211` |
| catalog arguments | custom attribute `AdditionalCommandLine` of the Epic catalog, read over the network (unknown for Rocket League) | 255-258, 194-197 |
| `-AUTH_LOGIN=unused` | constant | 199 |
| `-AUTH_PASSWORD=<code>` | exchange code: `GET account/api/oauth/exchange` with the access token of the supervisor's own session; 1 to 2048 characters `[A-Za-z0-9_-]` | 200, 272-283 |
| `-AUTH_TYPE=exchangecode` | constant | 201 |
| `-epicapp=<app_name>` | `app_name` of the receipt (Epic application name, public) | 202 |
| `-epicenv=Prod` | constant | 203 |
| `-EpicPortal` | constant | 204 |
| `-epicusername=<display name>` | `display_name` of the session credentials (OAuth response), after an account recheck | 205, 190-193 |
| `-epicuserid=<account id>` | `account_id` of the session credentials; must equal that of the receipt | 206, 172-180 |
| `-epiclocale=<locale>` | `Configuration.language` provided by the interface (`_` becomes `-`), otherwise `en` | 207, 313-325, `GR/epic/mod.rs:105` |
| `-epicsandboxid=<namespace>` | `namespace` of the receipt (public sandbox identifier, the same as EAC's `sandboxid`, section 3.7) | 208 |
| `-epicdeploymentid=<id>` | `deployment_id` of the receipt, only if it exists (MEASURED: absent) | 210-217 |
| `-epicovt=Z:\...\ownership.bin` | Windows path of a file that contains the bytes of the ownership token, added only if the catalog carries `OwnershipToken=true` (unknown for Rocket League) | `GR/epic/launch.rs:242-272`, `EK/client/launch.rs:259-271` |

### 3.5 Obtaining and passing the exchange code and the ownership token

Exchange code no. 1, the one of the supervisor's session.
- The interface requests `account_id` then `exchange_code` (`GET account/api/oauth/exchange`) with its own session, whose renewed credentials are saved (`GR/epic/content.rs:393-406`, `EK/client.rs:84-104`).
- It goes through a private socketpair: the supervisor's stdin receives the JSON `Request`, stderr sends back a single acknowledgment, and the code comment specifies that neither one reaches a log file (`GR/supervisor/client.rs:407-418`). According to the comment of `GR/epic/mod.rs:221-235`, the `Request` is never persisted, never formatted with `Debug`, never put in `argv`.
- The supervisor exchanges it for its own session (`grant_type=exchange_code`) and checks that the account is the right one, otherwise it revokes (`EK/auth.rs:137-146`). This session is distinct from the interface's stored token (`GR/epic/launch.rs:21`).

Exchange code no. 2 and ownership token, those of the game.
- The launch plan is made in the supervisor: ownership check, catalog read, then `try_join!` of the exchange code and, if required, of the ownership token (`POST ecommerceintegration .../identities/<account>/ownershipToken`, body `nsCatalogItemId=<namespace>:<catalogItemId>`) (`EK/client/launch.rs:235-283`, `EK/auth.rs:228-257`).
- Code no. 2 goes into `LaunchPlan.target.arguments`. `LaunchPlan` deliberately has no `Debug` (`EK/client/launch.rs:17-26`). It then travels in the `Admission` message (JSON, helper's socketpair, `GR/supervisor/exec.rs:27-35`), then becomes `argv` of the Wine process. It is therefore present in `argv` of the Wine process during the life of the game (INFERRED from the fact that it is part of `Target.arguments`); I do not know who can read it under macOS 27.
- The ownership token is written in an `obelisk-epic-ownership-*` folder in 0700, file `ownership.bin` (`GR/epic/launch.rs:242-256`). Only its Windows path `Z:\...` is in `argv`. The folder lives as long as `Host` lives: `run` keeps it until it returns, so after the game exits and the exit Cloud sync (`GR/epic/launch.rs:88-91,182-189`, `GR/supervisor/session.rs:599`).
- Neither of the two appears in a receipt (`.obelisk-install.json`: 12 keys, none carries a token or a code; `account_id` is an account identifier, MEASURED) nor in the `Record` session log (`GR/supervisor/mod.rs:111-135`).

### 3.6 What is done without any credentials

The preparation of the prefix and the prerequisites uses no Epic credentials: `prepare_windows` reads only the local receipt (`installed_launch`, `EK/client/launch.rs:63-99`, called by `GR/epic/windows.rs:25-31`). A lab engine can therefore prepare and test a prefix without an account.

### 3.7 Executable launched for Rocket League

MEASURED.
- `.obelisk-install.json` receipt of the installation folder (non-secret fields only): `app_name=Sugar`, `platform=Windows`, `build_version=++Prime+Update60-CL-528869-pchotfix4`, `launch_executable=Binaries/Win64/Launcher.exe`, empty `launch_command`, no `deployment_id`, prerequisite `ids=["EAC_Service"]`, `path=Binaries/Win64/EasyAntiCheat/EasyAntiCheat_EOS_Setup.exe`, `arguments="install e6bcca5b37d0457ca881aec508205542"`.
- SHA-256 unchanged since the start of the effort: `RocketLeague.exe` `ca3a9075...38fa`, `RocketLeague_EAC.exe` `903e7aba...14d1` (compared with `evidence/game-sha256-start.txt`). `Launcher.exe`: `40d1bb2d02989907894d29d59769f51975ea9b930255696e49be1f61fd515eb9` (not present in the starting file).
- `EasyAntiCheat/Settings.json`: `executable=RocketLeague.exe`, `productid=e6bcca5b37d0457ca881aec508205542`, `sandboxid=9773aa1aa54f4f7b80e44bef04986cea`, `deploymentid=da32ae9c12ae40e8a112c52e1f17f3ba`, `wait_for_game_process_exit=false`, `hide_bootstrapper=false`, `hide_gui=false`.

So Obelisk launches `Launcher.exe`, never `RocketLeague.exe` nor `RocketLeague_EAC.exe` directly, from `Binaries/Win64`.

Static analysis of `Launcher.exe` (.NET assembly 1.0.0.0, company "Psyonix LLC"), IL decoded with `dnfile` and `dncil` installed in `EXP/tools/venv`, without execution. Logic of `Main` (RVA 8512) and of the `LaunchOptions` iterator (RVA 8880):
1. The current directory becomes the assembly folder (`Environment.CurrentDirectory = dirname(Assembly.GetEntryAssembly().Location)`).
2. Ordered list of options: `("-eac","RocketLeague_EAC.exe")`, `("-noeac","RocketLeague.exe")`, `("-offline","RocketLeague.exe")`, filtered by `File.Exists`. If none exists, return code 1.
3. For each option in order, if an argument equals the flag (`OrdinalIgnoreCase`), launches the associated executable with the remaining arguments (the flag removed).
4. Without a flag, launches the first: `RocketLeague_EAC.exe` with ALL the arguments (`Main` 042c-043c).
5. `Launch(exe, args)`: `Process.Start(new ProcessStartInfo(exe, string.Join(" ", args)) { UseShellExecute=false, RedirectStandardOutput=false })`, `WaitForExit()`, returns the exit code (`Launch` 02f0-032f). The arguments are joined by a space without quotes (INFERRED consequence: an `-epicusername=` with a space would be split into two arguments for the child).
6. An exception writes `Launcher.log` in the current directory (`Main` 0446-0452).

Consequences. The default protected path is `Launcher.exe` then `RocketLeague_EAC.exe` (EAC bootstrapper 1.9.3 according to the log cited by `rocket-league-macos.md:12`), which reads `Settings.json`, downloads the `linux64` module from `modules-cdn.eac-prod.on.epicgames.com/modules/{{productid}}/{{deploymentid}}/{{system}}` (strings of the binary) and loads it through a Wine-specific loader (`wine_get_version`, `/proc/self/fd/%i`, `/proc/%i/maps`), then launches `RocketLeague.exe`. Under macOS Wine this step fails with launcher code 206 (`rocket-league-macos.md:19-27,47-61`). `-noeac` and `-offline` launch the game without EAC: out of scope.

## 4. Wine prefix

### 4.0 Installed library (read-only listing)

`LIB` (`drwxr-xr-x`), names and sizes (`du -sk`, 1 KB = 1024 bytes):

| Entry | Mode | Size | Remark |
|---|---|---|---|
| `EpicSessions/` | 0700 | not measured | logs and sockets of the Epic sessions; content not read |
| `SteamSessions/`, `SteamCloud/`, `SteamCloudValidation/` | 0700 | not measured | not read |
| `SteamCloud-sdk/` | 0755 | not measured | not read |
| `Games/Epic/` | 0755 | `Games/`: 103,290,020 KB in total | 6 installations: 3 `-Mac`, 3 `-Windows`; Rocket League is `epic-52edd447ec86748fc2afecc6cd67acc7748a5461-Windows`, 42,690,576 KB (receipt: 43,667,660,596 bytes) |
| `Games/Steam/` | 0755 | included above | not detailed |
| `Prefixes/` | 0755 | 8,095,316 KB | 7 prefixes, 0700 each: 2 Epic Windows, 5 Steam; from 1,086,360 to 1,256,688 KB each; the Rocket League one: 1,086,672 KB |
| `Voile/current/` | 0755 | 1,396,744 KB | development runtime: `bin`, `lib`, `share`, `steam`, without `runtime.json` or `licenses`; Wine Mono `wine-mono-10.4.1` in `share/wine/mono` |
| `Voile/v26.3.0-4/` | 0755 | 523,760 KB | release archive: `bin`, `lib`, `licenses`, `runtime.json`, `share`, `steam`; no Wine Mono in `share/wine/mono` |
| `epic-installations-<hex>.json`, `steam-installations-<id>.json`, `library.lock` | 0644 | 1,841 / 1,719 / 0 bytes | account receipts; not read |

`runtime.json` of `Voile/v26.3.0-4` (MEASURED): `version v26.3.0-4`, `voile 6d2bbb44...`, `crossover 26.3.0`, `voile-wine 9861d8b4...`, `proton 5b89db94...`, `d3dmetal "Evaluation environment for Windows games 4.0 beta 2"`. `Voile/current` has no manifest: its exact version cannot be identified from a file.

### 4.1 Creation and update

READ, `GR/wine.rs`.
- Path: `LIB/Prefixes/<name of the installation folder>` (`GR/wine.rs:52-55`, `GR/locations.rs:44`). For Rocket League: `epic-52edd447ec86748fc2afecc6cd67acc7748a5461-Windows` (MEASURED), mode 0700 (`GR/files.rs:66-77`, called by `GR/wine.rs:219`).
- `boot` (`GR/wine.rs:215-237`): missing runtime gives `WineUnavailable`; creation of the folder; `wineboot --init` if `system.reg` is missing; if the runtime carries Wine Mono and the `obelisk-mono` marker differs, `wineboot --update` (except at creation) then writing of the marker; finally `unlink_home_folders` turns into empty folders the links to the Mac home left by older runtimes (`GR/wine.rs:266-282`).
- `wineboot`: `<runtime>/bin/wine wineboot --init|--update`, `env_clear`, native + Wine environment, null stdio; then `wineserver -w` with `WINEPREFIX` only (`GR/wine.rs:241-260`). Failure if either of the two is not a success.
- The prefix is "prepared before each launch" (`CONTEXT.md`, Wine prefix entry); `prepare` also creates the log folder (`GR/wine.rs:172-175`).
- Removal: `Installation::remove` does `wineserver -k` then `remove_dir_all`, without following links (`GR/wine.rs:337-356`).
- Windows profile: `drive_c/users/crossover`, name fixed by CrossOver (`GR/wine.rs:22-39`). It serves as the root for Cloud saves (`GR/epic/mod.rs:120-129`).

### 4.2 Measured content of the Rocket League prefix (listing only)

`LIB/Prefixes/epic-52edd447ec86748fc2afecc6cd67acc7748a5461-Windows`: 1,086,672 KB (1.04 GiB), created on September 26 at 22:37.
- Root: `.update-timestamp` (12 bytes), `dosdevices/`, `drive_c/`, `obelisk-mono`, `obelisk-prerequisites`, `obelisk-prerequisites.log`, `system.reg` (3.3 MB), `user.reg` (66 KB), `userdef.reg` (4 KB).
- `dosdevices/`: `c:` to `../drive_c`, `z:` to `/`, plus `d:` to `/Volumes/VirtualBuddy_v2.2-417` and `d::` to `/dev/rdisk13s1` (drives detected by `wineboot` at creation time, unrelated to the game).
- `drive_c/`: `Program Files` (Common Files, Internet Explorer, Windows Media Player, Windows NT), `Program Files (x86)` (Common Files, `EasyAntiCheat_EOS` with `EasyAntiCheat_EOS.exe`, Internet Explorer, Windows Media Player, Windows NT), `ProgramData/Microsoft`, `users/` (`Public`, `crossover`), `windows/`.
- `drive_c/users/crossover/`: `AppData` (`Local` with `Microsoft` and `Temp`, `LocalLow`, `Roaming` with `EasyAntiCheat` and `Microsoft`), `Contacts`, `Desktop`, `Documents`, `Downloads`, `Favorites`, `Links`, `Music`, `Pictures`, `Saved Games`, `Searches`, `Videos`. These are real folders, not links; all are empty except `Pictures`, which contains a `Screenshots` subfolder. There is no `Documents/My Games`: INFERRED, the prefix never ran the game all the way.
- `AppData/Roaming/EasyAntiCheat/`: `service.log` and `e6bcca5b37d0457ca881aec508205542/da32ae9c12ae40e8a112c52e1f17f3ba/anticheatlauncher.log` (names only; content not read).
- Content of the Obelisk markers (non-secret): `obelisk-mono` = `wine-mono-10.4.1`; `obelisk-prerequisites` = `epic:EAC_Service`; `obelisk-prerequisites.log` = three lines (`msync: bootstrapped mach port on wine-164e476-msync.`, `msync: up and running.`, `err:ntoskrnl:ZwLoadDriver failed to create driver ... winebth: c00000e5`).

### 4.3 Prerequisites

- For Epic, the prerequisite comes from the receipt: a program under the installation, with a command line, identified `epic:<ids joined by comma>`, or `epic:<path>` if `ids` is empty (`GR/epic/windows.rs:52-77`). A path that leaves the installation is ignored.
- For Rocket League: `Binaries/Win64/EasyAntiCheat/EasyAntiCheat_EOS_Setup.exe install e6bcca5b37d0457ca881aec508205542`, launched by `wine <program> install <productid>` with the program's folder as working directory (`GR/wine.rs:396-437`). A `.cmd` or `.bat` would go through `wine cmd /c Z:\...`.
- Each id runs only once per prefix (`obelisk-prerequisites` marker, one line per id). Success codes 0, 1638, 3010; timeout 10 minutes then `wineserver -k`; a failure replays at the next launch and the game starts anyway (`GR/wine.rs:25-33,362-393`). A final `wineserver -w` waits for the end of the installer processes.
- MEASURED: the EAC service is installed in this prefix (`Program Files (x86)/EasyAntiCheat_EOS`), id `epic:EAC_Service` recorded.

### 4.4 DLL overrides and registry

- Obelisk writes no registry key itself: a grep of `crates/game-runtime/src` (excluding tests) for `reg`, `regedit`, `HKEY`, `winetricks` finds only the existence test of `system.reg` (`GR/wine.rs:220`). Settings go through environment variables. For Epic, `furnish` is the trait's default behavior, which does nothing (`GR/wine.rs:151-153`); Steam, for its part, copies `steamclient.dll` and `steamclient64.dll` into `Program Files (x86)/Steam` of the prefix (`GR/steam/windows.rs:63-77`).
- `WINEDLLOVERRIDES`: `winemenubuilder.exe=d` (no menu entries); `mshtml=` (no Gecko prompt); `mscoree` is neutralized only if the runtime has no Wine Mono (`GR/wine.rs:94-103`).
- The DLLs that Wine replaces or adds come from the runtime: see 4.5.
- Actual registry of the prefix: not read (section 0). INFERRED: `wineboot` registers the .NET of Wine Mono (`GR/wine.rs:224-226`) and the EAC installer registers its service.

### 4.5 D3DMetal

- In the runtime (MEASURED, `LIB/Voile/current/lib/wine`): `x86_64-windows/d3d10.dll`, `d3d11.dll`, `d3d12.dll`, `dxgi.dll`, `nvapi64.dll` dated July 10 (Apple DLLs), next to Wine's `wined3d.dll` dated September 26; on the Unix side, `x86_64-unix/d3d10.so`, `d3d11.so`, `d3d12.so`, `dxgi.so`, `nvapi64.so`, `nvngx-on-metalfx.so` are links to `../../external/libd3dshared.dylib`; `lib/external/D3DMetal.framework` is present. The `i386-windows` part stays Wine's (D3DMetal covers only 64-bit).
- In Obelisk: `Config::d3dmetal()` tests `lib/external/libd3dshared.dylib`; if it exists, `CX_APPLEGPTK_LIBD3DSHARED_PATH` points to it and `CX_ACTIVE_GRAPHICS_BACKEND=d3dmetal` (`GR/wine.rs:78-83,134-140`). On the Wine side, `voile-wine/dlls/ntdll/unix/loader.c:1443` reads the first, `dlls/win32u/d3dkmt.c:719` and `dlls/win32u/vulkan.c:3007` read the second.
- `OBELISK_GRAPHICS` is read only by `voile/scripts/wine-dev.sh` (values `wined3d` by default or `d3dmetal`, which overlays D3DMetal on a clone of the runtime). It exists nowhere in Obelisk's code (grep on `crates`, `apps`, `scripts`, `packaging`). INFERRED: Obelisk cannot choose wined3d instead of D3DMetal, since the DLLs are already overlaid in the installed runtime.
- Voile's Wine build is x86-64 (`--enable-archs=i386,x86_64 --without-x --without-vulkan`, `voile/scripts/build.sh:33-34`) and runs under Rosetta: `bin/wine` and `bin/wineserver` are x86_64 Mach-O (MEASURED, `lipo -archs`).

### 4.6 Role of Voile's scripts and test

READ in `voile/scripts/*` and `voile/tests/d3d11-smoke.c`. Voile does not create the game prefixes: Obelisk does (`wine::boot`). Voile provides the runtime and a means to test it.
- `build.sh`: builds Wine for x86-64 with `--enable-archs=i386,x86_64 --disable-tests --without-x --without-gstreamer --without-sdl --without-vulkan` (lines 33-34), requires Rosetta (`arch -x86_64 /usr/bin/true`, line 43), produces `build/stage`.
- `install-dev.sh`: copies `build/stage` into `LIB/Voile/current` (or `$OBELISK_LIBRARY_DIRECTORY/Voile/current`), places the D3DMetal DLLs over `lib/` and copies `steamclient.dll` and `steamclient64.dll` into `steam/`.
- `package.sh <version>`: builds, removes the debug info, adds D3DMetal after the `strip` (Apple's license), writes `runtime.json`, creates `voile-<version>.tar.xz` and its SHA-256, then checks the extracted archive: `wineboot --init` in a fresh prefix then 64-bit and 32-bit `cmd.exe` with `ver`, under `WINEDEBUG=-all` and `WINEDLLOVERRIDES=mscoree,mshtml=;winemenubuilder.exe=d`. Refuses uncommitted changes.
- `publish.sh`: creates the GitHub release of the version. `import-d3dmetal.sh`: extracts the D3DMetal Redistributables from the Game Porting Toolkit disk image.
- `wine-dev.sh`: launches the Wine of `build/stage` with `WINEPREFIX=build/prefix` by default; `OBELISK_GRAPHICS=d3dmetal` overlays D3DMetal on an APFS clone of the runtime and sets `CX_APPLEGPTK_LIBD3DSHARED_PATH` and `CX_ACTIVE_GRAPHICS_BACKEND=d3dmetal`. It does not set the other variables of 3.2.
- `tests/d3d11-smoke.c`: creates a D3D11 device and swap chain, presents 120 frames and displays the adapter and the feature level; compiles with `x86_64-w64-mingw32-clang`. According to Voile's `README`, wined3d stops at 10_1 and D3DMetal announces "AMD Compatibility Mode" at 11_0. It is a reusable graphics acceptance test for a new path, without EAC or credentials.
- There is no other test in `voile/tests/`.

## 5. Logs, launch without interface, credentials

### 5.1 Where the logs go

| What | Where | Source |
|---|---|---|
| Wine's stderr during the game | `~/Library/Logs/Obelisk/Epic/wine-<app_name>.log` (Rocket League: `wine-Sugar.log`, 297 bytes, mode 0600, Sept 26 22:38; content not read) | `GR/epic/windows.rs:19-24`, `GR/locations.rs:42`, `GR/supervisor/exec.rs:57-69` |
| output of the prerequisite installers | `<prefix>/obelisk-prerequisites.log` (rewritten at each launch that runs one) | `GR/wine.rs:29,380` |
| EAC log | `<prefix>/drive_c/users/crossover/AppData/Roaming/EasyAntiCheat/<productid>/<deploymentid>/anticheatlauncher.log` and `service.log` (MEASURED: files present) | `rocket-league-macos.md:19-25` |
| `Launcher.exe` log | `Launcher.log` in `Binaries/Win64`, only on exception (IL) | section 3.7 |
| session log | `LIB/EpicSessions/` (`journal::write`), control socket in `/tmp/obelisk-session-*/control` (0700) | `GR/supervisor/session.rs:420-447` |
| Epic diagnostic manifest | `~/Library/Logs/Obelisk/Epic/manifest-Sugar-Windows.json` and `.manifest` | MEASURED (listing) |

The `~/Library/Logs/Obelisk/Epic` folder is in 0700, the Wine logs in 0600. The game's stdout is discarded (`GR/supervisor/exec.rs:82-84`).

### 5.2 Launching without the interface: what exists and what is missing

READ: there is no launch CLI. The binaries of the repository are `uniffi-bindgen` (`crates/obelisk-bridge/src/bin/uniffi-bindgen.rs`), `obelisk-steam-runtime-package` (`crates/game-runtime/Cargo.toml:34-36`, packaging tool), the `obelisk-steam-host` helper (`crates/steam-native/src/bin/obelisk-steam-host.rs`) and a Steam Cloud probe example (`crates/steam-native/examples/cloud_storage_probe.rs`, variables `OBELISK_CLOUD_PROBE_*`). The `scripts/*.sh` scripts build, test, sign or open the application (`scripts/run.sh` does `open` of `Obelisk.app`); none launches a game. `README.md:98`: the tests "do not launch games".

Four ways exist, from the most faithful to the lightest.
1. **The application itself**, with an isolated library: `OBELISK_LIBRARY_DIRECTORY` is read in `apps/macos/Obelisk/App/AppIdentity.swift:11` (and does not change the Keychain identifier, `README.md:129`). The locations are derived by `obeliskLocations(library:bundle:home:)` (`AppIdentity.swift:21`, `GR/locations.rs:30-48`). It is the only way that exercises steps 1 to 22 without new code.
2. **The helper alone**: `obelisk-steam-host game-exec` reads a JSON `Admission` on stdin and does `exec` (`crates/steam-native/tests/exec_game.rs:15-33` shows how `Admission` is built and sent with `exec::send`). It is enough to provide the exact environment of 3.2 (the helper passes it as is) and an `Admission` with `executable=<runtime>/bin/wine`. The Debug build is at `build/xcode/Build/Products/Debug/Obelisk.app/Contents/Helpers/SteamNative/obelisk-steam-host` (arm64, MEASURED). The helper must run as native arm64, not under Rosetta.
3. **The supervisor with fake services**: `lifecycle::serve::<Launch<S>>` is generic over `Services`; `FakeEpic` in `GR/epic/launch/tests.rs:96-140` shows how to provide a `LaunchPlan` without network. The `EpicHarness` harness re-enters the test binary as a throwaway supervisor (`GR/supervisor/session/tests.rs:135-162,268-275`). A "lab" `Services` implementation is the intended seam for a launch outside the interface (section 7).
4. **Wine alone**: `voile/scripts/wine-dev.sh <program>` (runtime `voile/build/stage`, prefix `voile/build/prefix` or `WINEPREFIX`, `OBELISK_GRAPHICS=d3dmetal`) or the direct call of the installed `bin/wine`. The variables of 3.2 must then be reproduced by hand, because `wine-dev.sh` sets neither `WINEMSYNC`, nor `WINEDLLOVERRIDES`, nor `WINEDEBUG`. It is the way the attempts without authentication used (`rocket-league-macos.md:27-45`, APFS copies of the prefix).

What a faithful run outside the interface must provide: a `Locations` (library, home, bundle), an installed receipt whose account is the session's, a runtime (`Voile/current` or pinned version), the arm64 helper and the Rosetta bundle (`Contents/Helpers/Obelisk Rosetta.app`, MEASURED present in the Debug build; `GR/epic/launch.rs:229-238` locates it as the grandparent of `config.helper`, `GR/supervisor/rosetta.rs:7-9` appends `Obelisk Rosetta.app/Contents/MacOS/ObeliskRosetta`), and a fresh exchange code for an account that owns the game.

### 5.3 How the credentials stay out of the logs

READ.
- Paths without `argv`: `Request` over socketpair stdin/stderr, never persisted nor `Debug` (`GR/epic/mod.rs:221-235`, `GR/supervisor/client.rs:407-418`). `Admission` over socketpair, size capped at 1 MiB (`GR/supervisor/exec.rs:24-54`).
- Supervisor without inherited environment (`env_clear`, `PATH` only), without stdout, stderr dedicated to the acknowledgment (`GR/supervisor/client.rs:371-378,412-417`). `game-exec` helper with stderr to `/dev/null` (`GR/supervisor/session.rs:608-610`).
- `LaunchPlan` without `Debug` (`EK/client/launch.rs:17-18`). The errors are localization keys `backend.epic.*` (`GR/epic/mod.rs:21-50`).
- Ownership token in a 0700 folder, atomic write (`GR/epic/launch.rs:242-256`). Zeroization for the Steam credentials (`GR/protocol.rs:1-56`).
- Wine log 0600, created and truncated at each launch (`GR/supervisor/exec.rs:57-69`).

Limit: nothing prevents Wine from logging a command line if the channels of 3.3 are enabled, and the EAC log can contain whatever the publisher writes into it (unknown). Exchange code no. 2 is in `argv` of the Wine process.

## 6. Rust entry points for a "Linux userland under FEX" path

### 6.1 What is neutral and reusable as is

- `Target { executable, arguments, working_directory, environment, log }` (`GR/supervisor/mod.rs:40-50`): engine-independent spawn description.
- `exec::Admission` and `game_exec` (`GR/supervisor/exec.rs`): the arm64 helper does `exec` of any absolute `executable`, with the working directory, the environment inherited from the helper and a stderr redirected to `log`.
- The `Launch` trait (`GR/supervisor/session.rs:117-181`) and the whole `run` cycle: authentication, Cloud, target, Rosetta gate, host, admission, confirmation, supervision, exit. `Prepared { host, helper, subcommand, environment }` (`GR/supervisor/session.rs:54-62`) already makes it possible to choose another helper and another subcommand.
- The Epic authentication and the launch plan (`EK/client/launch.rs`), the ownership token (`GR/epic/launch.rs:242-256`), `native::environment` (`GR/native.rs:69-102`), the log and the Reservation.
- The path checks (`SC/launch.rs`) and `portable_executable` (`GR/wine.rs:555-566`).
- The Wine seam is already shared by two storefronts: Steam also calls `wine::prepare` and `wine::target`, and only inserts `C:\windows\system32\steam.exe` at the head of the arguments (`GR/steam.rs:88`, `GR/steam/windows.rs:79-90`). A second engine can therefore plug in at this spot for both.

### 6.2 What is specific to Wine under macOS and Rosetta

| Element | File:line | Why it is specific |
|---|---|---|
| `ARCHITECTURE = Intel` and the Rosetta gate | `GR/wine.rs:20-21`, `GR/supervisor/session.rs:581`, `GR/supervisor/architecture.rs:143-172`, `GR/supervisor/rosetta.rs` | `bin/wine` is x86-64; the gate tests the translation and offers the installation of Rosetta |
| `USER = "crossover"`, `profile()` | `GR/wine.rs:22-39` | profile name of CrossOver prefixes; also serves as Cloud root (`GR/epic/mod.rs:120-129`) |
| `WINEMSYNC`, `WINE_LARGE_ADDRESS_AWARE`, `CX_*`, `libd3dshared.dylib` | `GR/wine.rs:80-83,115-142` | Mach semaphores, CrossOver, D3DMetal: none of this exists under Linux |
| `windows_path` = `"Z:" + path` | `GR/wine.rs:296-298`, `GR/epic/launch.rs:268-271` | `Z:` is the Mac root; in a Linux userland, `Z:` would be the root of the Linux guest, so the ownership token and the installation must be visible there |
| `unlink_home_folders` | `GR/wine.rs:262-282` | links to the macOS home left by older runtimes |
| `wineboot`, `wineserver -w/-k` launched as host commands | `GR/wine.rs:241-260,455-459,512-522` | to be run in the guest for a Linux engine |
| `end_orphans` through `lsof` + `kill` on the files under `Voile` | `GR/wine.rs:466-508`, called from `GR/supervisor/session.rs:286-288` (runtime folder) and `crates/obelisk-bridge/src/locations.rs:52-57` (`Voile` folder, at application startup, `AppLifecycle.swift:27`) | relies on Mach-O mapped from `Voile/` |
| `Installation::of` (`windows` platform) and `runtime()` | `GR/wine.rs:311-332` | the runtime selector is here, fixed on `locations.voile` |
| Installation, version and checksum of Voile | `GR/voile.rs`, `voile-release.json`, `GR/sessions.rs:784-822` | pinned `tar.xz` archive, development `Voile/current` |
| `Request.wine`, `Configuration.prefix`, `runs_in`, `validate` | `GR/epic/mod.rs:93-164,233-234` | `Request.wine` is the only carried field that says "this Windows build runs in Wine"; `Configuration.prefix` duplicates it on the Cloud side |
| `Handover::voile/prepare` | `GR/epic/content.rs:369-401` | installs Voile and prepares the prefix before the exchange code |
| `Launch::wine()` and the stops that depend on it | `GR/supervisor/session.rs:134-136,286-288,312-314,482` | `stop_game` calls `wine::stop` |
| Helper entitlements (`allow-dyld-environment-variables`, `disable-library-validation`) and refusal to be translated | `packaging/steam-host.entitlements`, `SN/bin/obelisk-steam-host.rs:11-13` | constraints of the binary that does `exec` |

### 6.3 Minimal interface proposal

The three requested points are placed as follows: the runtime selector is `Installation::runtime` with `voile::runtime` (`GR/wine.rs:330-332`, `GR/voile.rs:61-73`); the spawn function is `wine::target` (`GR/wine.rs:181-199`); the environment assembly is `native::environment` plus `Config::environment` (`GR/native.rs:69-102`, `GR/wine.rs:115-142`), merged in `wine::target` and reused by `wineboot` and the prerequisites.

Goal: a single insertion point between the pipeline (unchanged) and the engine, without touching the supervisor, the Cloud or Epic. The names are proposals; each line indicates the current function that becomes the Wine implementation.

```rust
// crates/game-runtime/src/engine.rs (new)
pub(crate) trait Engine: Send + Sync {
    /// Architecture of the binary the helper will execute. `None`: no Rosetta gate.
    fn architecture(&self) -> Option<Architecture>;            // wine::ARCHITECTURE
    /// Engine installed and complete? (runtime or rootfs present)
    fn installed(&self) -> bool;                               // Config::installed
    /// Prepares the prefix: creation, update, prerequisites. Blocking, can take minutes.
    fn prepare(&self, home: &Path, install: &Path, target: &LaunchTarget,
               storefront: &dyn Storefront) -> Result<(), Error>;   // wine::prepare
    /// Engine-specific variables, merged over `native::environment(home)` for the game,
    /// for the prefix preparation and for the prerequisites.
    fn environment(&self) -> HashMap<String, String>;           // Config::environment
    /// Spawn form of the game: the arm64 helper will exec `Target.executable`.
    fn target(&self, install: &Path, target: &LaunchTarget,
              environment: HashMap<String, String>) -> Result<Target, Error>;   // wine::target
    /// Path as the Windows program sees it (Z:\... today).
    fn guest_path(&self, host: &Path) -> String;                // wine::windows_path
    /// Root of the Windows profile on the host side (Cloud, saves).
    fn profile(&self) -> PathBuf;                               // wine::profile(prefix)
    /// Ends all the processes of the prefix.
    fn stop(&self);                                             // wine::stop
    /// Ends the processes that a dead Obelisk left behind. Returns their count.
    fn end_orphans(&self) -> usize;                             // wine::end_orphans
}

#[derive(Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "kebab-case")]
pub(crate) enum EngineConfig {
    WineRosetta(wine::Config),        // the existing one
    LinuxFex(fex::Config),            // new: rootfs, FEX host, prefix in the guest, log
}
```

Hookups, in the order I would do them.
1. `Request.wine: Option<wine::Config>` (`GR/epic/mod.rs:233-234`) becomes `engine: Option<EngineConfig>`; `Configuration.prefix` stays (Cloud root, `runs_in` compares with `engine.prefix()`).
2. Runtime selector: `Installation::runtime` (`GR/wine.rs:330-332`) returns an `EngineConfig`. Proposed choice: a marker in the prefix (`obelisk-engine`) or, in development, an `Engines/linux-fex/current` folder that takes precedence, on the model of `Voile/current` (`GR/voile.rs:21,65-73`). Add `engines` to `Locations` (`GR/locations.rs:7-28`) and to its UniFFI mirror (`crates/obelisk-bridge/src/locations.rs:7-28,31-50,59-77`), then regenerate the Swift bindings (`scripts/build-rust.sh`).
3. `Handover::prepare` and `voile()` (`GR/epic/content.rs:369-401`) call `Engine::prepare` and the installation of the corresponding engine; `Canceller::install_runtime` (`GR/sessions.rs:784-810`) becomes generic over the engine.
4. `windows_target` (`GR/epic/launch.rs:258-274`) calls `Engine::target` and `Engine::guest_path` for `-epicovt`. It returns `engine.architecture()`.
5. `Launch::wine()` (`GR/supervisor/session.rs:134-136`) becomes `engine()`; `stop_game` and `terminated` call `Engine::stop` and `end_orphans`.
6. The Rosetta gate test (`GR/supervisor/session.rs:581`) is already conditioned on `architecture == Some(Intel)`: a FEX engine that returns `Some(Arm64)` or `None` bypasses it without any other change.

For `Engine::target` of a FEX engine, the contract to respect is that of the helper: absolute, existing `executable`, whose canonical path will be compared with the process identity (`GR/supervisor/session.rs:611-614`, `GR/supervisor/child.rs:94-118`). It can be arm64 (native FEX host) or x86-64 (current case, translated by Rosetta); only the helper itself must stay native arm64. The assembly of the guest environment (Wine variables on the Linux side, FEX variables) is to be defined by the FEX team: the `HashMap` of `Target.environment` is passed as is to the final process (`GR/supervisor/exec.rs:13-14`). I have no measurement on the future Darwin FEX host; these constraints are INFERRED from the code read.

Shortest path for a lab attempt (without touching the supervisor): produce a `Target` with a function analogous to `wine::target` and send it to the `game-exec` helper (way 2 of 5.2). The `Engine` trait becomes necessary only to plug the engine into the application flow.

### 6.4 Other points of attention for the hookup

- Wine Mono: `Launcher.exe` is managed. Either the guest ships Wine Mono (equivalent of `Voile/current/share/wine/mono/wine-mono-10.4.1`, `GR/wine.rs:86-92`), or the attempt launches `RocketLeague_EAC.exe` directly with the arguments that `Launcher.exe` would have passed (section 3.7, reproduces its default logic without modifying any binary; decision to be made by the team).
- File visibility in the guest: the installation (`Games/Epic/...`), the ownership token file (`/tmp/obelisk-epic-ownership-*/ownership.bin`) and the prefix must be reachable by the game. Today `Z:` exposes them through the Mac root.
- Cloud saves: the EGS sync reads the Windows profile on the host side (`GR/epic/mod.rs:120-129`). A prefix in a guest rootfs must stay accessible to the host, or the sync must be disabled by a `Launch::cloud` that returns `None` (`GR/supervisor/session.rs:144-147`).
- 16 KiB memory pages of the host: topic for the FEX/Darwin team, not covered here.

### 6.5 What the old documents said about the engine choice

Read through `git show 7d4d22f^:...` (deleted documents, predating the Wine wiring, old name "Forge"). They confirm the intent of a selector, which the code does not have yet.
- `docs/architecture.md:86-92` ("Compatibility engines"): the catalog stores game references, not a Wine version; a future Windows launch implementation "can choose an engine and a profile for each game"; the configuration of an engine stays separate from the user's files and allows going back to a previous version.
- `docs/research-game-runtime.md:43,47,51`: keep the CPU translator, the graphics engine and the synchronization mode replaceable; a launch profile must record the Windows prefix, the store client requirements, the executable and its arguments, the rendering engine, the CPU translator, the synchronization mode, the dependencies, the DLL overrides and the environment; plan for Wine ARM64EC with a macOS CPU translator rather than assuming Rosetta forever.
- `docs/epic-protocol.md:119-127` ("Launching"): describes the launch plan that the current code implements (fresh exchange code, ownership token written in a private file and passed through `-epicovt`, working directory = parent of the executable) and states that the exchange codes and tokens are never stored in the receipts nor logged.

## 7. Starting a protected EAC session from the command line, without credentials in the commands or the reports

Mechanism described only: nothing was launched or written. Goal: a controlled attempt with EAC intact, where no secret value goes through a command, a repository file, a log or a report.

### 7.1 What the launch requires (READ)

- An Epic account that owns Rocket League and whose identifier is the receipt's (`EK/client/launch.rs:172-180`).
- A live Epic session for this account, from which come exchange code no. 2 and, if the catalog requires it, the ownership token (`EK/client/launch.rs:183-188`).
- An open Begin gate if going through the supervisor (`GR/supervisor/session.rs:569-571`).

### 7.2 Mechanism A (recommended for the first session): Obelisk as credential broker

The Obelisk application, launched by the player, already holds the session (Keychain, refresh token). The lab engine is plugged in as in 6.3 and chosen by a marker or an `Engines/.../current` folder. The player clicks Launch. No secret value appears in a command, a file of the attempt or a report; the Keychain dialog, if there is one, is the player's. The attempt collects only non-secret facts: session phase, exit code, EAC launcher codes (206, 208, 210, ...), durations, engine log. Drawback: no command-line scripting.

### 7.3 Mechanism B: lab launcher reading an environment file

A small binary (example or test of `game-runtime`, never added to the application) that reuses `Launch<S>` with a lab `Services`, or that directly composes `Target` then `Admission` for `game-exec`.

All the variable and key names below are proposals: nothing of the kind exists in the repository.

1. Input: the PATH of an environment file, passed through a variable (for example `OBELISK_LAB_ENV_FILE`) or through an argument: the path, never the values. The file is written by the player, never by an agent, outside the repository and outside `EXP` (for example under `~/.config/obelisk-lab/`), mode 0600.
2. Content (key names only): `EPIC_AUTH_CODE` (single-use authorization code obtained by the player on the Epic sign-in page, which `Client::sign_in` already accepts as input, `EK/client.rs:63-68`), or `EPIC_ACCOUNT_ID` + refresh token for `Client::restore` (`EK/client.rs:70-74`). The installation directory and the engine are non-secret arguments.
3. Checks before reading: the launcher refuses if the file has group or other bits, if it is in a git working copy, or if it does not belong to the current user. Same spirit as the socket folder check (`GR/supervisor/client.rs:453-461`).
4. Flow: reading of the file into memory (zeroized afterwards), `sign_in` or `restore`, then `launch_plan` (`EK/client/launch.rs:150-225`) which produces `LaunchPlan` with `-AUTH_PASSWORD=` and the ownership token; `Engine::target`; writing of the `Admission` on the stdin of the `game-exec` helper through a pipe, never on its command line.
5. Consumption: the file is emptied or renamed as soon as it is read (an authorization code is single-use, an exchange code short-lived). Nothing secret stays at rest.
6. Logs: the launcher never writes `argv`. If it must write it, it replaces the values of `-AUTH_PASSWORD=`, `-epicusername=`, `-epicuserid=` and `-epicovt=`. It enforces a `WINEDEBUG` without `+process`, `+relay`, `+server`, `+all`, and refuses a `winedebug` file that contains them (section 3.3).
7. "Canary" check after the attempt: the launcher searches the secret strings it has in memory in the log folders of the attempt (engine log, copied EAC logs) and reports only a number of occurrences. If the number is non-zero, the logs stay out of the archived evidence.
8. Report: exit code, phase, EAC codes, durations. No identifier or display name.

### 7.4 Common precautions

- Work on an APFS clone of the prefix and use an isolated library (`OBELISK_LIBRARY_DIRECTORY`), so as not to touch `LIB/Prefixes/...` nor the `EpicSessions/` locks of a running Obelisk.
- Never pass `-noeac` nor `-offline` (out of scope); do not modify `Settings.json`, `RocketLeague_EAC.exe` nor `Launcher.exe`; check the hashes before and after (`evidence/game-sha256-start.txt`).
- Exchange code no. 2 is in `argv` of the Wine process during the life of the game: during the attempt, run no diagnostic tool that lists or logs the command lines of processes (`ps`, `lsof -p`, `dtruss` traces, attached crash reports) and treat them as sensitive if they exist.
- The `-epicusername=` and `-epicuserid=` strings identify the player: to be treated as sensitive in the reports.

## 8. Unknowns, risks, contradictions

### 8.1 Contradictions in the repository

- `README.md:74`: "Epic's Windows games remain downloadable, but launching them through Wine is not wired yet". False at HEAD (sections 1 and 2). `rocket-league-macos.md:15` says so too.
- `GR/voile.rs:179-181`: "only Steam launches Windows games". Outdated: Epic too.
- `voile-release.json` pins `v26.3.0-6`, while the library contains `v26.3.0-4` and a `current`. `Voile/current` (without `runtime.json`) takes precedence (`GR/voile.rs:65-73`): the version actually run by the attempts is therefore that of `current`, not identifiable by manifest. `runtime.json` of `v26.3.0-4`: voile `6d2bbb44...`, voile-wine `9861d8b4...`, proton `5b89db94...`, D3DMetal "Evaluation environment for Windows games 4.0 beta 2". HEAD of voile-wine: `e82937c9` (Sept 26), HEAD of voile: `fb27a63`.
- The timeline says "Wine 11.0" for `Voile/current` (`rocket-league-macos.md:11`); I did not recheck it.

### 8.2 Unknowns

- Content of `wine-Sugar.log` (reading refused) and of the EAC logs: not read.
- Whether the Epic catalog of Rocket League carries `OwnershipToken=true` and an `AdditionalCommandLine`: unknown without network. The code handles both cases.
- Actual registry of the prefix: not read.
- How `RocketLeague_EAC.exe` uses the `-AUTH_*` and `-epic*` arguments (it receives the same arguments as `Launcher.exe`, according to the IL): not analyzed. The strings of the binary show `-anticheat_settings=` and `Settings.json`.
- Whether `Launcher.exe` under Mono is enough: it uses `Process.Start`, `Path`, `File`, `Assembly`: nothing exotic, but not run here.
- Supervision behavior when the bootstrapper returns before the game (section 2).
- 16 KiB pages and FEX: outside my task.

### 8.3 Risks for a new path

- Exchange code no. 2 expires quickly: any time spent between `launch_plan` and the exec reduces the margin. In the current flow, `prepare` (prefix, prerequisites) is done BEFORE for this reason (`GR/epic/windows.rs:7-9`); an engine whose preparation would be long must stay before the authentication.
- A single Epic session at a time and a single owner of `EpicSessions/`: a parallel attempt with the real library would be refused (`Busy`) or, worse, would disturb the application.
- `end_orphans` kills through `lsof` any process running a file under `Voile/`: a new runtime outside this folder would not be cleaned up.
- Signature and entitlements of the final binary: the helper enforces native arm64 for itself; the entitlements that a Darwin FEX host (JIT) would need are not studied here.

## Appendix: measurements run (all read-only)

`ls`/`du` of `LIB`, `LIB/Voile`, `LIB/Prefixes`, `LIB/Games/Epic` and of the Rocket League prefix; `cat` of `Voile/v26.3.0-4/runtime.json` and of the `obelisk-*` markers; `file`/`lipo -archs` on `bin/wine`, `bin/wineserver`, `libd3dshared.dylib`, helper; `codesign -d --entitlements` on the helper of the Debug build; `shasum -a 256` on `Launcher.exe`, `RocketLeague.exe`, `RocketLeague_EAC.exe`; reading of the non-secret fields of `.obelisk-install.json` by script (the `account_id` field is not printed); reading of `EasyAntiCheat/Settings.json`; `strings` and extraction of the UTF-16 strings of `Launcher.exe`; IL decoding of `Launcher.exe` with `dnfile` and `dncil` (`pip install` in `EXP/tools/venv`, the only change made under `EXP`; the decoding script and its output stayed in the session scratchpad); `git show 7d4d22f^:docs/...` and `git ls-tree` for the old documents; `grep` of the code of Obelisk, voile and voile-wine. No `git commit`, `push`, `branch` nor `stash`; `git status` of the `obelisk` repository stays `?? docs/`; `voile` and `voile-wine` are clean. No output containing a secret was produced.
