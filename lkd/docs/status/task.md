# Statut du module task (LKD)

Date : 2026-10-02. Jalon 3 atteint : tous les syscalls du périmètre écrits et testés, dans la construction privée (core + task, puis core + task + fs + mm) et dans la construction commune avec tous les modules.

## Ce qui existe (src/task, 4755 lignes avec les tests)

| Fichier | Contenu |
| --- | --- |
| `task_priv.h` | interfaces internes, plafonds de sûreté (300 threads par processus, 64 enfants vivants, 512 processus vivants par famille) |
| `family.c` | registre de famille partagé : fichier temporaire supprimé aussitôt, `MAP_SHARED`, 1024 processus et 8192 threads ; mutex local puis verrou d'octet `fcntl` (`family.c:146`) ; compteur de pid atomique partagé, jamais réutilisé (`family.c:138`) ; export et rattachement par `LKD_FAMILY_FD` ; recherches publiques |
| `tasks.c` | objets tâche et processus, table locale, publication, `lkd_task_create_main` (`tasks.c:551`, reprise d'un enregistrement après ré-exécution hôte), sortie de thread, `exit_group`, `lkd_task_release` (`tasks.c:702`), `de_thread` (`tasks.c:747`), crochets de fork |
| `ids.c` | pid, tid, ppid, pgid, sid, setpgid, setsid, famille uid/gid, groupes, capget/capset, personality |
| `sys.c` | uname, rlimits, getrusage, times, sysinfo, sched_*, priorités, getcpu, getrandom, set_tid_address, listes robustes, exit, exit_group, arch_prctl, prctl, iopl, ioperm, unshare, setns |
| `time.c` | horloges, CPU clocks dynamiques, nanosleep, clock_nanosleep, gettimeofday, time, adjtimex en lecture, itimers, alarm |
| `clone.c` | clone, clone3, fork, vfork, crochet `lkd_task_fork_child` (`clone.c:54`) |
| `wait.c` | mort d'un processus (`wait.c:166`), reparentage, notification du parent, wait4, waitid, enfants hôtes |
| `exec.c` | execve, execveat, `lkd_exec_prepare`, crochet `lkd_task_exec_commit`, `lkd_exec_finish` |

## Tests (MESURE)

Construction commune `./build.sh -k 0` puis `tools/run_tests.sh build/tests` (`test_task_proc` recompté le 2026-10-10, après les orphelins) :

| Programme | Assertions | Cas |
| --- | --- | --- |
| `test_task_ids` | 228 | 16 |
| `test_task_time` | 118 | 10 |
| `test_task_proc` | 260 | 21 |
| `test_task_exec` | 89 | 9 |
| `test_task_bigwin` | 4 | 1 |
| total task | 699 | 57 |

- Les quatre passent dans la construction commune (tous modules liés, dont le vrai module sig) et dans la construction privée core + task (les cas d'exec qui demandent le vfs y sont signalés `SKIP`) ; trois à cinq exécutions consécutives sans échec, aucun processus restant, répertoire temporaire supprimé.
- `test_core_contract` (89) passe contre le module task.
- `test_core_dispatch` passait (155) dans la construction privée core + task ; dans la construction commune il ne se lie plus, à cause de ses propres gestionnaires non statiques en conflit avec fs (demande `task-1.md` point 5).
- Le seul autre échec de la suite commune est `test_fs_pipe` (bits en attente de SIGPIPE et EINTR avec le vrai module sig) : sans rapport avec task.
- Les tests de fork forkent vraiment (au plus trois processus en même temps), chaque enfant a `alarm(20)`. Les tests de threads utilisent le moteur factice.

## Blocage signalé sous le vrai moteur (2026-10-02, après le jalon 3)

`/bin/sh -c '/bin/true; echo status=$?'` restait bloqué dans `wait4` (ou après le `write` final avec `LKD_TRACE=1`). MESURE (`sample`) : aucun réveil perdu dans task ; l'enfant de vfork, passé dans son propre processus hôte à l'exec, tournait dans `lkd_mm_reset_for_exec -> mm_host_cleanup`, qui parcourt toute la fenêtre de 2^46 octets page hôte par page hôte ; le shell faisait ensuite de même dans son `exit_group` (`lkd_mm_put`). Avec la boucle corrigée dans une copie de l'arbre, la commande finit en 1 s avec `status=0` et l'ordre de syscalls attendu. Demande `docs/requests/task-4.md` au module mm. Test ajouté : `tests/test_task_bigwin.c` (fenêtre de production, borne 5 s) : ÉCHEC sur l'arbre actuel (20 114 ms), PASSE avec la correction (524 ms). Il passera quand mm aura corrigé sa boucle.

## Comportements (LU dans Linux v6.12, MESURE par les tests)

1. Identité. Le thread principal a `tid == pid`. Le premier processus a le pid `first_pid` (2 par défaut), jamais 1 sauf configuration, et `ppid` 1 par défaut (`lkd_main_spec.ppid` le change). Aucun processus 1 n'est inventé : `/proc/1` n'existe que si le runtime crée un processus de pid 1. Le premier processus est chef de session et de groupe (pgid = sid = pid). Le premier processus enregistré sans parent dans la famille en est l'init (`fam_child_reaper`, en-tête du registre, version 2) : il tient le rôle du `child_reaper` d'un espace de pid Linux, sans prendre le pid 1 (raisons : `docs/journal/01-integration-status.md`, section des orphelins du 2026-10-10).
2. Registre partagé. Chaque processus de la famille lit le nom, ppid, pgid, sid, état, exe, cmdline (4096 octets au plus), nombre de threads, statut de sortie, limites d'un autre. `wait4` publie l'état `S` du thread qui attend (cas `waiting_parent_shows_sleeping`).
3. clone. Trois formes (`clone.c`, en-tête) : thread (`spawn_thread`), processus partageant la mémoire (vfork, posix_spawn : thread hôte du processus hôte du parent jusqu'à l'exec, le parent attend vraiment, l'écriture de l'enfant en mémoire est vue par le parent, donc le rapport d'erreur de `posix_spawn` de la glibc fonctionne), fork hôte. Les drapeaux suivent `copy_process()` et `clone3_args_valid()` : SETTLS, PARENT_SETTID, CHILD_SETTID (dans la mémoire de l'enfant après fork), CHILD_CLEARTID, PARENT, VFORK, THREAD, SIGHAND, VM, FILES, FS. `CLONE_FILES` ou `CLONE_FS` sans `CLONE_VM` répond EINVAL (impossible entre deux processus hôtes, journalisé). Namespaces : EPERM. `CLONE_PIDFD` : EINVAL (pas de pidfd). `set_tid` de clone3 : EPERM.
4. exec. Résolution par le vfs, droit x selon fsuid/fsgid/groupes, `#!` (argument unique, nom tronqué ENOEXEC, 5 niveaux puis ELOOP comme `exec_binprm`), ELF64 x86-64 `ET_EXEC`/`ET_DYN` sinon ENOEXEC (pas d'émulation i386), argv vide remplacé par `""`, E2BIG pour une chaîne de plus de 128 Kio ou le total au-delà de min(RLIMIT_STACK/4, 6 Mio, 2 Mio), `AT_SYMLINK_NOFOLLOW` sur un lien ELOOP, `execveat` avec `/dev/fd/N[/chemin]`. Point de non-retour : `de_thread` (le thread non chef prend le tid du chef), déplacement d'un enfant de vfork vers un fork hôte, `lkd_exec_commit`, nom = base du nom de fichier, exe = fichier chargé, cmdline, environ, dumpable 0 si le fichier n'est pas lisible, liste robuste et `clear_child_tid` traités comme `exec_mm_release`, setpgid du parent ensuite EACCES. Échec de `exec_image` après ce point : SIGSEGV.
5. Sortie. `exit` code `(c & 0xff) << 8`, `exit_group` tue les autres threads (FATAL + kick), le premier statut de groupe gagne. Par thread : liste robuste (sig), `clear_child_tid` mis à 0 et futex réveillé si la mémoire a d'autres utilisateurs, crochets `task_exit`, signal de mort du parent pour les enfants créés par ce thread. Par processus : zombie publié, enfants reparentés (`forget_original_parent`, `find_new_reaper`) vers le sous-moissonneur vivant le plus proche parmi les ancêtres, sinon vers l'init de la famille, sinon 1 quand l'init lui-même meurt. Chaque enfant reparenté reçoit son signal de mort du parent (SI_USER, pid du parent mort) et prend SIGCHLD comme signal de sortie (`reparent_leader`) ; un zombie reparenté est annoncé au nouveau parent par SIGCHLD, ou libéré si aucun processus vivant ne le prend. Un processus hôte mort sans passer par sa sortie laisse ses enfants orphelins de la même façon (`poll_host_children`, cas `orphans_of_a_killed_host_process`). Puis signal de sortie au parent (SIGCHLD par `lkd_sig_child_event`, autre signal avec siginfo `CLD_*`), libération immédiate si sig le demande.
6. wait4/waitid. WNOHANG, WUNTRACED/WSTOPPED, WCONTINUED, WNOWAIT, WEXITED, `__WALL`, `__WCLONE`, `__WNOTHREAD`, sélection par pid, groupe (0 = le sien) ou tous, ECHILD, EINVAL pour les options inconnues, `INT_MIN` ESRCH, `P_PIDFD` EBADF, siginfo de waitid mis à zéro s'il n'y a rien, rusage des enfants cumulé. Un enfant hôte mort sans passer par sa sortie (SIGKILL hôte, plantage du moteur) est rapporté avec le statut réel de l'hôte (cas `host_killed_child_is_reported`). L'attente est interruptible (ERESTARTSYS) et relit le registre toutes les 10 ms au plus.
7. Temps. REALTIME, REALTIME_COARSE, TAI, REALTIME_ALARM : `CLOCK_REALTIME` de Darwin. MONOTONIC, MONOTONIC_RAW, MONOTONIC_COARSE : `CLOCK_UPTIME_RAW` (s'arrête en veille comme Linux MONOTONIC). BOOTTIME : `CLOCK_MONOTONIC_RAW` de Darwin (compte la veille). CPU clocks `~pid << 3 | which` : processus (soi ou tout processus de la famille par `proc_pidinfo`), thread (soi seulement). Sommeil : partie longue interruptible par le tube de réveil, dernière milliseconde par `nanosleep` hôte (précision mesurée sous la milliseconde) ; relatif : reste écrit et redémarrage par `restart_syscall` à la même échéance ; absolu : ERESTARTNOHAND. itimers REAL, VIRTUAL, PROF par un thread hôte auxiliaire par processus (SIGALRM/SIGVTALRM/SIGPROF avec `SI_KERNEL`), non hérités par fork.
8. Limites et ressources. Défauts Linux (pile 8 Mio, core 0, NOFILE 1024, MEMLOCK 8 Mio, MSGQUEUE 819200, NICE et RTPRIO 0), limite dure NOFILE = `kern.maxfilesperproc` de l'hôte, NPROC = limite hôte. getrusage SELF, THREAD (temps du thread Mach), CHILDREN. sysinfo : mémoire réelle, charge, swap, uptime arrondi au-dessus comme `do_sysinfo`.
9. prctl : SET/GET_NAME, DUMPABLE (0 ou 1), PDEATHSIG, CHILD_SUBREAPER, NO_NEW_PRIVS, TID_ADDRESS, KEEPCAPS, TIMERSLACK, CAPBSET_READ/DROP, CAP_AMBIENT, SECUREBITS, THP_DISABLE, MCE_KILL, TSC, SPECULATION_CTRL (non affecté), SET_VMA (par mm), GET_AUXV (par la disposition rapportée par le moteur), GET/SET_SECCOMP (point 11) ; SYSCALL_USER_DISPATCH, MDWE, PTRACER : EINVAL (journalisés pour les deux premiers) ; options inconnues EINVAL.
10. arch_prctl : SET/GET_FS et GS par `get_regs`/`set_regs` du moteur, EPERM au-delà de `TASK_SIZE_MAX`, `ARCH_CET_STATUS` EINVAL, CPUID faulting ENODEV, LAM et pile fantôme comme un CPU sans ces fonctions.
11. seccomp (`seccomp.c`, `kernel/seccomp.c` v6.12). `seccomp(2)` et `prctl(PR_SET_SECCOMP)` : mode strict (read, write, exit, rt_sigreturn, sinon SIGKILL) et filtres BPF classiques, vérifiés comme `seccomp_check_filter` puis `bpf_check_classic` (codes permis, chargements alignés de `seccomp_data`, sauts dans le programme, retour final, cases mémoire écrites avant d'être lues), EACCES sans `no_new_privs` ni CAP_SYS_ADMIN, ENOMEM au-delà de `MAX_INSNS_PER_PATH`. Chaque syscall passe par la chaîne du thread après l'arrêt ptrace d'entrée (`syscall_trace_enter`), avec le RIP qui suit SYSCALL ; l'action de plus petite valeur gagne. ERRNO, TRAP (SIGSYS forcé, `SYS_SECCOMP`, adresse, numéro, arch, RAX = numéro), LOG, ALLOW, KILL_THREAD et KILL_PROCESS (statut SIGSYS, core comme `core_dumps`). Filtres hérités par clone et fork, TSYNC et TSYNC_ESRCH. Non modélisés : notification utilisateur (`NEW_LISTENER` EINVAL, `USER_NOTIF` sans écouteur : ENOSYS) et `PTRACE_EVENT_SECCOMP` (`TRACE` : ENOSYS, comme sans traceur). Proton installe un tel filtre pour attraper les appels système du code Windows (cas `wine_trap_filter`).

## Syscalls laissés sans enregistrement (ENOSYS compté, choix délibéré)

`rseq` (la glibc s'en passe), `pidfd_open`, `modify_ldt` (pas de LDT dans le moteur), `timer_create`, `timer_settime`, `timer_gettime`, `timer_getoverrun`, `timer_delete` (minuteries POSIX pas encore écrites), `ioprio_get`, `ioprio_set`.

## Réponses ENOSYS ou EPERM visibles à l'intérieur de syscalls enregistrés

- `prlimit64` qui modifie un autre processus, `sched_setscheduler`, `sched_setattr`, `setpriority` sur un thread d'un autre processus hôte : ENOSYS journalisé. `setpriority` par groupe ou utilisateur : ENOSYS.
- `sched_setaffinity` sur un thread d'un autre processus hôte passe par le service inter-processus (`famsvc.h`, opcode `TASK_OP_SET_AFFINITY`, 97) : le processus qui possède le thread pose le masque et le publie. wineserver applique ainsi le `SetThreadAffinityMask` d'un jeu. Avant, ENOSYS faisait échouer cet appel, et Wwise abandonne un fil dont il ne peut poser le masque (ELDEN RING, 2026-10-10). MESURE : `affinity_of_a_thread_in_another_host_process` dans `test_task_proc`.
- `clock_nanosleep` sur une horloge de temps CPU : EOPNOTSUPP journalisé. CPU clock d'un autre thread : EINVAL journalisé.
- `arch_prctl(ARCH_*_XCOMP_*)` : EINVAL journalisé (l'ensemble XSAVE est au moteur).

## Lacunes connues (INFERE sauf mention)

1. L'affinité et le nice sont enregistrés et rapportés, pas appliqués à l'hôte (Darwin n'a pas d'API de placement de thread ni de nice par thread).
2. `pdeathsig` est stocké par processus dans le registre (Linux : par thread de l'enfant). Envoi par `lkd_sig_send_pid`.
3. Un processus partageant la mémoire de son parent (vfork avant exec) meurt avec le processus hôte du parent si celui-ci sort : il est alors rapporté tué par SIGKILL.
4. Un processus hôte de la famille qui meurt brutalement et dont le parent invité n'est pas son parent hôte (cas `CLONE_PARENT`) reste `R` dans le registre : aucun statut réel n'est disponible.
5. `cmdline` des autres processus tronqué à 4096 octets, `exe` à 512.
6. Les CPU clocks d'un processus partageant l'hôte d'un autre comptent tout le processus hôte.
7. L'état publié d'un autre processus hôte n'est `S` que pendant wait4 ; pour les autres attentes il faut la demande `task-1.md` point 4.
8. La sortie du thread principal seul (autres threads vivants) termine aujourd'hui le processus côté moteur (`task-3.md` point 2).
9. `exec_image` du moteur répond ENOSYS : tout exec atteint le point de non-retour puis tue le processus par SIGSEGV (`task-3.md` point 1).
10. `lkd_tid_lookup` publie l'état courant d'un thread local avant de répondre ; `syscall_nr` d'un thread d'un autre processus hôte est celui de la dernière publication.
11. L'init de la famille n'a pas les règles du pid 1 de Linux : `kill(-1)` ne l'épargne pas, il n'a pas la protection `SIGNAL_UNKILLABLE`, et sa mort ne tue pas le reste de la famille (`zap_pid_ns_processes`). C'est le superviseur du moteur (`FEXDARWIN_SUPERVISOR_PID`) qui termine alors les processus hôtes. D'ici là, les orphelins restants ont le parent 1, qui n'existe pas.
12. Le premier processus garde `ppid` 1 alors que `/proc/1` n'existe pas. Linux donnerait 0 pour un parent hors de l'espace de pid (cas d'un processus entré par `setns`). Non changé faute de besoin mesuré.

## Demandes ouvertes

`docs/requests/task-1.md` (core), `task-2.md` (sig, ptrace, procfs, fs), `task-3.md` (moteur), `task-4.md` (mm, remise à zéro de l'espace d'adressage).

## Suite possible

Minuteries POSIX (`timer_*`), opcodes famsvc pour les opérations sur un autre processus hôte, nettoyage d'un zombie dont le processus hôte a disparu sans statut.

## getrusage (2026-10-03, profil du menu du jeu)

`getrusage(RUSAGE_THREAD)` ne fait plus de `getrusage(RUSAGE_SELF)` de l'hôte, qui parcourt tous les threads du processus hôte, seulement pour remplir ru_maxrss.
- ru_maxrss vient de `MACH_TASK_BASIC_INFO.resident_size_max`, relevé au plus toutes les 10 ms.
- Les temps viennent de `thread_info` du thread appelant.

MESURE (`w20_statbench`, un seul thread) : de 0,93 us à 0,47 us par appel. Le gain croît avec le nombre de threads du processus hôte. RUSAGE_SELF reste la somme exacte de l'hôte (0,5 us ici).
