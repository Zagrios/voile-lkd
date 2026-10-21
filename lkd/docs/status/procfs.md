# Statut du module procfs (LKD)

Date : 2026-10-02. Jalon 2 atteint : `/proc`, `/sys` et `/dev` sont servis par le montage virtuel du vfs, les trois programmes de test passent (551 assertions), les autres processus de la famille se lisent par famsvc. procfs n'enregistre aucun syscall et ne tient aucun verrou.

## Ce qui existe (src/procfs, 3060 lignes)

| Fichier | Contenu |
| --- | --- |
| `lkd_procfs.h` | API pour le moteur et les tests : `lkd_procfs_set_cpuid`, `lkd_procfs_set_backend`, `lkd_procfs_render_wire`, `lkd_procfs_dev_is_zero`, `lkd_procfs_ncpu` |
| `procfs_priv.h` | types internes (nœuds, cibles, requêtes inter-processus) |
| `node.c` | résolution des trois arbres (`pf_resolve`), attributs, listings, readlink ; propriétaire des nœuds de `/proc/<pid>` (`task_dump_owner`) |
| `file.c` | `open` des fichiers générés (sémantique seq_file : contenu produit à la première lecture et à toute lecture à l'offset 0), objet fichier, droits |
| `pid.c` | tous les fichiers de `/proc/<pid>` et `task/<tid>`, règles d'accès, objet `mem` |
| `global.c` | cpuinfo, meminfo, uptime, loadavg, version, stat, filesystems, `/proc/sys` |
| `sysfs.c` | `/sys/devices/system/cpu` (online, possible, present, offline, kernel_max, cpuN/online, topology, cache), `clocksource0` |
| `dev.c` | `/dev` : null, zero, full, random, urandom, tty, console, ptmx, pts, fd, stdin, stdout, stderr |
| `remote.c` | opcode famsvc 49 (`LKD_FAMSVC_OP_TASK_STATUS`), backend par défaut, objet sur descripteur reçu |
| `init.c` | montages `/proc` (proc), `/sys` (sysfs), `/dev` (devtmpfs) |
| `buf.c`, `NOTICE` | tampon texte, attributions |

## Modèle

- Un fichier de `/proc/<pid>` est rendu par le processus hôte qui porte la tâche, avec ses propres structures (task, mm, sig, fs, ptrace). Pour un autre processus hôte, le lecteur envoie une requête famsvc (`struct lkd_procfs_wire`) avec le verdict d'accès `ptrace_may_access` ; l'autre processus répond avec le texte. Un zombie est rendu depuis le registre seul. `/proc/<pid>/maps` d'un autre processus passe par le service de mm (`lkd_mm_remote_render_maps`), `mem` par `lkd_mm_remote_read/write`.
- `open("/proc/<autre>/fd/N")` : l'autre processus rouvre la description (`lkd_fdobj_reopen`) et renvoie un descripteur hôte par SCM_RIGHTS ; le lecteur l'enveloppe dans un objet à offset privé (`pf_dupfile_new`), l'offset de l'autre processus n'est jamais déplacé. Tubes et sockets : `-ENXIO` (règle de fs).
- Aucun fichier n'est un instantané figé : chaque lecture à l'offset 0 régénère le contenu à partir de l'état courant.
- Aucun contenu n'est inventé. Une quantité que l'hôte ne donne pas est omise, ou vaut 0 là où le concept n'existe pas sous Darwin (liste dans « Lacunes »).

## Tests (MESURE, build privé, `tools/runtest.sh`)

| Programme | Assertions | Cas | Contenu |
| --- | --- | --- | --- |
| `test_procfs_self` | 271 | 17 | self et thread-self, exe, cmdline/environ/auxv (lectures partielles), comm (lecture, écriture, échappement), status (tous les champs, valeurs fixées par le test), compteurs Vm* (mmap, mlock), stat (52 champs, valeurs de la disposition), statm, maps/smaps, fd (memfd, tube, listing getdents64, réouverture indépendante, fdinfo), cwd/root, mem, limits, syscall/wchan/cgroup/personality/mounts/net, propriétaires et accès d'un autre lecteur (dumpable, uid), comparaisons aux échantillons Linux |
| `test_procfs_other` | 104 | 4 | registre factice (ordre de listing, zombie, verdicts ptrace, requête envoyée à l'autre processus) ; vrais processus : fork, lecture par famsvc de status, stat, cmdline, exe, cwd, root, limits, maps, fd/fdinfo, descripteur mémoire reçu, `mem` en lecture et écriture, TracerPid réel (PTRACE_TRACEME), zombie avant wait4, disparition après |
| `test_procfs_global` | 176 | 6 | cpuinfo (fournisseur CPUID factice, chaque ligne), meminfo/uptime/loadavg mesurés contre sysctl, version/stat/filesystems, `/proc/sys`, arbre `/sys` CPU et cache, /dev |

Dans la construction complète (`cmake --build -- -k 0` puis `tools/run_tests.sh`), les trois passent avec les autres modules ; trois exécutions consécutives sans échec, aucun processus ni répertoire famille laissé. Échecs d'autres modules vus dans la même construction : `test_net_host`, `test_ptrace_requests`, `test_sig_futex` (délai), `test_ptrace_smoke` (ne compile pas : `body_loop` défini deux fois), sans lien avec procfs.

## Comparaison aux échantillons Linux (explications des écarts)

- stat : `rocket-hyperlinux-20260928/linux-reference/loader-trace/ge-eac-120s-threads.txt:17`. Même nombre de champs (52), mêmes champs fixés à 0 (itrealvalue, kstkesp, kstkeip, nswap, cnswap). Écarts : `flags` (4194560 chez Linux : PF_RANDOMIZE et autres ; LKD met seulement PF_EXITING, pas d'ASLR), `tty_nr` 0 et `tty_pgrp` -1 (aucun terminal de contrôle suivi), `processor` (voir lacunes), `start_data`... viennent de la disposition rapportée par le moteur.
- maps : `qemu-neptune-graphics-20260930/game-query-fix-20260930T211549Z/outputs/game-process-maps.txt`. Le nom commence au décalage 73 (748 lignes mesurées sur 748), ligne anonyme finie par un espace, `/memfd:nom (deleted)`, `[heap]`, `[vdso]`. Le test applique le même vérificateur de colonnes aux lignes enregistrées et aux lignes de LKD. Écart : LKD fusionne comme Linux les VMA anonymes voisines (mm).
- status : `linux-reference/pid1-control/pid1-before.txt` (noyau 7.0, x86-64). Mêmes clés dans le même ordre, `Seccomp` et `Seccomp_filters` compris (mode et nombre de filtres du thread, `seccomp.c` du module task). Largeur de 8 colonnes des champs Vm*, 16 chiffres hexadécimaux des ensembles de signaux : identiques. Le test vérifie ces trois points.

## Source de chaque champ de `/proc/cpuinfo`

| Champ | Source |
| --- | --- |
| nombre de blocs | `sysctl hw.logicalcpu` (18 sur ce Mac, MESURE) |
| vendor_id, cpu family, model, stepping, fpu, cpuid level | CPUID 0 et 1 du moteur (FEX : `CPUID.cpp:429-448`) |
| model name | CPUID 0x80000002 à 4 par CPU (FEX : MIDR du cœur, `CPUID.cpp:1121`) |
| cpu MHz, bogomips | CPUID 0x15 : ecx*ebx/eax Hz ; absent si eax vaut 0 (FEX divise par zéro dans ce cas) |
| cache size | plus grand cache de CPUID 4, en Ko |
| flags | bits de CPUID 1, 6, 7, 0xD, 0x10, 0x80000001, 0x80000007, 0x80000008, 0x8000000A, 0x8000001F, même table que FEX ; toujours présents : `art rep_good nopl xtopology cpuid tsc_known_freq` ; `hypervisor` suit le bit CPUID 1 ecx 31 (affiché, pas caché) |
| physical id, siblings, core id, cpu cores, apicid | topologie plate : un paquet, N cœurs, un thread chacun (comme FEX) ; identique à `/sys/devices/system/cpu/cpuN/topology` |
| clflush size, cache_alignment | CPUID 1 ebx[15:8] * 8 |
| address sizes | CPUID 0x80000008 eax |
| bugs, power management | vides |
| microcode | non imprimé (valeur inconnue ; FEX imprime 0x0) |

FEX est un émulateur et cela se voit : le nom de modèle est celui que le moteur donne, et `hypervisor` apparaît si le CPUID le fixe.

## `/proc/<pid>/lkd_host_pid` (2026-10-10)

Fichier propre à LKD, absent de Linux : le PID macOS du processus hôte qui porte le processus, suivi d'un saut de ligne (`72451\n`), mode 0444. Le même fichier existe sous `task/<tid>/` et donc sous `/proc/thread-self/` : tous les threads d'un processus partagent son processus hôte.
- Source : `lkd_proc_info.host_pid` du registre de la famille, sans requête famsvc. Juste après un fork, après un exec (même processus hôte) et après la migration d'un enfant de vfork à son exec (nouveau processus hôte), le registre a déjà la bonne valeur.
- Un zombie n'a plus de processus hôte : `-ESRCH`.
- Le fichier n'apparaît pas dans le listage de `/proc/<pid>` : il s'ouvre par son nom seulement, pour que le listage reste celui du noyau modélisé (EAC lit ces répertoires).
- Usage : `guest/steam-hostpid.so` (racine du dépôt). Le `steamclient.so` de Valve annonce `getpid()` au moteur Steam macOS d'Obelisk, qui coupe le tuyau si ce PID n'est pas un processus macOS vivant.
- Tests : `test_procfs_other` cas `host_pid_file` (registre factice, soi, fork réel vu des deux côtés, zombie, thread, absence du listage) ; témoin invité `tests/glibc-x86_64/hostpid.sh` (w25).

## Parent d'un autre processus : `PPid` lu dans le registre (2026-10-10)

- `status` (`PPid`) et `stat` (champs 4, 5, 6 et 38) d'un processus sont rendus par son propre processus hôte. Le rendu prenait sa copie locale de ppid, pgid, sid et du signal de sortie, que le processus ne relit dans le registre que lorsqu'il appelle lui-même `getppid`, `getpgrp`, etc.
- Effet mesuré : un orphelin qui n'avait pas fait cet appel montrait encore son parent mort. Sonde ppid du rapport ELDEN RING 07 : `ping.exe` avec `PPid 52`, le processus intermédiaire du double fork de Wine, sorti depuis longtemps.
- `snap_get` relit maintenant ces champs dans le registre (`lkd_proc_refresh_ids`) avant chaque rendu. Un zombie était déjà rendu depuis le registre.
- Test : `test_procfs_other` cas `orphan_ppid_follows_its_reaper`. ÉCHEC sans la relecture (`PPid 4`, le processus intermédiaire mort), PASSE avec.

## Fichiers volontairement absents (ENOENT)

`/sys/devices/virtual/dmi/id/*`, `/sys/class`, `/sys/bus/pci`, `/proc/bus/pci/devices` (sondes EAC, absence acceptée), `/proc/<pid>/io` (pas de comptage d'E/S par tâche), `map_files`, `ns`, `sched`, `stack`, `pagemap`, `oom_*`, `attr`, `loginuid`, `uid_map`, `/proc/sys/kernel/yama`, `/proc/net/tcp` et voisins, `/proc/stat` lignes `intr`, `ctxt`, `softirq`, `processes`. `/proc/<pid>/net/dev` ne porte que l'en-tête (aucune interface traduite). Si `inotify_init1` n'a pas de propriétaire, `/proc/sys/fs/inotify` n'existe pas.

## Pseudo-terminaux

SCM_RIGHTS (2026-10-10). null, zero, random et urandom partent vers un autre processus hôte par leur jumeau macOS (même comportement), ouvert au premier envoi avec le mode d'accès invité, et un jumeau reçu sans chemin invité redevient le périphérique Linux (`lkd_procfs_dev_adopt`, aussi pour l'entrée standard du runtime). Lectures et écritures restent dans LKD : macOS refuse `write` sur `/dev/urandom` (EPERM) et ne sait pas attendre un périphérique (`poll` POLLNVAL, `kevent` EINVAL). `/dev/full`, qui n'a pas de jumeau, et un périphérique ouvert O_PATH partent en enregistrement (`LKD_FDREC_DEV`). Un fichier généré et `/proc/<pid>/mem` partent en enregistrement `LKD_FDREC_PROCFS` (chemin canonique, position) : le récepteur le rouvre avec ses propres droits (`pf_open`) et reprend à la position. Voir `net.md`.

Non pris en charge. `/dev/ptmx` existe avec les numéros Linux (5:2, 0666) mais `open` répond `-ENODEV` (noyau sans pilote devpts) ; `/dev/pts` est un répertoire vide. `/dev/tty` et `/dev/console` ouvrent le terminal de contrôle de l'hôte (`/dev/tty`) et répondent `-ENXIO` sans terminal ; `/dev/console` n'est donc pas la console système.

## Lacunes connues (honnêtes)

1. `VmPeak` et `VmHWM` valent la valeur courante (mm ne garde pas de crête) : borne inférieure. `procfs-2.md` point 2.
2. `THP_enabled` fixé à 1 (INFERE). `VmPTE` 0 (aucune table de pages invitée).
3. `cmdline` et `environ` sont les copies prises à l'exec : une réécriture d'argv par le programme (setproctitle) n'apparaît pas. Linux relit `arg_start..arg_end`.
4. `stat` : utime et stime d'un thread autre que l'appelant valent 0 (vue par thread) ; la vue du processus entier utilise `getrusage(RUSAGE_SELF)` du processus hôte, donc un processus co-hébergé (vfork avant exec) voit les temps de l'hôte. `processor` : CPU du thread appelant, 0 pour un autre thread. `flags` : voir plus haut. `minflt` et `majflt` sont ceux du processus hôte.
5. `uptime`, `btime` : horloge CLOCK_BOOTTIME du module task (CLOCK_MONOTONIC_RAW de Darwin) ; `btime` = maintenant - uptime. `kern.boottime` diffère de 8 s sur ce Mac (MESURE : 32695,43 contre 32687,00 s) et n'est pas utilisé, pour que `btime`, `/proc/uptime` et `clock_gettime(CLOCK_BOOTTIME)` s'accordent.
6. `meminfo` : `Buffers`, `Dirty`, `Writeback`, `Slab`, `SReclaimable`, `SUnreclaim`, `KernelStack`, `PageTables`, `Shmem`, `Committed_AS`, `SwapCached` valent 0 (aucun équivalent Darwin ou aucun comptage). `MemAvailable` = libre + inactif + spéculatif + purgeable, borné à `MemTotal`. `Cached` = pages adossées à un fichier, `AnonPages` = pages internes. `CommitLimit` = moitié de la mémoire + swap (`overcommit_memory` vaut 1 : mm ne compte rien).
7. `/proc/stat` : ticks par CPU de `host_processor_info` (100 Hz) ; `iowait`, `irq`, `softirq`, `steal`, `guest` valent 0 car ils n'existent pas sous Darwin.
8. `/proc/sys` : écriture refusée (`-EACCES` pour un non-root, `-EPERM` journalisé pour root) ; les paramètres sont fixes. `pid_max` = `LKD_PID_MAX + 1` (plus grand que le plafond de Linux, c'est la limite réelle du compteur de famille). `randomize_va_space` 0 (pas d'ASLR). `overcommit_memory` 1. Limites inotify 128 / 65536 / 16384 (non fournies par net, `procfs-4.md` point 3).
9. Un autre processus hôte dont le service famsvc ne répond pas dans le délai (5 s) donne `-EIO` ; un service disparu `-ESRCH` (la lecture d'un processus mort rend `-ESRCH`, comme Linux, et le répertoire disparaît à `wait4`).
10. L'état `S` d'un autre processus hôte vient du registre (task : `R` sauf pendant wait4, nanosleep) ; `State:` lu dans le processus lui-même est exact pour ses propres threads. Voir `task-1.md` point 4.
11. Les listings de `/proc` limitent le registre à 1024 processus (taille du registre de task) ; chaque entrée relit la liste (O(n) par entrée, non mesuré).
12. `execve("/proc/self/exe")` (le lien lui-même, pas son texte) n'est pas vérifié avec le moteur (`procfs-3.md` point 3).
13. `/proc/<pid>/fd/N` : le mode du lien suit le mode d'accès de la description ; pour un autre processus hôte, il vient de `fdinfo` du processus cible.
14. Mémoire d'un processus mort : `mem` répond 0 octet (`-ESRCH` du service est transformé en fin de fichier), comme `mm_access` renvoyant NULL.

## Performance

Non mesurée (décision : pas de benchmark avant le premier lancement réel). Le service famsvc coûte p50 10,3 us par aller-retour (MESURE de sig) ; une lecture de status d'un autre processus fait un aller-retour.

## Demandes écrites

`docs/requests/procfs-1.md` (core : chemin d'inclusion, `lkd_task_uname`), `procfs-2.md` (mm : /dev/zero, crêtes), `procfs-3.md` (moteur : fournisseur CPUID, disposition après exec), `procfs-4.md` (fs, net, ptrace, task).

## Suite possible

`/proc/<pid>/io` à partir de `proc_pid_rusage` (octets lus et écrits du disque), `/proc/net/tcp`/`udp`/`unix` avec le module net, `smaps_rollup`, `/proc/<pid>/children`, lecture de argv vivant, mesure du coût de listing de `/proc`.
