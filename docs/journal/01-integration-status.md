# État d'intégration moteur et noyau, 2 octobre 2026

Document court, mis à jour à chaque jalon d'intégration. Détails du moteur dans `fexcore-darwin-port.md`, du noyau dans `src/lkd/docs/status/`.

## Moteur (FEXCore sur Darwin, `src/fexdarwin`)

MESURE. Code x86-64 exécuté par le JIT FEXCore dans un processus macOS arm64 natif, sans Rosetta ni virtualisation. Témoins possédés, tous rejoués avec une base invitée de 8 Tio : entiers 9524/9524, SIMD et x87 109/109 identiques à l'oracle Rosetta, TLS 25/25, mémoire 39/39, code automodifiable 20/20, syscalls 50/50, mémoire sous 4 Gio 19/19 (dont 0x7ffe0000), threads 13/13, sauts vers adresse non mappée 15/15, récursion profonde 4/4, débordement de pile récupéré 7/7. Performance sur noyaux C : au niveau de Rosetta (0 à 23 % plus lent), base invitée au plus 4 %.

## Noyau (LKD, `src/lkd`)

Coeur : dispatch, mémoire invitée protégée contre les fautes, traductions d'ABI, 599 assertions. Modules mm et fs en cours (agents), task, sig, procfs, net et ptrace à lancer.

## Raccordement

`src/fexdarwin/src/LkdGlue.cpp` implémente `lkd_engine_ops` sur FEXCore : registres au format `user_regs_struct`, état FP FXSAVE et XSAVE, interruption par signal hôte réservé, création de threads, invalidation de code, cadres `rt_sigframe`, verrous de fork. `LkdLoader.cpp` charge l'exécutable et son PT_INTERP par des appels système LKD (openat, pread64, mmap, mprotect), construit la pile initiale et l'auxv avec AT_PAGESZ = 4096. Mode activé par `FEXDARWIN_LKD=1`, racine invitée `FEXDARWIN_ROOT`.

MESURE (14:58) : `lkd_task_create_main` crée la tâche 2 ; le premier appel `mmap` du chargeur répond ENOSYS, attendu tant que le module mm n'est pas livré.

Ajouts de 15:20 (compilés, témoins hors LKD rejoués sans régression ; pas encore exercés en mode LKD faute des modules mm, task et sig) :

- Interruption d'un thread qui tourne dans le code traduit. Le signal hôte réservé (SIGUSR2) arrive dans le gestionnaire ; si le PC est dans le code du JIT, hors section à signaux différés et hors séquence d'appel système, les registres invités sont déversés (même méthode que FEX sur Linux pour les signaux asynchrones) et le thread repart sur un trampoline qui appelle `lkd_signal_next` sur la pile ordinaire du thread (il peut donc s'y bloquer pour un arrêt ptrace), puis rentre dans la boucle du répartiteur. Dans les autres cas (répartiteur, C++ hôte), un thread de relance renvoie le signal toutes les millisecondes tant que la demande tient et que le thread n'est pas dans LKD. Témoin `w15_async` écrit : tgkill vers un thread qui boucle sans appel système, kill vers le processus pendant que le thread principal boucle, contrôle des registres entiers et SSE après cinq interruptions.
- Fautes synchrones en mode LKD : `lkd_fault` puis le même trampoline, au lieu de l'ancienne table de signaux interne.
- `exec_image` : chargement en place après le point de non-retour (code traduit invalidé sur toute la fenêtre, registres d'un processus neuf, EFLAGS 0x202, FPU remise à zéro, AT_EXECFN = chemin passé à execve). Le chargeur passe par `lkd_dispatch` (appel interne au noyau) et non `lkd_syscall` : un traceur en PTRACE_SYSCALL ne doit voir que l'entrée et la sortie d'execve, pas les mmap du chargeur.
- (16:10) Les deux restes de cette liste sont faits : classement MAPERR/ACCERR et `[stack]` (par `start_stack` du module mm), voir plus bas.

## Constat qui a orienté le travail

Annoncer des pages de 16 Kio à glibc fait échouer ld.so (« ELF load command address/offset not page-aligned » sur libc.so.6, mesuré avec l'ancien hôte). La vue logique de 4 Kio du module mm est donc obligatoire, comme prévu par la fiche de décision.


## Premier passage de bout en bout sur LKD (16:00 à 16:15)

MESURE, hôte `fexdarwin-run` lié à un instantané privé de `liblkd.a` (modules core, mm, fs, task livrés ; sig en cours), journaux sous `evidence/fexcore-darwin/runs-lkd/` :

- Neuf témoins sur treize passent en mode LKD : `w01_hello`, `w03_simd`, `w04_tls`, `w05_mem` (39/39), `w06_smc` (20/20, mode SMC complet), `w08_sys`, `w09_lowmem`, `w10_threads`, `w13_deeprec`. Les quatre qui échouent (`w02_int`, `w11_badjump`, `w14_overflow`, `w15_async`) passent tous par la livraison de signaux : `lkd_signal_next` rend -1 malgré un gestionnaire installé (module sig en cours, reproduction transmise à son agent). Côté moteur, l'interruption fonctionne : le thread qui boucle sans appel système est arrêté à `rip 0x2aaaaaaaf7ea` et atteint le point sûr.
- Programmes glibc Debian 2.41, dynamiques, par `ld.so`, racine Debian montée en lecture seule : `/usr/bin/true`, `/bin/ls /`, `/bin/uname -a` (`Linux obelisk 6.12.0-lkd ... x86_64`), `dash -c` avec substitution de commande (fork réel). `vfork` puis `execve` puis `exit_group` du fils, SIGCHLD livré au gestionnaire de dash (cadre construit, `rt_sigreturn`), `wait4` rend le fils et `status=0` s'affiche ; ensuite le processus ne sort pas (attente perdue dans `wait4` entre deux processus hôtes, reproduction transmise à l'agent task).
- Wine GE-Proton11-7 : `/opt/ge-proton/files/bin/wine --version` affiche `wine-11.0 (Staging)` et sort avec 0. Les appels `socket(AF_UNIX)` répondent ENOSYS (module net en cours) ; c'est la condition suivante pour wineserver et wineboot.

Corrections faites pour y arriver :
- Moteur : base de fenêtre de 8 Tio passée à LKD (sinon mode identité des tests) ; FEXCore créé après la réservation de la fenêtre, parce que le répartiteur grave la base invitée comme constante ; mode SMC complet par défaut en LKD et CRC du mode complet calculé sur la copie hôte (`Core.cpp`) ; requêtes de plage exécutable et classement MAPERR/ACCERR lus dans la table des VMA de LKD (`LkdShim.c`, seul fichier qui inclut des en-têtes internes de LKD) ; stdio invité ; début du tas et de la pile communiqués à mm ; pile initiale dans l'ordre de Linux et disposition du processus pour `/proc` et `PR_GET_AUXV` ; adresses de chargement à la manière de Linux (`ELF_ET_DYN_BASE` = 2/3 de la fenêtre, interpréteur par mmap descendant) ; `fork_child`, fin du seul thread principal, SIGPIPE ignoré ; montages `FEXDARWIN_BIND` et racine en lecture seule `FEXDARWIN_ROOT_RO=1`.
- Témoin `w05_mem` : l'assertion d'alignement de 16 Kio venait de l'ancien hôte ; Linux garantit 4 Kio, l'assertion suit Linux.

## Signaux, SMC rapide et témoins complets sur LKD (19:00 à 19:40)

MESURE, journaux `evidence/fexcore-darwin/runs-lkd/` :

- Les treize témoins du lot passent en mode LKD (`w01` à `w06`, `w08` à `w11`, `w13` à `w15`) avec les binaires ET_EXEC de `witnesses/elf-x86_64/bin-exec/` (`EXEC=1 ./build.sh`). Les échecs de `w02`, `w11`, `w14`, `w15` venaient de nos binaires : des PIE statiques dont personne n'appliquait les relocations RELATIVE (le gestionnaire de signal lu dans la structure valait 0, soit SIG_DFL). Un noyau Linux ne les applique pas non plus ; l'ancien hôte le faisait à sa place. Trouvé par l'agent sig (`docs/status/sig.md`, `docs/requests/sig-4.md`).
- Le chargeur oubliait la page d'un segment PT_LOAD sans données de fichier dont le bss tient dans une page (`filesz == 0`) ; Linux la mappe (`elf_load` appelle `vm_brk_flags` depuis le début de page). Corrigé dans `LkdLoader.cpp`.
- `w07_fault` imprime `PASS 99/99` puis `ret` de `run` saute à une adresse de pile (stack corrompue après le dernier test de débordement) ; même symptôme que sur l'ancien hôte, donc pas propre à LKD. Non élucidé ; `w14` couvre le même scénario et passe. À reprendre si un programme réel montre le même défaut.
- Interruption asynchrone exacte. `w15_async` échouait 3 fois sur 8 (invariants de registres) : FEX reconstruit RIP depuis une table qui ne contient que les instructions à effets de bord et au mieux « as close as FEX can get » (`Core.cpp`, `RestoreRIPFromHostPC`) ; une interruption au milieu de la traduction d'une instruction x86 la rejoue après le gestionnaire. Corrigé dans FEXCore : une entrée de table pour chaque instruction invitée, et `IsHostPCAtGuestInstructionBoundary` (PC hôte exactement au premier octet de la traduction d'une instruction) ; le gestionnaire du signal de coup de pied n'interrompt que dans ce cas, sinon le fil de relance renvoie le signal une milliseconde plus tard. Résultat : 30 exécutions de `w15_async` sur 30 réussies (16 contrôles), latence maximale de livraison 16 ms. Enjeu réel : Wine suspend et lit le contexte des threads avec SIGUSR1 ; une reprise inexacte corromprait le jeu.
- Détection du code automodifiable rapide (`mtrack`) rétablie : le module mm retire l'écriture des pages hôtes qui contiennent du code traduit (`lkd_mm_watch_code`), la première écriture revient par le gestionnaire (`lkd_mm_code_write_fault`, qui appelle `invalidate_code`), y compris pour les copies du noyau (patch `usermem.c`, demande mm-5). `w06_smc` 20/20 dans ce mode. Détail à ne pas filtrer : macOS rapporte l'écriture sur une page en lecture seule non résidente comme faute de traduction (DFSC 0x06), pas de permission.
- Performance (`w12_bench` sous LKD, deux exécutions) : entiers 393/403 ms, mémoire 38/37, flottants 371/369, tri 183/185 ; Rosetta 379/30/351/141. La table de RIP complète et le mode LKD coûtent environ 4 % sur les entiers.
- Module sig terminé (703 assertions), mm terminé (685 assertions + `test_task_bigwin`), task (616), fs (747). net, procfs et ptrace en cours de reprise après deux coupures par la limite de session de l'API.

## Incident machine du 2026-10-02, 20:21 : panique noyau, cause et garde-fous

MESURE (`/Library/Logs/DiagnosticReports/panic-full-2026-10-02-202119.0002.panic`) : « zone map exhausted while allocating from zone [VM map entries] ... (10G, 177893865 elements allocated) », 332 processus `fexdarwin-run` vivants. Le dernier essai de Wine (`logs/wine-linux-m2-version-013.log`, Wine construit depuis voile-wine, de 20:18:46 à 20:20:23) a lancé des processus Windows en boucle (identifiants 0x2c à 0x3e4, « failed to create desktop window »), chacun étant un processus hôte ; des exécutions dont l'enveloppe de délai avait été tuée laissaient leurs processus vivants. MESURE après redémarrage : un processus invité ordinaire a environ 400 régions VM (dash 400, client Wine 380 à 410, wineserver 460).

Garde-fous ajoutés (`src/fexdarwin/src/LkdGlue.cpp`, `scripts/run-with-timeout.sh`, `src/lkd/src/task/task_priv.h`) :
- chaque processus hôte suit `FEXDARWIN_SUPERVISOR_PID` (l'enveloppe, ou à défaut le premier processus de la famille) et s'arrête en 0,5 s quand il disparaît ; MESURE : `kill -9` de l'enveloppe, les 5 processus invités disparaissent en moins de 2 s ;
- chaque processus s'arrête de lui-même au-delà de 200 000 régions VM ;
- au plus 96 processus vivants par famille (512 avant) ;
- règles de travail : jamais d'invité hors `run-with-timeout.sh`, au plus deux agents qui lancent des invités en même temps, comptage des `fexdarwin-run` restants après chaque essai.

### Cause réelle (20:40) et campagne de sondes EAC

MESURE (`build-lead/chunk.c`) : XNU découpe un mapping anonyme dont la protection maximale n'est pas nulle en entrées de 128 Mio (`ANON_CHUNK_SIZE`). La fenêtre invitée de 2^46 octets réservée par `mmap(PROT_NONE, MAP_NORESERVE)` coûtait 524 288 entrées de la table du noyau par processus ; `vmmap` les fusionne à l'affichage (320 régions affichées). 332 processus × 524 288 = 174 millions, ce que la panique rapporte (177 millions). La même fenêtre réservée par `mach_vm_map` avec protection maximale nulle est une seule entrée. Correctif dans `src/lkd/src/mm/window.c` (réservation et remise à vide en protection maximale nulle, recréation en mémoire anonyme avant tout accès, région par région) : 10 programmes de test mm et `test_task_bigwin` passent ; un processus invité compte maintenant 120 à 160 entrées (compteur `FEXDARWIN_REGION_LOG=1`). Le garde-fou de 200 000 régions reste en place ; il avait arrêté proprement, avant correction, la sonde qui l'atteignait.

Sondes EAC (`witnesses/eac-probes/run-probes.py`, 39 sondes x86-64 de la campagne HyperLinux, valeurs attendues du vrai Linux, `evidence/eac-probes/20261002T204027/`) : 1 592 contrôles sur 1 824 passent. Passent entièrement : cycle de vie de `/proc/<pid>`, jeux de registres généraux et FP, contrat siginfo, arrêts sur signaux et double INT3, tgkill entre processus et entre threads, état brk. Échecs groupés : POKEDATA dans du code en lecture seule (EIO au lieu de l'écriture forcée de Linux) et code réécrit non réexécuté ; arrêt d'événement d'exec (GETEVENTMSG, GETSIGINFO) ; quelques sondes qui sortent avant leur premier contrôle. Transmis à l'agent d'intégration.

### Sondes EAC après intégration (21:00)

MESURE (`evidence/eac-probes/20261002T205719/`, 40 sondes, 4 376 contrôles, les sondes vont maintenant jusqu'au bout) : 38 échecs (232 sur 1 824 avant). Corrections : POKE forcé dans du code en lecture seule partageant sa page hôte avec des données (`src/mm/remote.c`) ; lien traceur réécrit à tort après l'exec d'un fils de fork (`src/ptrace/life.c`, GETEVENTMSG et GETSIGINFO en ESRCH sur 14 sondes) ; invalidation du code traduit demandée par le fil de service de mm (moteur : `InvalidateCode` sans thread invité). Les 38 restants : 31 contrôles de sondes qui font un `MAP_FIXED` à 0x7ffff7ff0000 ou 0x7ffffdffb000, au-delà de la fenêtre invitée de 2^46 octets (limite d'architecture : avec une base décalée, l'espace utilisateur macOS s'arrête à 0x7ffffe000000 et la zone de nano malloc occupe 0x600000000000, donc l'espace Linux complet de 128 Tio n'est pas atteignable ; Wine et EAC ne demandent pas ces adresses fixes, les adresses qu'EAC lit viennent de `/proc/<pid>/maps`) ; 6 contrôles où `CONT(SIGTRAP)` à un arrêt de livraison de SIGTRAP sans gestionnaire tue le traceé avec le statut 5, comportement de Linux selon notre lecture (valeur attendue sans oracle Linux dans le répertoire de la sonde, ancienne exécution HyperLinux en échec aussi).

## EAC 301 sur la pile fexdarwin + LKD + Wine Linux (22:12)

MESURE (`evidence/eac-runs/20261002T221224/`, lancement authentifié par `run/eac-run.py`, journal EAC expurgé `files/.../anticheatlauncher.log`) : le bootstrapper EAC 1.9.3 choisit `linux64`, télécharge le module (13 825 695 octets, HTTP 200 depuis modules-cdn.eac-prod.on.epicgames.com), écrit « Starting Wine module mapping, Wine version: 11.0 » puis, 2,4 s plus tard, « Launcher finished with: 301, 'Easy Anti-Cheat successfully loaded in-game' », « Successfully initialized the Easy Anti-Cheat module, waiting for game window to become visible », « Terminating with: 0 ». `RocketLeague.exe` est chargé dans un nouveau processus. Le traceur montre la séquence connue (retour vers l'adresse 9 dans le JIT, « NoExec instruction in entry block: 9 »). Empreintes du jeu d'origine inchangées. Référence : PC Linux réel 301 en 0,5 s, QEMU TCG 301 en 3,5 s ; toutes les piles ARM précédentes 509/210 ou 502.

Ce qui a débloqué : (1) le bootstrapper demande dix allocations RWX à adresse fixe de 0x7001'0000'0000 à 0x700a'0000'0000 et affichait « Failed to initialize Wine helper buffer » avec une fenêtre invitée de 2^46 (trouvé par l'agent Wine avec `+virtual`) ; la fenêtre invitée va maintenant de la base hôte choisie au démarrage (fin de la plus haute région occupée au-dessus de 0x7d80000000, MESURE : une réservation de 12,8 Gio placée par l'ASLR atteint parfois 0x7cb9400000) jusqu'à 0x7ffffe000000, soit environ 127,5 Tio d'adresses invitées, une seule entrée noyau ; pile en haut de fenêtre, exécutables PIE à 0x555555554000 comme Linux ; 16 témoins passent, 20 démarrages sur 20 sans échec de réservation. (2) le pilote d'affichage présente la boîte de dialogue d'erreur (elle restait invisible) ; (3) la persistance du registre du préfixe (attente de wineserver).

Étape suivante : le processus du jeu (fenêtre, D3D11 par Neptune vers Metal). Défauts vus dans ce même essai : `MAP_SHARED` refusé à 0x6fffffd17000 (adresse et décalage non congruents modulo la page hôte de 16 Kio), allocations de Wine au sommet de l'espace (0x7fffffb50000 et au-dessus, hors fenêtre), AF_NETLINK absent, option IPv6 62 non gérée.

## Jeu au menu principal, connecté à Psyonix avec EAC actif (22:54)

MESURE (`evidence/eac-runs/20261002T225400/`, `files/.../Launch.log` et `anticheatlauncher.log` expurgés), après le correctif wine-7 (phase des memfd et vues de code partagées) : EAC 301 ; `RocketLeague.exe` charge `EOSSDK-Win64-Shipping.dll`, nos `dxgi.dll`/`d3d11.dll` Neptune, `xaudio2_9redist.dll` ; « Found D3D11 adapter 0: Apple M5 Max », « Shader platform (RHI): PC-D3D-SM5 », 466 923 shaders chargés ; requêtes HTTPS Psyonix 200 ; connexion EOS par code d'échange, « Platform auth ticket completed with success 'True' », « PsyNet auth RPC succeeded », « Login successful », WebSocket PsyNet connecté, carte `MENU_Main_p` chargée et mise en jeu ; le rendu Neptune crée une chaîne d'échange de 3 images 2560x1440 présentée par wsd en plein écran. Défaut restant : aucune image n'atteint l'écran (wsd `presented=0`). Durées : menu à 76 s, connexion Psyonix à 100 s (chargement du cache de shaders 30 s).

## Premier affichage du jeu par Metal (23:07)

MESURE (`evidence/eac-runs/20261002T230701/`, capture de la seule fenêtre du jeu `evidence/wine-linux/game-keep/rl-title-20261002T230701.png`) : l'écran titre « ROCKET LEAGUE SEASON 24 » est rendu par le client Neptune D3D11, le serveur de rendu DXMT sur Metal, et présenté par wsd dans une fenêtre 1280x720 (mode fenêtré réglé dans la configuration utilisateur du préfixe, `TASystemSettings.ini`, jamais dans l'installation). 96 présentations, 95 affichées ; attente de fence moyenne 2,3 ms. Cadence pendant le chargement : 148 ms par image en moyenne sur les 60 premières (environ 7 images par seconde). Défauts ouverts : les présentations s'arrêtent après 96 images (le journal du jeu s'arrête à 113,9 s) ; le serveur de rendu ignore des appels VSSetShader/PSSetShader (« argument handle is unknown ») et signale des objets non enregistrés de types 61 et 65.

## Menu principal stable à 60 images par seconde (2026-10-03, 00:30)

MESURE (`evidence/eac-runs/20261002T233036/`, capture de la fenêtre du jeu `evidence/wine-linux/game/m2-win1084.png`, qui montre le nom d'affichage Epic du joueur) : menu principal atteint et stable, 1 680 présentations en 150 s. Le blocage après 96 images venait de Wine : `gethostbyname` passe par nsiproxy, qui lisait la liste des interfaces par `if_nameindex()` ; sans AF_NETLINK dans LKD elle rendait NULL, nsiproxy le déréférençait en tenant son verrou, et tous les appels suivants restaient bloqués. Corrigé dans Wine (repli `SIOCGIFCONF`, `GetAdaptersAddresses` sans interface rend ERROR_NO_DATA comme Windows) ; demande LKD wine-8 pour des interfaces réelles. Temps d'image au menu : 16,1 à 16,4 ms (affichage à 60 Hz), latence de soumission à l'affichage 12 à 16 ms. Profil processeur du jeu au menu, attentes exclues : environ 93 % dans des appels système LKD (`getrusage` en boucle 1,4 s sur 5 s ; `mincore` pour VmRSS de `/proc/<pid>/status` 0,4 s), 5 % code JIT, 1,5 % C++ de FEX et LKD. Restent : 14 paires de shaders jamais créées côté hôte (28 commandes rejetées sur des milliers, menu visuellement correct), entrées pour jouer, essai de jeu hors ligne, puis partie publique.

## Jeu en 3D hors ligne et sortie propre (2026-10-03, 01:01)

MESURE (`evidence/eac-runs/20261003T005434/`, capture `evidence/wine-linux/play/d1-win1362.png`) : navigation écran titre, menu, Training, Free Play par entrées injectées (wsdctl), carte `CHN_Stadium_Day_P` en Free Play (hors ligne, sans autre joueur), conduite 70 s. Temps d'image en 3D : 23,0 ms en moyenne sur 2 160 images (environ 43 i/s), fence 2,4 ms ; wsd 22,5 ms en moyenne, maximum 129 ms, latence d'affichage 13,7 ms ; menu 16,1 ms. Processeur pendant la conduite : processus du jeu, part active, JIT 43 %, appels système 42 % (sockets), C++ hôte 15 % ; serveur de rendu 86 % d'un cœur, surtout des copies mémoire (memmove). Shaders : 0 identifiant inconnu au menu et en 3D. Sortie par les menus du jeu : code 0 en 441,9 s, « Exit: Exiting. », EAC « Launcher finished with: 301 » puis « Terminating with: 0 », aucun processus restant, empreintes inchangées. Défauts avant une partie publique : environ 1 s entre une entrée et son effet ; texte de l'interface (Scaleform) dessiné en partie ; cadence 3D limitée par les copies du serveur de rendu.

## Régression « bloqué sur l'écran d'initialisation » corrigée (2026-10-03, 02:35)

Symptôme (MESURE, essais `20261003T014348` à `022545`, 8 essais sur 8) : 0 à 3 images présentées puis plus rien ; le jeu continue son chargement jusqu'à ~80 s puis tout s'arrête. Le dernier essai bon était `013654`.

Cause (MESURE) :
- Vidage des fils invités, sans rien changer pour l'invité : nouvel outil `FEXDARWIN_DUMP_DIR` dans `LkdGlue.cpp`. Il donne par fil l'appel système en cours, ses arguments, le mot futex et le propriétaire du mutex glibc, ainsi que les adresses de retour avec module et décalage.
- Résultat : le fil de présentation (`npt_unix_wsi_wsd.c:277`) attend `g_lock` alors qu'il en est lui-même propriétaire (`__owner` = son tid). Le drain (ligne 322) et le fil d'événements du pilote (`on_presented`) attendent derrière lui.
- Aucune source ne prend ce verrou deux fois. Le déverrouillage a été perdu.
- La recompilation de nptunix à 01:43 a déplacé `.plt` (0x4020–0x41d0) dans la même page hôte de 16 Kio que `.got.plt`, `.data` et `.bss` (où se trouve `g_lock` à 0x65a0).
- Chaque appel par la PLT retraduit ce code et reprotège la page (SMC mtrack). Chaque écriture dans `g_lock` passe donc par `lkd_mm_code_write_fault`.
- Sous contention, la borne `MAX_RETRIES` (4 réponses « réessayer » consécutives, état « surveillé » lu seulement avant le traitement) refusait des écritures légitimes. L'invité recevait SIGSEGV.
- Wine abandonne alors l'appel unix en cours et revient côté PE avec le mutex tenu. La présentation suivante se bloque.

Correctif (LKD, `src/lkd/src/mm/watch.c`) :
- `mm_unwatch_for_write` distingue « page trouvée surveillée ou protection changée » (2) de « déjà inscriptible, rien fait » (1).
- Seul le cas 1 compte vers la borne, maintenant 1000.
- Une écriture Linux dans une VMA inscriptible ne doit jamais échouer à cause de la surveillance du code.

Témoins (`witnesses/glibc-x86_64/`) :
- w23_codedatapage reproduit le défaut (SIGSEGV, rc 139 avant ; PASS après) ;
- w22_mutexsig (mutex sous signaux) passe ;
- w24 (`cond_timedwait`) passe ;
- non-régression : w06, w10, w15, w17, w18, w19 et w21 passent.

Vérification (MESURE, `evidence/eac-runs/20261003T023459/`) :
- EAC initialisé ;
- menu atteint, musique lancée à 77 s ;
- 3 540 images présentées en 150 s, environ 16 ms par image au menu ;
- empreintes du jeu inchangées.

Le vidage de copie de texture mappée (texte Scaleform) est réactivé par défaut (`NPT_FLUSH_MAPPED_COPY=0` le coupe). Il avait été accusé à tort.

À faire (performance) : la page mixte PLT+données provoque une tempête d'invalidations SMC dans toute bibliothèque dont les données partagent une page hôte avec du code. Pistes : surveillance plus fine, ou édition des liens de nos .so à 16 Kio (`-z max-page-size=16384`).

## Texte Scaleform qui disparaît : corrigé (2026-10-03, 02:56)

Cause (MESURE, trace client `NPT_TRACE_STAGING`, essais `20261003T024536` et `025108`) :
- Le cache de glyphes de Rocket League copie son atlas (texture GPU 1024x1024 R8) dans une texture STAGING (accès CPU en écriture seule), puis la mappe en `D3D11_MAP_WRITE`.
- Il ajoute ensuite les nouveaux glyphes et envoie toute l'image mappée à l'atlas par `UpdateSubresource`.
- Le renderer Neptune (`src/graphics-spike/virgl/src/neptune/npt_resource.c`, map synchrone) ne recopiait le contenu dans l'emplacement partagé que pour un accès READ.
- Le jeu voyait donc une image vide hors des glyphes du moment : chaque envoi effaçait les anciens. Mesure : octets non nuls envoyés 17 617, 24 879, 7 313, 946… au lieu d'une croissance.
- Le correctif client de 01:40 (copie depuis une texture mappée) visait une séquence que le jeu n'emploie pas : la trace ne l'a jamais vue.

Correctif :
- Pour tout Map D3D11 qui n'est pas DISCARD (WRITE, READ_WRITE, NO_OVERWRITE), le renderer recopie le contenu courant dans l'emplacement, comme le prévoit la sémantique D3D11.
- Le chemin D3D12 est inchangé.

Vérification (MESURE, essai `20261003T025436`, capture `evidence/wine-linux/play/t2-menu-win1846.png`) :
- l'atlas croît sans jamais redescendre (17 617 → 117 736 octets non nuls) ;
- tous les textes du menu sont affichés : boutons, défis, nom du joueur, rang.

## Session joueur du 2026-10-03 (02:57–03:57) : affichage perdu en entrant en Free Play

MESURE (`evidence/eac-runs/20261003T025713/`) :
- Lancement authentifié, EAC actif, menu avec texte complet. Free Play chargé à 151 s.
- Juste après, le pilote reçoit `WSD_RELATIVE_MOUSE` (souris capturée, dernière ligne de `wsd.log` : « relative mouse on »).
- Puis les deux connexions du jeu à wsd sont perdues (« lost the connection to the window server wsd »). wsd disparaît sans message de sortie, sans rapport de plantage et sans « signal 14 » : c'est la signature d'un arrêt forcé ou d'un Quitter via le Dock.
- Le jeu continue sans affichage jusqu'à la limite d'une heure (journal jusqu'à 3 542 s, rc 142). Empreintes inchangées.

Cause probable (LU, `src/wsd/wsd.m`) :
- En mode relatif, wsd appelle `CGAssociateMouseAndMouseCursorPosition(false)`, qui agit sur tout le système.
- La perte du focus (Cmd+Tab, notification) ne rendait pas le curseur : il restait figé partout.
- Arrêter wsd était alors la seule issue.
- Reproduction automatique (60 000 mouvements relatifs à 1000 Hz en Free Play, `run/mouse-stress.sh`, essai `20261003T035930`) : wsd tient. Mais l'écran était en veille (fenêtre occultée) et le scénario de focus n'a pas pu être rejoué.

Correctifs (wsd) :
- perte de focus : curseur rendu et mode relatif suspendu ;
- retour du focus : mode relatif repris ;
- un Quitter (Dock ou menu) est maintenant journalisé ;
- `wsdctl move COUNT HZ` (entrée de test) injecte des mouvements relatifs.

Non validé à l'écran : à vérifier avec l'utilisateur.

## Clavier et son (2026-10-03, 10:35)

Clavier :
- Cause : les fenêtres de jeu sont créées sans bordure, car Wine dessine lui-même le cadre. Une `NSWindow` sans bordure refuse le statut de fenêtre clé, donc aucun `keyDown` n'arrivait (`key=0` dans tous les états de fenêtre). Les touches injectées marchaient parce qu'elles étaient envoyées directement à la fenêtre.
- Correctif : sous-classe `WsdNSWindow` (`canBecomeKeyWindow`/`canBecomeMainWindow` → YES) dans `src/wsd/wsd.m`.
- Mesure (`evidence/eac-runs/20261003T103502/wsd.log`) : fenêtre du jeu `key=1 app_active=1`.

Son :
- Cause : Wine construit sans pilote audio Linux, et aucun serveur de son côté hôte (« No driver from pulse,alsa,oss,coreaudio »).
- Correctif : nouveau pilote Wine `wineobeliskaudio.drv`, dérivé de wineoss pour la logique de temps.
  - Les trames vont dans un anneau partagé (memfd, `struct wsd_audio_ring`), passé à wsd par SCM_RIGHTS.
  - wsd les joue par une AudioUnit CoreAudio par flux, dont le rappel lit l'anneau directement : `WSD_AUDIO_OPEN/START/STOP/RESET/CLOSE` dans `wsd_protocol.h`.
  - L'ouverture est synchrone (PING/PONG) : un refus de wsd fait échouer le flux proprement.
  - `obeliskaudio` est en tête de la liste par défaut de mmdevapi. Pas de MIDI ni d'aux. Pas de capture pour l'instant.
- Mesures :
  - client natif : 48 000 trames lues en 1,12 s ;
  - WASAPI partagé par événements (`src/wine-linux/tests/wasapitone.c`) : position 2,000 s après 2,154 s, en float 48 kHz stéréo ;
  - le jeu ouvre son flux (format 4 = float, 48 kHz, 2 canaux).
- Limites : le volume par flux de Wine n'est pas appliqué (seul le silence total l'est, comme wineoss) ; pas de micro.

## Chargements de cartes : de 26–38 s à 2–5 s (2026-10-03, 14:57)

MESURE (profil `sample` de chaque processus invité, déclenché par chaque `LoadMap:` de `Launch.log`, lancements par Obelisk) :
- Avant : Free Play 37,8 s, retour au menu 18,4 s, démarrage jusqu'au menu 80,6 s. Le fil principal du jeu passait 69 % du chargement bloqué dans `read` sur un tube (attentes Wine), 10 % dans `getdents64`, 10 % dans le code JIT. Le serveur de rendu et wineserver étaient presque inactifs.
- `getdents` : `snap_add` comparait chaque nom à tous les précédents. `CookedPCConsole` contient 20 746 fichiers, soit environ 215 millions de `strcmp` et 357 ms par listage. Wine liste le dossier à chaque recherche insensible à la casse qui échoue. Les noms d'un `readdir` hôte sont uniques : seuls les points de montage et les dossiers virtuels gardent la vérification. Listage : 6,4 ms (test `getdents_large_directory`).
- LKD était compilé en Debug (sans optimisation) par défaut, et c'est ce binaire qui partait dans l'archive `lkd`. `lkd/build.sh` compile désormais en Release. Toute la suite passe en Release (le test de borne de `mm/watch.c` attendait encore l'ancienne borne de 4).
- Après les deux correctifs : Free Play 4,6 s, menu 1,6 à 2,3 s, démarrage 28,3 s, arène en ligne 3,6 s. Le match en ligne a tenu jusqu'à la sortie du joueur, sans « Error: 42 ». Pendant le chargement Free Play, aucun processus n'est saturé (wineserver à environ 6 %).

## Pics de lag et image qui revient en arrière (2026-10-03, 15:55)

Symptômes rapportés par le joueur : de gros pics de lag en jeu, le menu qui reste lent après un match, et l'image entière qui revient en arrière le temps d'une image.

MESURE, en trois temps :
- Les profils `sample` de l'après-midi faussaient les sessions. Ils suspendent chaque thread environ mille fois par seconde, et Free Play tombait de 60 à environ 25 i/s tant qu'ils tournaient. Sans profil, Free Play tient 60 i/s. D'où le journal interne `FEXDARWIN_HITCH_LOG` (temps de traduction FEX par thread, fautes SMC et invalidations, fenêtres de 50 ms, pages fautives toutes les 10 s) et le fichier `fexdarwin.env`, qui l'active sans toucher au lanceur d'Obelisk.
- Tempête SMC : environ 19 000 fautes d'écriture par seconde dans le processus du jeu pendant toute la partie, 0,9 s de gestion par tranche de 10 s, presque toutes sur `nptunix.so+0x3000`. La fin de son code et ses données (`.got`, `.data`, `.bss`) partageaient une page hôte de 16 Kio. En reliant les 27 `.so` Unix de voile-wine avec `-z max-page-size=16384` (Voile, `build-linux.sh`), on tombe à 4 fautes par 10 s, et les pics ont disparu du ressenti du joueur.
- Image en arrière : wsd fait copier par son GPU 64 pixels du tampon qu'il dessine. L'image N est parfois dessinée avec les pixels de N−3, qui sont le contenu précédent du même tampon de la chaîne d'échange. Relu par le CPU 2 à 8 ms plus tard, le tampon contient la bonne image : la copie arrive, mais après le signal « GPU terminé ». Correction annexe dans vtest (neptune) : un retrait de fence sur un anneau d'événements Neptune emportait les soumissions précédentes du même anneau. C'était un vrai défaut (signal prématuré, puis accès après libération au retrait suivant), mais ce n'était pas la cause : après correction, il reste environ une image en arrière par minute. Piste suivante : ordonner la présentation sur la fin réelle du tampon de commandes DXMT (requête d'événement D3D11 côté serveur de rendu) plutôt que sur l'événement partagé.


## PID hôte pour Steam : `lkd_host_pid` et `steam-hostpid.so` (2026-10-10, 08:20)

Contexte : chemin ELDEN RING (Steam) sur le moteur Linux. Le `steamclient.so` Linux de Valve, dans l'invité, parle en TCP au moteur Steam arm64 d'Obelisk et joint `getpid()` à chaque message IPC. Le moteur coupe le tuyau (RST) 2 à 3 s après la connexion si ce PID n'est pas un processus macOS vivant. Les PID invités de LKD (2, 3...) n'en sont pas.

- LKD : `/proc/<pid>/lkd_host_pid` (et `task/<tid>/`, `/proc/thread-self/`) donne le PID macOS du processus hôte, lu dans le registre de la famille. Il reste juste après un fork, un exec et la migration d'un enfant de vfork. Il n'apparaît pas dans le listage de `/proc/<pid>`. Détails : `lkd/docs/status/procfs.md`.
- `guest/steam-hostpid.so` (bibliothèque x86-64, zig, glibc 2.36) : `getpid()` rend ce PID hôte seulement quand l'appelant est dans `steamclient.so` (adresse de retour, `_dl_find_object`). Pour tout autre appelant, il rend le PID invité.
  - Pourquoi filtrer par appelant : ntdll envoie `getpid()` à wineserver (`unix_pid` de `init_first_thread`), qui signale les threads par `tgkill(unix_pid, unix_tid)` et lit leur mémoire par `process_vm_readv` (`server/ptrace.c`). Un PID hôte y donnerait ESRCH. Limiter le préchargement à l'arbre du jeu ne suffit pas : le processus du jeu est lui-même un processus Wine.
  - Les 22 appels de `getpid` dans `steamclient.so` (build 1788652215) sont des `call` par la PLT, sans saut terminal : l'adresse de retour est bien dans le `.so`.
  - Le résultat est mis en cache, avec le PID invité comme clé : un fork (PID invité jamais réutilisé) relit le fichier.
- MESURE, témoin `tests/glibc-x86_64/hostpid.sh` (w25) : thread principal, thread, fork, exec, `posix_spawn`, puis la bibliothèque appelée depuis un faux `steamclient.so`, depuis un thread et après un fork. Chaque PID annoncé est un `fexdarwin-run` vivant, avec le bon parent macOS (`ps`). PASS.
- MESURE, sonde Steam en direct (session Obelisk d'ELDEN RING, port `Steam3Master` seul lu) :
  - avec la bibliothèque : session complète (tuyau, utilisateur, licence, ticket chiffré `eResult=1`), 25 s stables, sortie 0 ;
  - sans la bibliothèque (témoin) : RST environ 2 s après la connexion, assertion fatale, sortie 1.
- Pour Voile et Obelisk : livrer `guest/bin/steam-hostpid.so` dans le runtime Linux, et le précharger (`LD_PRELOAD`) dans l'invité du lanceur. Rapport : `sailover/build/experiments/eldenring-linux-20261010/reports/05-lkd-pid-hote.md`.

## Orphelins : un parent vivant et lisible (2026-10-10, 10:50)

Contexte : ELDEN RING sur le moteur Linux. Son bootstrapper EAC, lancé par `steam.exe`, finissait toujours en `208, 'Launch Error: 256'` (rapport sailover 07, § 4). EAC lit `getppid()` puis `/proc/<ppid>/{comm,exe,cmdline,status}` (digest EAC, H8). Or Wine crée chaque processus par un double fork (`ntdll/unix/process.c`, `spawn_process`) : le processus intermédiaire sort aussitôt et le processus créé devient orphelin.

Ce que faisait LKD (MESURE, sonde `ppid-probe.sh` : `ping.exe` lancé par `cmd.exe`) :
- sans sous-moissonneur, l'orphelin partait au pid 1. Aucune famille LKD n'a de pid 1 (les pids commencent à 2) : `getppid()` rendait 1, et `/proc/1` n'existait pas ;
- `/proc/<pid>/status` faisait pire. Il montrait le processus intermédiaire mort (`ping.exe` : `PPid 51` ; `winedevice.exe`, `plugplay.exe`, `svchost.exe` : 12, 22, 28 ou 34), parce que procfs rendait la copie locale du ppid, que le processus ne relit que s'il appelle lui-même `getppid` ;
- un parent dont le processus hôte meurt sans passer par sa sortie (SIGKILL hôte) gardait ses enfants : leur ppid restait sur lui.

Correctif dans lkd (`task/wait.c`, `task/family.c`, `task/tasks.c`, `procfs/pid.c`), sur le modèle de `kernel/exit.c` v6.12 :
- l'init de la famille est son premier processus enregistré sans parent dans la famille, celui que le moteur lance (chez Obelisk, le `sh` de `launch.sh`). Le registre garde son pid dans son en-tête (version 2). Comme le `child_reaper` d'un espace de pid Linux, il reçoit les orphelins qui n'ont aucun sous-moissonneur vivant parmi leurs ancêtres ;
- `PR_SET_CHILD_SUBREAPER` et `PR_GET_CHILD_SUBREAPER` existaient déjà. Le sous-moissonneur garde la priorité, et l'indicateur ne passe pas aux enfants ;
- l'orphelin prend SIGCHLD comme signal de sortie (`reparent_leader`). Un zombie reparenté est annoncé au nouveau parent, qui peut le récolter par `wait4` ;
- le même reparentage a lieu quand le processus hôte d'un parent meurt sans sa sortie. Le signal de mort du parent part alors avec SI_USER et le pid du parent mort, comme `SEND_SIG_NOINFO` ;
- `/proc/<pid>/status` et `stat` relisent ppid, pgid, sid et le signal de sortie dans le registre avant le rendu.

Le premier processus ne devient pas le pid 1. Raisons :
- EAC exige un parent vivant et lisible, pas le pid 1. Sur le PC Bazzite, le jeu a pour parent `pv-adverb`, un sous-moissonneur ; sous HyperLinux, un `dash` de pid 1 passait aussi le contrôle du parent (digest EAC, § 1.3 et H8) ;
- Wine en pid 1 (PPid 0) donne précisément 256 sur un vrai noyau (`pid1-control`). Avec le premier processus numéroté 1, tout lancement où Wine est ce premier processus y serait exposé ;
- un vrai pid 1 a aussi la protection `SIGNAL_UNKILLABLE`, l'exclusion de `kill(-1)`, et sa mort arrête tout l'espace de pid. LKD n'a aucune de ces règles : numéroter le premier processus 1 sans elles inventerait un init à moitié ;
- le bootstrapper de Rocket League a déjà ce `sh` (pid 2) pour parent et passe en 301. Rien ne change pour lui ;
- LKD ne donne pas le pid 1 par choix documenté (`first_pid` vaut 2 par défaut, statut task, point 1). `first_pid = 1` reste possible par configuration, et l'init de la famille est alors le pid 1.

MESURE :
- suite LKD : 42 programmes passent ; `test_sig_futex` dépasse son délai comme sur l'arbre intact, au même cas (`waitv`). `test_task_proc` compte 21 cas (260 assertions) et `test_procfs_other` 6 cas. Les cinq nouveaux cas échouent sans le correctif ;
- témoin invité `w26_reaper` : 8 échecs avec le `fexdarwin-run` du runtime -13-dev, PASS avec le nouveau ;
- sonde ppid : avant, `ping.exe ppid=51 parent_comm=UNREADABLE` ; après, `ppid=2 parent_comm=sh`, comme tous les processus de Wine ;
- ELDEN RING, bootstrapper derrière `steam.exe` (`er-direct.sh` en mode B, runtime et préfixe copiés, `/opt/game` en lecture seule, binds Steam d'Obelisk) : avant, `208, 'Launch Error: 256'` 0,9 s après le début du mapping ; après, le mapping dure 89 s et finit en `210, 'Unexpected error. (#1)'`. C'est le verdict du lancement direct (mode A) du rapport 07, un autre chantier ;
- Rocket League (`rl-direct.sh`) : `301, 'Easy Anti-Cheat successfully loaded in-game'` 0,39 s après le début du mapping.

Reste ouvert : l'init de la famille n'a pas les règles propres au pid 1 (voir plus haut), et le premier processus garde `ppid` 1 sans `/proc/1`. Linux donnerait 0 pour un parent hors de l'espace de pid. Rapport : `sailover/build/experiments/eldenring-linux-20261010/reports/08-lkd-orphelins.md`.

## ELDEN RING : le 210 d'EAC vient du nom du préchargeur de Wine (2026-10-10, 11:20)

Contexte : le bootstrapper EAC d'ELDEN RING (EOS, 1.9.4) finissait en `210, 'Unexpected error. (#1)'` après 81 à 89 s de mapping, lancé par le `sh` du lanceur ou, depuis le correctif des orphelins, derrière `steam.exe`. Avec le même moteur, le même préfixe et le même lanceur, Rocket League passe en 301 en 0,4 s.

MESURE, traces LKD complètes des deux bootstrappers (instrumentation locale, non commitée) :
- Les deux modules de lancement ne sont pas de la même génération. ELDEN RING : 9 168 824 octets, GCC 7.5, un seul PT_LOAD RWX dont le code se réécrit. Rocket League : 13 825 695 octets, GCC 11.4, segments séparés, rien en RWX. Le module de RL d'avant le 29 septembre (9,7 Mo) était probablement de la première sorte.
- Jusqu'au fork, les deux modules font les mêmes appels, avec les mêmes réponses : `/proc/<pid>/maps`, `dlopen` par `/proc/self/fd/N`, `/proc/<pid>/status`, `/proc/<ppid>/{comm,cmdline,status}`, `/proc/mounts`, DMI et PCI (ENOENT), `readlink(/proc/self/exe)` puis le `realpath` de sa cible, `runtime.conf` (ENOENT). Aucun ENOSYS, EINVAL, EPERM ou ESRCH propre à ELDEN RING, aucun `process_vm_readv`.
- Les 81 s sont du calcul dans le fil du module : un ou deux appels système par tranche de 10 s, environ 33 000 fautes d'écriture SMC par seconde, autant d'invalidations et de blocs retraduits. Le code tourne dans la page qu'il écrit (`rbp` à module+0x5d2d3, `rip` entre module+0x5d546 et +0x5e96b).
- Ensuite viennent le fork et la relation ptrace connue (TRACEME, SIGSTOP, SETOPTIONS 0x11, SYSCALL, GETREGSET), trois `clock_gettime` suivis par le traceur, le memfd du second ELF (6 130 496 octets, en-têtes cohérents), les seals, la lecture de la première et de la dernière page de `eldenring.exe`. Puis `readlink("/proc/self/exe")` rend `/opt/wine/lib/wine/x86_64-unix/wine-preloader` et l'enfant fait `exit_group(1)` sans autre appel. Le traceur lit le statut 0x100 et le bootstrapper écrit 210 (#1).
- Avec la même réponse de `readlink`, l'enfant de Rocket League fait `newfstatat(.../wine64)` (ENOENT), puis `execve` de `[wine-preloader, wine, Z:\...\RocketLeague.exe]`.

Contre-épreuves, sur une copie privée de l'arbre Wine (aucun fichier d'EAC ou du jeu touché) :
- `x86_64-unix/wine64-preloader` en vrai fichier, `wine-preloader` en lien vers lui, `wine64` en lien vers `wine` : l'enfant exécute `[wine64-preloader, wine64, Z:\opt\game\Game\eldenring.exe]`, injecte son module, et le bootstrapper écrit `301, 'Easy Anti-Cheat successfully loaded in-game'`. Lancé par le `sh` comme derrière `steam.exe` (binds Steam d'Obelisk). Rocket League reste à 301 (0,38 s) avec cet arbre ;
- même 301 avec le préchargeur placé en `bin/wine64-preloader`.

Le module d'ancienne génération veut donc un préchargeur 64 bits nommé `wine64-preloader`, le nom de Wine avant le nouveau WoW64. GE-Proton 11 livre encore `wine64` et `wine64-preloader` à côté de `wine` et `wine-preloader`. Le Wine 11 de Voile n'a que `wine-preloader`. INFÉRÉ : un vrai Linux avec ce même arbre Wine donnerait le même 210, puisque les appels et leurs réponses ne diffèrent pas. Rien à corriger dans LKD pour ce verdict : la correction est dans l'arbre Wine du runtime Linux de Voile.

Reste la lenteur. Une fois l'enfant accepté, le mapping dure 234 à 240 s : le traceur exécute le même code auto-modifiant entre ses arrêts ptrace. `--smc=full` ramène le chemin du 210 à 1,5 s, mais casse l'injection : le bloc de l'adresse de retour 9 est retraduit en boucle (`NoExec instruction in entry block: 9`, plus d'un million de fois) et le traceur ne voit jamais le SIGSEGV. La voie propre est dans FEXCore, une validation par bloc (le `ForceFullSMCDetection` existe, réservé aux hacks mono) pour les pages qui prennent des fautes d'écriture en série. Pas fait ici : FEX n'accepte pas de contribution générée.

EFLAGS (corrigé dans fexdarwin) : `ReconstructCompactedEFLAGS` de FEXCore ajoute aussi les octets de sa copie NZCV (`flags[24..27]`) aux bits 24 à 31. MESURE : le bit 31 apparaissait dans l'eflags de ptrace (et dans le r11 qu'en tire LKD aux arrêts de syscall), et dans le `uc_mcontext` des signaux, que Wine recopie dans le CONTEXT Windows. `GuestEflags` (`include/Host.h`) ne garde que les bits définis de RFLAGS, bit 1 à 1, pour `GetRegs` et les cadres de signal. Témoin `w27_eflags` : 11 échecs sur 12 contextes avec le moteur du runtime -13-dev, PASS après. w22, w23, w24 et w26 passent. Sondes EAC : 4 376 contrôles, les mêmes 38 échecs avant et après. Les verdicts d'EAC ne changent pas (301 et 210 comme avant). Rapport : `sailover/build/experiments/eldenring-linux-20261010/reports/09-eac-210-eldenring.md`.

## Code réécrit sans arrêt : la page quitte le suivi en écriture, le mapping d'EAC d'ELDEN RING passe de 233 s à 2,8 s (2026-10-10, 12:40)

Contexte : avec le préchargeur renommé (section précédente), le module de lancement d'EAC d'ELDEN RING rend 301, mais son mapping dure 233 à 240 s. Rocket League met 0,4 s sur la même pile.

MESURE, profil du moteur de `c545aa6` (`FEXDARWIN_PROFILE=1`, `FEXDARWIN_HITCH_LOG=1`) :
- Toutes les fautes tombent dans une seule page hôte de 16 Kio, `memfd+0x5c000` du module, dans son PT_LOAD RWX. C'est la page de la boucle qui fait les écritures.
- 352 112 fautes d'écriture SMC en 11,26 s, autant d'invalidations, autant de blocs retraduits. Chaque écriture coûte une faute, l'invalidation de la page et une nouvelle traduction, environ 30 µs en tout.
- L'enfant forké reprend la même page au même rythme.
- Ce sont surtout des données que le module écrit. Une fois la page passée en vérification, le processus du bootstrapper ne compte plus que 130 fautes SMC et 256 invalidations sur toute sa vie.

Correctif, dans FEX et fexdarwin (LKD n'est pas modifié) :
- Zagrios/FEX `3d0290743`. Une instruction sans octets décodés (lecture dans une page non exécutable, opcode invalide) n'est plus vérifiée. `ValidateCode` sur zéro octet comparait le CRC attendu à TMP1, que rien n'avait calculé. Le bloc se retirait et se retraduisait en boucle, d'où les « NoExec instruction in entry block: 9 » de `--smc=full` et le SIGSEGV que le traceur d'EAC n'a jamais vu. `w11_badjump` en `--smc=full` : 1 290 237 retraductions et 5,45 s avant (la boucle finit par hasard ici), 2 retraductions et 0,24 s après.
- Zagrios/FEX `d08fd1541`. `SyscallHandler::NeedsCodeValidation(Start, Length)` : en mode mtrack, les instructions d'un bloc qui tombent dans une page pour laquelle le frontend répond vrai reçoivent la vérification d'octets du mode full. FEXCore pose la question une fois par page de code du bloc, mutex d'invalidation tenu.
- fexdarwin, `CodeValidation` (`3932460`). Une page hôte qui prend 128 fautes d'écriture SMC en 100 ms quitte le suivi en écriture jusqu'au prochain exec. Le gestionnaire de faute la marque, puis l'invalide lui-même : l'invalidation de LKD peut manquer si un autre fil a retiré la surveillance entre-temps, et une traduction commencée avant la marque peut finir après sans surveiller la page. `MarkGuestExecutableRange` ne demande plus à LKD de surveiller une page marquée. Les autres pages restent en suivi.

MESURE, runtime et préfixe privés, arbre Wine renommé du rapport 09 :

| Essai | Avant | Après |
|---|---|---|
| ELDEN RING, lancé par `sh` | 301 en 232,6 s (240,2 s avec profil) | 301 en 2,74 à 2,80 s, 6 essais dont 2 avec profil |
| ELDEN RING derrière `steam.exe` | 301 en 235,1 s (rapport 09) | 301 en 2,74 et 2,75 s |
| ELDEN RING, arbre Wine d'origine | 210 après 85 s | 210 après 0,18 s |
| Rocket League | 301 en 0,40 et 0,43 s | 301 en 0,38 à 0,42 s, 6 essais |

- Aucune page de Rocket League ne quitte le suivi : 10 fautes SMC au plus par processus et par tranche de 10 s, loin du seuil. D3D11 par DXMT et D3D12 par D3DMetal : sortie 0, relectures MATCH, médianes égales au bruit près.
- Une page vérifiée coûte. Une boucle ALU serrée y tourne 1,7 fois plus lentement (378 ms contre 225 ms), et la page le reste jusqu'à l'exec même quand les écritures cessent. Je n'ai pas ajouté de retour en suivi : il faudrait invalider la page pour retrouver des blocs sans vérification, et aucun cas mesuré ne le demande.

Témoins :
- `w28_smcstorm` (nouveau, autonome) : une tempête de 20 000 écritures, puis réécritures entre appels, écriture dans le bloc en cours, boucle chaude qui corrige sa fonction, instruction à cheval sur une page suivie et une page vérifiée, code écrit par le noyau (`read` d'un tube), appel dans la page rendue non exécutable (SIGSEGV à la bonne adresse). 26/26 en mtrack et en full, 15/26 en `--smc=none`. La tempête prend 0 à 1 ms au lieu de 130 ms.
- `w29_smcthreads` (nouveau, glibc) : un fil corrige le code, un autre l'exécute, un troisième écrit des données dans la même page. W29 PASS 5 fois sur 5 après. Avant, 2 390 à 6 153 tours sur 20 000 rendent l'ancienne valeur. Cause, déjà là avant ce travail : le suivi en écriture ne protège une page qu'après avoir traduit son code, et une écriture qui tombe entre les deux n'est jamais vue. INFÉRÉ : FEX amont a le même ordre (`MarkGuestExecutableRange` après la compilation). Pas corrigé pour les pages qui restent en suivi.
- `w01` à `w21` (sauf `w07`, qui dépasse son délai avant comme après), `w23`, `w24`, `w26` et `w27` : mêmes résultats avant et après. `w06_smc` : 20/20 en mtrack et en full, 12/20 en none, comme avant.
- `w22_mutexsig` échoue parfois sur les deux moteurs, par un saut vers une adresse invalide : 1 fois sur 66 avant, 2 fois sur 67 après. `w26_reaper` a pris une fois le SIGBUS du répartiteur FEX juste après un fork (demande `wine-5`, déjà connue), puis 6 passages sur 6.
- Suite LKD : 42 programmes passent, `test_sig_futex` dépasse son délai comme sur l'arbre intact. Sondes EAC : 4 376 contrôles, les mêmes 38 échecs.
- Empreintes des binaires du jeu et d'EAC inchangées.

Limites :
- Les FPS de Rocket League en partie ne sont pas mesurés. Sans session Epic le jeu quitte au bout de 19 s sans afficher d'image, et l'app Obelisk servait à un autre travail. Comme aucune page de RL ne quitte le suivi, FEXCore produit le même code qu'avant ; il reste une question par page de code à chaque compilation, qui rend faux sans chercher tant qu'aucune page n'est marquée.
- Une instruction invalide dans une page vérifiée n'est pas revérifiée. Si un programme corrige ses octets en place plus tard, le bloc lève encore SIGILL. Je n'ai vu aucun programme le faire.

Rapport : `sailover/build/experiments/eldenring-linux-20261010/reports/11-fex-smc-eac.md`.

## SCM_RIGHTS : les périphériques émulés, les répertoires synthétiques et les autres objets sans fd hôte passent d'un processus hôte à l'autre (2026-10-10, 13:15)

Contexte : avec le runtime -13-dev2, l'EAC d'ELDEN RING rend 301 dans Obelisk, puis le jeu meurt une seconde plus tard : `lkd: net: SCM_RIGHTS of a dev object cannot cross host processes (-EOPNOTSUPP)` et `wine client error:198: sendmsg: Operation not supported`. `steam.exe` crée `start_protected_game.exe` sans handles standard, Wine lui donne `/dev/null` en fd 0 et 1 (`ntdll/unix/process.c` `set_stdio_fd`). Sous LKD c'est le `/dev/null` émulé, sans fd hôte. Le processus Wine d'`eldenring.exe`, lancé par l'enfant d'EAC, n'a pas de parent Wine et envoie ses fd 0, 1 et 2 à wineserver (`env.c` `get_initial_console`). `net_scm_export` refusait tout objet sans fd hôte. Même refus pour le répertoire synthétique `/games` (rapport 06 de sailover). Sonde : `wine cmd /c ver </dev/null` échouait de la même façon.

Inventaire des sortes de descripteurs (avant) :

| Sorte | fd hôte | SCM_RIGHTS avant |
|---|---|---|
| fichier, répertoire hôte, tube, socket, tty, shm hôte, memfd | oui | fd hôte (memfd : fichier supprimé, sans nom ni scellés) |
| eventfd | non (région shm et tube) | 3 fd hôtes |
| ntsync, périphérique et objets | non (ancre) | l'ancre |
| /dev/null, zero, full, random, urandom | non | refusé |
| répertoire synthétique, répertoire de /proc, /dev, /sys | non | refusé |
| O_PATH | non | refusé |
| fichier généré de /proc et /sys, /proc/<pid>/mem | non | refusé |
| signalfd | non (tube de notification) | refusé |
| epoll, timerfd, inotify | kqueue | refusé (macOS ne passe pas une kqueue : EINVAL) |

Correctif (`4ad078c`, témoin `7f98f29`), mécanisme général : `lkd_fd_ops.export_fds` dit ce qui porte une description, le récepteur la reconstruit (`net_scm_import`).
- Jumeaux hôtes. `/dev/null`, `/dev/zero`, `/dev/random` et `/dev/urandom` envoient le périphérique macOS de même comportement, ouvert au premier envoi avec le mode d'accès invité. Un jumeau qui arrive sans chemin invité, par SCM_RIGHTS ou comme entrée standard du runtime, redevient le périphérique Linux (1:3, 1:5, 1:8, 1:9) : `lkd_procfs_dev_adopt`, appelé par `fs_wrap_host_fd`. Lectures et écritures restent dans LKD. MESURE : macOS refuse `write` sur `/dev/urandom` (EPERM), rend POLLNVAL à `poll` et EINVAL à `kevent` sur ses périphériques ; `F_SETFL` O_NONBLOCK et O_APPEND y passent et se partagent par SCM_RIGHTS.
- Enregistrements (`src/core/fdrec.c`). Un objet sans forme hôte part comme un objet shm POSIX de 16 Kio, supprimé dès sa création, avec la signature `LKDFREC1`, son type, les drapeaux d'état Linux et des données de niveau invité seulement (pas de chemin hôte, pas de pointeur). Le module propriétaire rouvre la chose dans le récepteur, avec les droits du récepteur : répertoires synthétiques et virtuels, O_PATH (chemin invité global, `fs_rec_import`) ; `/dev/full`, sans jumeau macOS, et les périphériques O_PATH ; fichiers générés et `/proc/<pid>/mem` (chemin canonique et position) ; signalfd (son masque ; il lit les signaux du récepteur, comme `signalfd_dequeue` sur `current`).
- epoll, timerfd et inotify restent refusés : leur état est une kqueue et une table de ce processus.

Écarts : le récepteur d'un enregistrement a une nouvelle description de la même chose. Position et drapeaux changés ensuite ne sont pas partagés, la règle de `fd.h` pour les objets sans fd hôte. Constatés en passant, non corrigés : un tube créé par LKD arrive non bloquant chez le récepteur (son fd hôte l'est toujours, MESURE par `F_GETFL`) ; un eventfd EFD_NONBLOCK arrive bloquant ; un memfd arrive en fichier ordinaire.

MESURE :
- `test_net_scm` (nouveau, 5 cas, 38 assertions) : périphériques aller et retour, `/dev/null` hôte en entrée standard, répertoire synthétique, `/dev`, `/proc/<pid>`, O_PATH, `/proc/<pid>/status` à sa position, signalfd, entre deux processus hôtes. Les 5 cas échouent sur l'arbre d'avant. Suite : 43 programmes passent ; `test_sig_futex` dépasse son délai au même cas (`waitv`) que sur l'arbre intact.
- Témoin `w30_scmdev` (glibc, nouveau) : rien de reçu avec le moteur de `c545aa6` (même LKD que `97f227e`), `W30 PASS` après. `w22` à `w27` passent avec le moteur final (`w25` par `hostpid.sh`).
- Sonde Wine, préfixe et runtime copiés : `wine cmd /c ver </dev/null` rendait 1 avec `wine client error ... sendmsg: Operation not supported`, rend 0 (`Microsoft Windows 10.0.19045`). `wine cmd /c ver` seul : 0 avant et après. Deux sessions Wine de suite (`wineserver -p0` entre elles) restent parfois bloquées, avec l'ancien moteur aussi (1 fois sur 3) : le second `cmd.exe` attend une réponse de wineserver qui ne vient pas. Antérieur, non traité.
- Rocket League (`rl-direct.sh` copié, runtime et préfixe privés, `/opt/game` en lecture seule) : `301` en 0,39 s.
- ELDEN RING derrière `steam.exe` (préfixe privé, sans hôte Steam) : `301` en 2,7 s, plus d'erreur SCM ni d'erreur client de Wine.
- ELDEN RING lancé depuis Obelisk, moteur de `4ad078c` posé à la main (avec le correctif SMC de `3932460`) : `301` en 2,74 s (au lieu de 241,5 s au rapport 10), plus d'erreur SCM. Aucune question Cloud, sauvegarde et binaires du jeu inchangés.

Le jeu ne va pas plus loin, et ce n'est plus ce défaut. Le processus Wine d'`eldenring.exe` disparaît moins d'une seconde après le 301, sans message (sa sortie d'erreur est le `/dev/null` de `set_stdio_fd`), sans fenêtre ni device D3D12. MESURE, trace LKD d'un essai privé derrière `steam.exe` : après l'exec, le processus parle à wineserver, crée cinq fils ; l'un lance un programme avec sa sortie dans un tube (`clone3` puis `execve`, comme `popen`), qui finit en 127 (INFÉRÉ : `sh -c` d'une commande introuvable) ; puis les cinq fils sortent ensemble de leurs attentes par EINTR et le processus disparaît sans `exit_group` : un signal fatal. fexdarwin ne rapporte aucune faute. Le lancement direct du rapport 09 (`er-ge2-mtrack`, entrées-sorties du lanceur, donc sans ce défaut) finissait déjà de la même façon : toute la session s'arrêtait 2 s après le 301, sans sortie normale du jeu. INFÉRÉ : c'est le prochain verrou, indépendant de SCM_RIGHTS. Le nom de la commande et le signal ne sont pas identifiés. Le plantage `0xdeadba`, `SteamAPI_Init` et les fps ne sont pas atteints. Rapport : `sailover/build/experiments/eldenring-linux-20261010/reports/12-lkd-scm-devices.md`.

## ELDEN RING après son 301 : un dlopen de CrossOver avant que GS désigne le TEB (2026-10-10, 13:55)

Contexte : l'entrée précédente laissait le processus d'`eldenring.exe` mourir moins d'une seconde après le 301 d'EAC, par un signal fatal inconnu. LKD n'avait aucun moyen de dire quel signal tue un processus.

Correctif LKD, diagnostic seulement : avec `LKD_TRACE`, le signal qui met fin à un processus donne une ligne `lkd: pid N dies of signal S in tid T: ...`, avec l'émetteur (si_code <= 0) ou l'adresse fautive et le rip. Elle part de `complete_signal` et de `lkd_signal_next`. Sans `LKD_TRACE`, rien ne change. Cas `fatal_signal_trace` dans `test_sig_basic`.

MESURE, essais privés derrière `steam.exe` (runtime -13-dev2 copié, moteur `f587e5a3` puis moteur de ce commit, préfixe privé, sans hôte Steam) :
- La mort : `lkd: pid 77 dies of signal 11 in tid 77: fault at 0x30 (si_code 1), rip 0x7f827548df5c`. Ni kill ni tgkill : une faute du fil principal, avant que Wine installe ses gestionnaires (`signal_init_process`), donc action par défaut.
- Le rip est dans le second ELF d'EAC, injecté dans le jeu (memfd, chargé RWX) : `module+0x48df5c`, `movq %gs:0x30, %rax`. EAC lit le TEB par GS pour parcourir la liste des modules Windows du PEB.
- Le dernier appel du fil avant la faute : `openat("/opt/wine/lib/wine/x86_64-unix/cxcompatdb.so") = ENOENT`. Dans voile-wine, `start_main_thread` fait `dlopen` de cette bibliothèque de CrossOver (CW Hack 24067), juste avant `server_init_process_done`. Sur Linux, Wine ne pose la base GS sur le TEB qu'ensuite (`init_syscall_frame`, `arch_prctl(ARCH_SET_GS)`). GS vaut 0, la lecture tombe sur 0x30. Voile ne livre aucun `cxcompatdb.so`.
- A/B : `ntdll.so` du runtime avec l'appel à `dlopen` remplacé par `xor eax,eax` : plus de faute, le jeu continue jusqu'à steamclient. Un `cxcompatdb.so` vide à sa place (dlopen réussi) : même faute à 0x30. EAC réagit donc à tout `dlopen` de ce fil. Proton et Wine amont ne font aucun `dlopen` à cet endroit.
- Le programme lancé comme `popen` est `sh -c -- "lshw -C display 2>/dev/null"` : `lshw` manque, `sh` rend 127, EAC lit une sortie vide et continue. Le fichier de 255 octets est `/proc/cpuinfo`. Les cinq fils (tids 78 à 82) sont créés pendant le `dlopen` injecté du module d'EAC, avant la suite de l'initialisation de Wine. Rien de cela ne tue le jeu.

Correctif, hors de ce dépôt : voile-wine `1aeb924` retire ce `dlopen`. voile-wine `829f80c` reprend aussi le correctif de Proton pour ce cas (`aa83aa0a89`, « Set thread teb earlier in start_main_thread() », CW-Bug-Id #22218) : sur Linux x86-64, GS désigne le TEB dès `init_files`. MESURE : avec ce seul changement, `dlopen` gardé, plus de faute à 0x30 non plus. Voile `787c237` puis `eda2e17` prennent ces commits. `ntdll.so` reconstruit avec la recette de Voile (la même recette rend l'`ntdll.so` installé à l'octet près), puis passé dans le runtime privé :
- ELDEN RING : 301, puis le jeu charge steamclient, échoue faute d'hôte Steam (`Failed to connect to Steam`, attendu hors de l'app) et sort. À sa sortie, un fil d'EAC (tid 78) saute dans `main_arena` de la glibc (motif fd/bk d'un bloc libéré), puis le gestionnaire SIGSEGV de Wine faute à son tour sur ce fil sans TEB. INFÉRÉ : usage après libération côté EAC pendant la sortie. Non traité.
- Rocket League (`rl-direct.sh` copié, sans Epic) : 301 en 0,37 à 0,39 s, avec le premier commit seul puis avec les deux.

Vus en passant, non corrigés :
- EAC retire `LD_PRELOAD` de l'environnement du jeu : `steam-hostpid.so` n'est plus chargé dans `eldenring.exe`. steamclient.so y enverra donc le pid invité au moteur Steam d'Obelisk. À vérifier dans l'app, avec l'hôte Steam.
- `semget` (IPC System V) rend ENOSYS : steamclient.so l'affiche en assertion (`threadtools.cpp (2526)`), puis veut écrire un minidump avec `clone(CLONE_FS | CLONE_FILES | CLONE_UNTRACED)` sans CLONE_VM, que LKD refuse (EINVAL).
- La racine Debian n'a ni `/etc/os-release`, ni `/etc/machine-id`, ni `/etc/hostname`, qu'EAC lit.

Suite : 43 programmes passent, `test_sig_futex` dépasse son délai au même cas qu'avant. Rapport : `sailover/build/experiments/eldenring-linux-20261010/reports/13-eldenring-apres-301.md`.

## ELDEN RING dans Obelisk : Steam tient, puis Wwise abandonne faute de masque CPU (2026-10-10, 14:45)

Contexte : l'entrée précédente laissait ELDEN RING passer son 301 en essai privé, sans hôte Steam. Premier essai dans Obelisk (runtime -13-dev2, `ntdll.so` de voile-wine `829f80c`, moteur `f587e5a3`).

MESURE, trois lancements depuis le bouton « Lancer » :
- Lancement 1 : 301 en 1,37 s, fenêtre plein écran 2560x1440, device D3D12. Le moteur Steam d'Obelisk ferme la connexion du jeu une seconde après son ouverture, puis steamclient.so s'arrête : `fatal stalled cross-thread pipe (pipe is disconnected)`. C'est le pid invité de l'entrée du 2026-10-10, 08:20 : EAC retire `LD_PRELOAD`, `steam-hostpid.so` manque dans `eldenring.exe`.
- Correctif hors de ce dépôt : Voile écrit `/etc/ld.so.preload` (`/opt/obelisk/steam-hostpid.so`) dans la racine. Le chargeur de la glibc le lit à chaque exec, quel que soit l'environnement. Les traces du rapport 13 montrent que seul ld.so lit ce fichier (un `access` par exec), jamais le module d'EAC.
- Lancement 2 : `steam-hostpid.so` est dans la liste des modules du jeu, la connexion Steam tient 42 s. Le jeu crée sa chaîne d'échange, puis s'arrête sur `movl $0xdeadba, 0` (`eldenring+0x1ebb809`), le plantage du rapport 07. Sur la pile : `Failed MagicOrchestra_Wwise::MOSoundManager::Initialize()` et `DL_PANIC is firing core dump...`. Juste avant : `wineserver: file_set_error() can't map error: Function not implemented`.
- Cause : l'`AkCreateThread` de Wwise (`eldenring+0x2385af4` à `0x2385b47`) crée le fil, puis appelle `SetThreadAffinityMask`. En cas d'échec, il ferme le fil et rend AK_Fail. wineserver applique ce masque par `sched_setaffinity(unix_tid)` sur un fil du jeu, donc d'un autre processus hôte, et LKD répondait ENOSYS.

Correctif LKD : `sched_setaffinity` sur un fil d'un autre processus hôte passe par le service de la famille (opcode `TASK_OP_SET_AFFINITY`). Le processus qui possède le fil pose le masque et le publie. Cas `affinity_of_a_thread_in_another_host_process` dans `test_task_proc` : ENOSYS avant, réussite après.

MESURE après le correctif :
- Lancement 3 : 301 en 1,39 s, plus de `can't map error`. mmdevapi et `wineobeliskaudio` sont chargés : Wwise s'initialise. La chaîne d'échange passe en `WSI up 2560x1440`, une image est présentée.
- Le jeu s'arrête ensuite dans Neptune : `ucrtbase+0x7b1b0` lit l'adresse 1, appelé par `npt_id3d12object_default_SetName` (d3d12.dll). L'appelant, `eldenring+0x1e8eae5`, fait `call *0x210(%rax)` avec `(1, &{va, 2}, &{2})` : c'est le slot 66, `ID3D12GraphicsCommandList2::WriteBufferImmediate` (marqueurs). Dans la d3d12.dll de Neptune, la table v0 de `ID3D12GraphicsCommandList` plus 0x210 tombe sur `npt_id3d12heap1_default_vtbl_storage + 0x30`, le `SetName` de la table suivante. INFÉRÉ : la liste a été créée avec l'IID de la v0, et le jeu l'utilise comme une v2 sans QueryInterface. Cela marche sous Windows et vkd3d-proton, où une seule table couvre toutes les versions. Neptune donne à l'enveloppe la table de l'IID demandé. À traiter dans Neptune, non fait ici.
- Rocket League, essai privé avec ce moteur et `/etc/ld.so.preload` : 301 en 0,41 s. Sans `/opt/obelisk`, le chargeur écrit une ligne `cannot be preloaded ... ignored` par processus, sans autre effet.

Suite : 43 programmes passent, `test_sig_futex` dépasse son délai au même cas qu'avant. Rapport : `sailover/build/experiments/eldenring-linux-20261010/reports/14-eldenring-app-titre.md`.

## ELDEN RING : le moteur manquait d'espace d'adressage hôte, puis wsd n'affichait pas la seconde chaîne d'échange (2026-10-10, 17:10)

Contexte : après la correction des vtables de Neptune, ELDEN RING présentait 61 images dans Obelisk, puis le moteur mourait : `[fexdarwin] FATAL unhandled fault: signal 11 (si_code 2) fault address 0x10005297`, hors du code JIT, avec un état invité identique à chaque lancement.

MESURE :
- L'état invité est celui de l'enfant d'un `clone3` qui n'a encore rien exécuté : rip = `libc.so.6+0x110529`, juste après le `syscall` de `__clone3`, rax = 0, rdx = `start_thread`.
- Avec l'ancien moteur et un relevé des régions par `proc_pidinfo` (sans suspendre le processus) : pc = `fexdarwin-run+0x98390`, soit `FEXCore::LookupCache::FindBlock+32`. Une variante du même arbre qui écrit les registres : appelée par `CompileBlock+132`, `this->L1Pointer` vaut 0xfffffff. C'est -1 + 128 Mio + 128 Mio : le mmap du cache de recherche du nouveau fil (260 Mio) a échoué, l'assertion qui le vérifie n'existe pas en Release, et le cache a été construit sur le -1. 0xfffffff + 0x529 x 16 + 8 = 0x10005297.
- La cause de l'échec : l'espace d'adressage de l'hôte était plein. macOS 27 garde [0xfc0000000, 0x7000000000) pour lui (VM_MEMORY_GUARD, présent dans tout processus), la fenêtre invitée commençait à 0x7d80000000. Il restait 54 Gio à l'hôte, dont libmalloc réserve 12 ou 24. Chaque fil invité en prend environ 280 Mio (cache de recherche de FEXCore, pile call-ret, pile hôte). Relevé une seconde avant la faute : 53,4 Gio sur 54 occupés, plus grand trou 387 Mio, environ 110 fils.

Correctifs :
- FEX `7f6e2a3a7` : un cache de recherche qui n'a pas pu être réservé le dit (`IsAllocated`), `InitializeCompiler` s'arrête avant de s'en servir et `CreateThread` rend nullptr.
- fexdarwin : la fenêtre invitée commence à 1 Tio, ce qui laisse 576 Gio à l'hôte (les 300 fils par processus de LKD en demandent 85). Adresses invitées jusqu'à 0x7efffe000000, toujours au-dessus des mappings fixes d'EAC à 0x7001'0000'0000. Un fil que l'hôte ne peut pas loger rend EAGAIN à `clone`, comme Linux sans mémoire pour une nouvelle tâche. Un `pthread_create` raté ne laisse plus le fil dans la liste, et les fils finis sont récoltés avant d'en créer un. `FEXDARWIN_GUEST_BASE` place maintenant toute la fenêtre.
- fexdarwin : une faute hôte fatale écrit l'adresse de chargement (pour `atos -l`), l'offset et le symbole de pc et lr, les régions de pc et de l'adresse fautive, les registres et la chaîne des pointeurs de cadre, puis le fil invité : son appel système, module et offset à rip, octets de code, adresses de retour sur sa pile. Lecture sans verrou (`mach_vm_read_overwrite`, `lkd_mm_find_nowait`). Rien ne s'exécute avant une faute fatale.
- Témoin `w31_threadvm` : 280 fils en attente, puis join et 50 de plus. Avec `FEXDARWIN_GUEST_BASE=0x7f00000000` (l'ancienne place) : l'ancien moteur meurt à 0x10005297 en 0,3 s, le nouveau rend EAGAIN après 123 fils et passe. Sans la variable : 280 fils.

Le verrou suivant, dans wsd : ELDEN RING recrée sa chaîne d'échange avant le titre. Neptune retire alors le plan de superposition (région 0x0) puis présente au même rectangle. wsd gardait ce rectangle comme courant et ne plaçait jamais le calque du nouveau plan : il restait 0x0, aucune image n'était montrée (fenêtre noire), et chaque présentation attendait les 100 ms du délai de cadence de Neptune (10 i/s, `1380 presents (0 shown events)`). Correctif : un nouveau plan est toujours placé. Test `tests/graphics/wsd_overlay.c` : 0 image montrée sur le plan recréé avant, 29 sur 30 après.

MESURE dans Obelisk avec les deux correctifs : 301 d'EAC, première image à 17:02:09, seconde chaîne d'échange à 17:05:52, puis l'écran titre d'ELDEN RING à 60 i/s (120 images : moyenne 16,67 ms, min 12,5, max 20,8). Quitter par Obelisk, Cloud synchronisé, aucune question. Le chargement prend encore 3 min 40 : au profil (lancement précédent), le fil « CR2.Device(HDD) » du jeu tourne à 100 % d'un cœur pendant une minute dans du code traduit, sans fautes SMC ni temps de JIT notable, et le reste attend. À étudier à part.

Suites : LKD 44 programmes dont `test_mm_map` (cas `find_nowait`), `test_sig_futex` dépasse son délai au même cas qu'avant ; témoins w01 à w31 comme avant (w07 dépasse son délai après 99/99) ; sondes EAC 38 échecs sur 4 376, les mêmes ; Rocket League 301 en 0,39 s. Le SIGBUS du répartiteur dans un enfant de fork (wineboot) est celui de la demande `wine-5`, non traité. Rapport : `sailover/build/experiments/eldenring-linux-20261010/reports/16-fexdarwin-fault.md`.

## ELDEN RING démarre en ligne : un mmap de 4 Gio parcourait toutes les VMA pour chaque page hôte (2026-10-10, 19:05)

Contexte. Avec le texte revenu, ELDEN RING affichait à l'écran titre « Unable to start in online mode due to a detected frame rate issue », puis un menu OFFLINE. Juste avant la boîte, les présentations 361 à 420 de la seconde chaîne d'échange duraient 162,8 ms en moyenne, soit environ 9,7 s d'arrêt, avec un délai de barrière dépassé. Le jeu crée à ce moment deux tampons UPLOAD engagés de 4 Gio pile. Neptune adosse chacun à un objet shm de l'hôte que le jeu mappe MAP_SHARED, et le serveur de rendu l'importe dans D3DMetal.

MESURE, création d'un tampon de 4 Gio dans le harnais D3D12 (`online18`, mode `bigup`) : 1 265 à 1 284 ms. Décomposition :
- côté hôte seul, sous Rosetta : objet shm 0,01 ms, mmap 0,003 ms, import du tas 0,004 ms, tampon placé 66 ms ;
- le reste, environ 1,2 s, est le mmap MAP_SHARED de 4 Gio dans l'invité, dans LKD.

La cause est `mm_watch_alias_sync` (src/mm/watch.c). Une nouvelle vue partagée doit naître protégée en écriture sur les pages dont une autre vue des mêmes octets porte du code traduit. Pour chacune des 262 144 pages hôtes de 16 Kio de la vue, l'ancienne boucle cherchait les alias en parcourant toutes les VMA du processus, verrou de surveillance tenu. Le coût suit pages × VMA. Témoin w32, mmap de 4 Gio : 37,6 ms avec 18 mappings, 13,6 s avec 2 017, plus de 120 s avec 8 017. Pendant ce temps, tout fil qui traduisait du code attendait le verrou.

Correctif (12eeeba). Les pages intérieures de la vue n'appartiennent qu'à la nouvelle VMA. Une autre vue du même objet les tient donc à un décalage constant, ou pas du tout. Un seul parcours des VMA trouve ces vues, puis un balayage des bits de surveillance de chacune (64 pages par mot) marque les pages correspondantes. Les deux pages de bord, qu'une VMA voisine peut partager, gardent la recherche page par page. Au-delà de 32 vues d'un même objet, on retombe sur l'ancienne boucle. La sémantique ne change pas : le cas `late_view_of_a_large_object` de test_mm_watch place le code à 32 Mio dans l'objet et vérifie la page surveillée de la vue tardive, ses voisines non surveillées et l'invalidation par une écriture.

MESURE après :
- w32 : 0,032 ms (18 mappings), 0,072 ms (2 017), 0,258 ms (8 017). Le code réécrit par une vue créée après coup tourne bien dans la vue RX, à 32 Mio dans l'objet comme au début.
- test_mm_watch, vues de 64 Mio et 1 Gio avec 4 000 VMA, en natif : 7,4 s avant, sous 250 ms après.
- Harnais `bigup` (police 4 Gio + 1 Kio, puis deux tampons de 4 Gio, données relues) : 63 à 160 ms par tampon au lieu de 1,27 s (une fois 460 ms sur 27 créations), séquence 0,23 à 0,31 s au lieu de 4,4 s (une fois 0,64 s). `bigframes` avec la séquence sur le fil de rendu : plus longue image 247 ms au lieu de 4 727 ms. 1 000 tampons UPLOAD de 64 Kio : 0,12 ms chacun au lieu de 0,17.
- Les 66 ms qui restent viennent du wrapper d3dmetal-native : il ajoute chaque tampon importé à un MTLResidencySet et le valide aussitôt, ce qui épingle tout le tampon. MESURE : +259 745 pages de 16 Kio câblées pour un tampon de 4 Gio ; avec `DMN_RESIDENCY_SET=0`, le tampon placé coûte 0,1 ms. ELDEN RING câble ainsi 12 Gio pour ses trois tampons. Non traité.

MESURE dans Obelisk avec ce moteur (deux lancements). Le premier n'a pas atteint l'écran titre en 300 s : chargement 4 fois plus lent que d'habitude (imports de tas 35 à 45 par 10 s contre 160), cause non trouvée. Le second, même moteur, `FEXDARWIN_PROFILE=1` : EAC 301 en 1,38 s, écran titre 48 s après la première image. Présentations 361 à 420 : 41,9 ms en moyenne, aucun délai de barrière dépassé ; mmap de tout le processus 417 appels, 94 ms sur la période du premier tampon de 4 Gio. Aucune boîte de fréquence d'images. Après « PRESS ANY BUTTON » et l'avertissement de session mal fermée, le jeu affiche « Logging in to the ELDEN RING game server… » puis les informations du serveur : il démarre en ligne.

Suites : LKD 43 programmes passent, `test_sig_futex` dépasse son délai au même cas qu'avant ; témoins w01 à w32 comme avant (w07 en délai après 99/99, w03 identique octet pour octet) ; sondes EAC 38 échecs sur 4 376, les mêmes ; Rocket League 301 trois fois sur quatre (0,38, 1,37 et 0,39 s). Le quatrième lancement a rendu 210 : SIGBUS du répartiteur de FEX dans un enfant de fork, la signature de la demande `wine-5`, intermittente et antérieure. Vu en passant, déjà présent avec l'ancien moteur : un SIGILL hôte à la fin d'un fil invité (`EndGuestThread`, dans `pt_find_tracee` ou le munmap de `UnregisterThread`), au démontage. Rapport : `sailover/build/experiments/eldenring-linux-20261010/reports/18-demarrage-online.md`.

## Le SIGBUS au fork (demande wine-5) : l'enfant recevait un état d'écriture JIT périmé (2026-10-10, 20:40)

Contexte. Depuis le 2026-10-02, un enfant de fork meurt parfois avant sa première instruction invitée : `[fexdarwin] FATAL unhandled fault: signal 10 (si_code 1)`, pc égal à l'adresse fautive, dans le répartiteur de FEX, au retour de `clone` aux drapeaux de fork (rdi = 0x1200011, rax = 0). Quand l'enfant est un processus du lanceur EAC, Rocket League finit en 210. Dans les essais du jour, environ un démarrage de Wine sur 30 à 60 (souvent le fork de wineserver qui se détache), et un lancement de Rocket League sur quatre au rapport 18.

MESURE dans les fautes déjà enregistrées (rapport de faute fatale) : esr 0x8200000f, une faute de permission à l'exécution de niveau 3 ; la région du pc est le tampon du répartiteur, 64 Kio rwx/rwx ; l'offset dans la région est toujours 0x5f4 ou 0x604, la première instruction exécutée après le retour de la syscall. La page est là et exécutable, mais pas pour ce fil.

MESURE en natif, sans FEX (`tests/darwin-process/f7_fork_jitstate.c`, fait F7 de `darwin-process-facts-a.md`) : le fil de l'enfant d'un fork reçoit l'état d'écriture MAP_JIT que le fil du parent avait à son dernier changement de contexte, pas son état réel. Un parent bloqué ou préempté en mode écriture, puis revenu en exécution sans changer de contexte, donne 37 à 39 enfants sur 40 qui meurent en SIGBUS au premier code JIT. Un appel système ou une faute de page en mode écriture ne laissent rien de périmé ; un fil créé par `pthread_create` n'est pas touché ; un `pthread_jit_write_protect_np(1)` dans l'enfant suffit.

Côté moteur, `JITWriteScope` de FEXCore bascule ce mode à chaque traduction (copie du bloc), liaison et déliaison de bloc : 0,04 à 2,5 µs par fenêtre, mesuré avec un anneau de fenêtres par fil dans un moteur de diagnostic jamais commité. Une sonde dans l'enfant (exécution d'une page MAP_JIT sous gestionnaire SIGBUS) voyait chaque état périmé, puis le corrigeait. Une préemption pendant une de ces fenêtres, puis aucun changement de contexte jusqu'au fork : la faute. Les appels système de LKD restent dans le processus et ne commutent souvent pas, ce qui laisse le temps.

Correctif : `ForkChild` repose l'état du fil, exécution hors d'une écriture, avant tout retour au code JIT.

Témoin `w33_forkwx` (README des témoins glibc). Un appelant de 60 000 appels directs vers 256 feuilles est traduit une fois. À chaque tour, les feuilles sont remappées, l'appelant tourne (tous les sites sont liés), les feuilles sont démappées (le moteur défait les 60 000 liens d'un coup), puis fork. 2 fils actifs par CPU chargent la machine. La rafale doit durer au moins un quantum d'ordonnancement : 0 mort sur 1 000 tours avec 20 000 sites, 14 sur 500 avec 60 000, 7 sur 200 avec 150 000. Moteur de 8ddfed2 : 15 enfants morts sur 600 tours, chacun avec la faute du répartiteur à l'offset 0x5f4. Moteur corrigé : 0 sur 600, deux fois. Les modes plus simples (fork après traduction, ou fork puis exec comme wineserver) ne reproduisaient pas : 0 sur 2 000 et 0 sur 600.

Suites avec le moteur corrigé : LKD 43 programmes passent, `test_sig_futex` dépasse son délai au même cas qu'avant ; témoins w01 à w33 comme avant (w07 en délai après 99/99, w03 identique octet pour octet, w25 hostpid PASS, w31 aussi avec l'ancienne base) ; sondes EAC 38 échecs sur 4 376, les mêmes sondes et les mêmes premiers échecs ; Rocket League (`rl-direct.sh`, runtime privé) 301 douze fois sur douze, dont six sous charge (mapping EAC 0,60 à 0,70 s). Harnais D3D12 et D3D11 du rapport 18, deux passes : relectures justes, le trou connu de la résolution de renderpass, la mort du worker signalée sans blocage. Le moteur de diagnostic avait déjà donné 301 à 12 lancements, 264 forks, aucun état périmé : la faute dépend de la charge de la machine. Rapport : `sailover/build/experiments/eldenring-linux-20261010/reports/19-sigbus-fork.md`.

## ELDEN RING : le chargement de 5 minutes venait du cache de shaders Metal, vidé par DXMT (2026-10-10, 22:20)

Contexte. Entre la première image et l'écran titre, ELDEN RING prenait 48 s, 88 s, 3 min 43 s ou plus de 300 s, avec le même moteur. Au rapport 16, le fil « CR2.Device(HDD) » brûlait un cœur pendant une minute.

MESURE (9 lancements depuis Obelisk, profil, vidages et relevés toutes les 5 s) :
- un lancement lent (275 à 298 s) a `MTLCompilerService` à 90-98 % d'un cœur dans chaque relevé, de la première image au titre. CR2 y brûle 147 à 183 s de CPU sans appel système : il attend en boucle active ;
- un lancement chaud (7 à 48 s) n'a aucune compilation Metal, et CR2 n'y prend que 3 à 10 s.

La cause : les deux serveurs de rendu, l'arm64 de DXMT et l'Intel de D3DMetal sous Rosetta, tournaient hors de tout bundle. Metal rangeait leurs fonctions compilées dans un seul cache, `$DARWIN_USER_CACHE_DIR/com.apple.metal/16777235_355/`. Chaque démarrage du serveur de DXMT le vidait : après un ELDEN RING, `functions.data` fait 46,4 Mo et environ 7 500 entrées ; après un Rocket League, 320 Ko ; après le harnais D3D11 sur DXMT, 128 Ko. Deux Rocket League puis ELDEN RING : 296 s jusqu'au titre.

Correctif dans le lanceur d'Obelisk (sailover `787942a`, local, non poussé) : chaque serveur tourne depuis un bundle d'aide dans celui du jeu, `<jeu>.app/Contents/Helpers/<backend>.app`, avec son propre identifiant, donc son propre cache Metal. Deux faits macOS mesurés en route :
- pendant un instant après le démarrage d'un exécutable d'un bundle, macOS refuse toute nouvelle écriture dans ce bundle (EPERM, aussi hors bac à sable ; 0,3 s plus tard, elles passent). Le bundle d'aide est donc prêt avant wsd ;
- Rosetta traduit chaque nouveau fichier à son premier lancement : 0,34 à 0,55 s pour une copie neuve du serveur Intel, 9 ms ensuite. La copie est donc gardée d'un lancement à l'autre et traduite avant le jeu, hors de la fenêtre d'EAC.

MESURE après : le cache d'ELDEN RING (46,5 Mo) ne bouge pas pendant deux Rocket League, et ELDEN RING arrive au titre 7 s après sa première image. EAC 301 à chaque lancement (mapping 1,38 à 1,43 s).

Reste, avec un cache chaud : le titre arrive 7-8 s ou 43-48 s après la première image. Dans les lents, le fil principal tourne en NtYieldExecution dans un module de plus de 11 Mo, sans doute `eossdk-win64-shipping.dll`. Hors de notre pile. Le cache propre de D3DMetal (`d3dm/virgl_render_server`) n'est plus écrit depuis 17:20, peut-être parce que le serveur se termine par SIGTERM. Rapport : `sailover/build/experiments/eldenring-linux-20261010/reports/19-sigbus-fork.md`.

## ELDEN RING : le son craquait en permanence, et le lancement passe de 156 à 66 s avant la première image (2026-10-10, 23:30)

Contexte. L'utilisateur entendait le son « buguer » à l'ouverture du menu. Mesure avec un wsd de diagnostic jamais commité (compteurs atomiques dans le rappel de l'AudioUnit, une ligne par seconde) : CoreAudio lit 512 trames par rappel, 94 fois par seconde, et l'anneau du pilote `wineobeliskaudio` en avait souvent moins. 860 lectures courtes en 154 s, 5,6 par seconde, 6,3 ms de silence par seconde, et des pics à chaque transition du menu (1 056 trames juste après un appui). Ce n'était pas l'ouverture du menu seule : le son craquait tout le temps.

La cause. mmdevapi impose en mode partagé un tampon de 3 périodes (30 ms) et FAudio le garde à peu près plein. Mais le pilote ne recopiait les trames écrites par le client dans l'anneau de wsd qu'à son tic suivant, jusqu'à une période plus tard : l'anneau avait environ une période de moins que ce que le client avait écrit, et son niveau descendait à 480 trames, sous les 512 d'un rappel.

Correctif (voile-wine `f04f2bb`, Voile `451d1e1`) : `ReleaseBuffer` pousse aussitôt les trames dans l'anneau ; le minuteur continue de le compléter. MESURE : 0 lecture courte sur 146 s (titre, boîte de session, fenêtre d'information, menu principal), puis 0 sur 91 s au lancement suivant ; niveau minimal de l'anneau 576 trames au lieu de 0 à 480. La latence baisse d'une période au plus. Le même pilote sert à Rocket League.

Fuseau horaire. La racine invitée n'a pas de `/etc/localtime` : tout tournait en UTC, et glibc retentait ce fichier à chaque `localtime()`, environ 80 fois par seconde dans ELDEN RING (1,9 µs au lieu de 0,04, mesuré dans l'invité). Le lanceur d'Obelisk passe maintenant `TZ` à l'invité, la règle POSIX qui termine le fichier de zone du Mac (sailover `d63ca60`, local) : 0 recherche ensuite, heure locale juste.

Côté Obelisk (sailover, commits locaux non poussés), mesuré au clic près par des horodatages temporaires :
- la vérification des 55 Go avant lancement prenait 2 min dans l'app Debug, parce que `sha1` y tournait sans optimisation (284 Mo/s par fil contre 3 025 Mo/s) ; ces crates sont optimisées même en Debug (`03d1f1e`) : 11 à 13 s, le SSD plafonnant à 6,8 Go/s ;
- la synchronisation Cloud prenait 25 s, dont 15 perdues sur un serveur CM qui acceptait la connexion TCP sans finir la poignée de main websocket (port 27018, à travers le VPN de l'utilisateur) ; les serveurs sont maintenant essayés en course décalée d'une seconde (`3074347`) : 9 à 10 s.

MESURE, du clic à la première image : 156 s au premier lancement de la soirée, 66 s en régime établi (73 s quand Obelisk vient d'être reconstruit et réextrait son runtime Steam). Reste avant le moteur : vérification 11 s, Cloud 9 s, démarrage de l'hôte Steam natif 10 s. Le démarrage du jeu jusqu'à la première image (34 à 37 s) et l'écran noir avant le titre (8 à 48 s selon le lancement) attendent EOS et le réseau : le fil principal cède en boucle dans `eossdk-win64-shipping.dll` et attend en `win32u`, avec une seule connexion HTTPS établie vers `api.epicgames.dev`. Rien d'imputable au moteur.

Écarté après mesure : relancer moins souvent le serveur de rendu (8 démarrages avant la première image, mais chacun prêt en moins de 0,5 s) ; un vDSO pour `clock_gettime` (2 millions d'appels par seconde sur le fil principal au menu, mais c'est sa boucle d'attente d'image : moins cher par appel, elle ne consommerait pas moins). Dans les menus, 60 i/s stables, GPU à 22 %, attente de barrière 0,8 ms. Le jeu lui-même n'a pas été mesuré : charger la sauvegarde la modifierait et Obelisk l'enverrait au Cloud. Rapport : `sailover/build/experiments/eldenring-linux-20261010/reports/20-perf-elden-ring.md`.

## ELDEN RING en jeu : deux gels du moteur trouvés et corrigés (2026-10-11, matin)

Contexte. Premières parties d'ELDEN RING sur le moteur Linux, chargées par l'utilisateur. Quatre gels définitifs (image fixe, souris capturée) : trois pendant le chargement du monde, un en jeu au moment d'un coup reçu.

Outil. Le vidage des fils (`FEXDARWIN_DUMP_DIR`) restait vide pour le jeu : il attendait lui-même le verrou en cause. Il écrit maintenant d'abord la pile native de chaque fil hôte, sans prendre aucun verrou (`af36ede`). Un guetteur hors dépôt déclenchait le vidage après 20 s sans image.

Gel 1, le verrou de la table des VMA (`efc8606`). MESURE (vidage du jeu figé) : 30 fils attendent `mm->vlock` en lecture, un `munmap` attend en écriture, aucun fil ne le tient, les 68 autres dorment dans ntsync. Reproduit hors du jeu par le témoin `w34_vlocksig` en moins d'une seconde, à chaque essai, et seulement quand les fils qui utilisent le verrou reçoivent des signaux. Cause : sur macOS 27.0.1, un `pthread_rwlock` dont les fils en attente reçoivent des signaux cesse de réveiller qui que ce soit (fait G1, programme natif `g1_rwlock_signals` : gel en 4 s, avec ou sans `SA_RESTART`), et le kick de fexdarwin est un signal. Correctif : un verrou d'un mot sur `os_sync_wait_on_address`. `w34` passe 3 fois sur 3, et le chemin des copies vers la mémoire invitée (celui de `clock_gettime`, 2 millions d'appels par seconde dans le jeu) va de 27 à 91-99 millions d'opérations en 20 s.

Gel 2, le mode bloquant des tubes passés par SCM_RIGHTS (`d924c5b`). MESURE (journal du jeu) : `wine client error:148: write: Resource temporarily unavailable`, puis `xinput_cs wait timed out ... blocked by 0148`. wineserver crée le tube de requêtes de chaque fil et en envoie l'extrémité d'écriture au client ; reçue, elle devenait non bloquante, car un tube LKD est toujours `O_NONBLOCK` sur l'hôte. Une requête qui trouvait le tube plein échouait, Wine terminait le fil, et ce fil tenait le verrou d'XInput (une vibration de la manette au coup reçu, déduit). Correctif : vers un autre processus de la famille, une extrémité de tube part avec un enregistrement portant ses drapeaux invités ; vers un programme hôte (wsd, serveur de rendu), rien ne change. Témoin `w35_scmpipe` : 614 628 EAGAIN avant, 3 passes sur 3 après.

Vérifications : tests LKD (`test_sig_futex` expire sur `waitv` comme avant ces changements), témoins w22 à w33 et w35, `w30_scmdev` échoue comme avant, Rocket League EAC 301 deux fois avec chaque moteur. En jeu : partie de 11:52 à 12:01 sans gel, quittée par l'utilisateur. Moteur installé à la main dans le runtime de dev : `fexdarwin-run` cae1f598, sauvegardes `*.87493e9b`, `*.9e4dd94b` et `*.e759f92c`.

Restent ouverts : `test_sig_futex` (`waitv`) et `w30_scmdev`, qui échouaient déjà ; une extrémité de tube envoyée à un processus invité du même processus hôte garde l'ancien chemin.

## wsd rend la souris quand le jeu ne montre plus d'images (2026-10-11, après-midi)

Contexte. Les gels de ce matin laissaient la souris capturée par le jeu figé, et l'utilisateur ne pouvait plus rien faire sur le Mac. La capture du mode relatif (`CGAssociateMouseAndMouseCursorPosition`) vaut pour toute la session, et seule une perte de focus la relâchait.

Correctif (`54464f7`). wsd note quand chaque fenêtre a affiché sa dernière image nouvelle, sur le plan de base comme sur le plan superposé, où ELDEN RING et Rocket League présentent. Une minuterie d'une seconde regarde les fenêtres qui tiennent le pointeur, en mode relatif ou avec un rectangle de clip. Par image, il n'y a que cet horodatage en plus. Après 5 s sans image, wsd réassocie le pointeur, réaffiche le curseur et cesse de ramener le pointeur dans le rectangle de clip. Les mouvements ne partent plus au jeu en deltas relatifs, sinon il les recevrait tous d'un coup à la reprise. La première image affichée ensuite remet le mode relatif si la fenêtre est active, et cache de nouveau le curseur si le client l'avait demandé. Le client n'est prévenu de rien. Revenir dans la fenêtre pendant le gel ne recapture pas. Rien n'est surveillé avant la première image de la fenêtre.

MESURE :
- Client de test hors dépôt contre un wsd privé, avec un clip plus grand que la fenêtre pour ne pas piéger le vrai pointeur. 7 s de clip avant toute image, aucun déclenchement. Images du plan de base arrêtées, relâche à 5,8 s ; images du plan superposé, reprise ; nouvel arrêt, relâche à 5,7 s, puis reprise.
- ELDEN RING en jeu, souris capturée, processus du jeu arrêté 20 s par `kill -STOP` : relâche à 5,5 s, pointeur libre et curseur visible (vu par l'utilisateur). Après `kill -CONT`, `frames shown again after 23.6 s, relative mouse resumed`, et la souris pilote de nouveau la caméra.
- Le chargement de la partie, juste avant, est resté 8,4 s sans image : souris relâchée, reprise à la première image. Un chargement muet de plus de 5 s montre donc le curseur jusqu'à ce qu'une image revienne.

wsd installé à la main dans le runtime de dev : `e16992cc`, sauvegarde `wsd.8e706708`.

## Les deux échecs « déjà présents » n'étaient pas des bugs du moteur (2026-10-11, après-midi)

Rectificatif de l'entrée du matin, qui les donnait pour ouverts.

- `test_sig_futex` bloquait sur le cas `waitv` (`827b74b`). Son échéance « dans 20 ms » venait de `CLOCK_MONOTONIC_RAW` de l'hôte, qui compte la veille, et partait comme `CLOCK_MONOTONIC` invité, que LKD fait correspondre à `CLOCK_UPTIME_RAW`, arrêté pendant la veille comme sous Linux. MESURE : 2 180 s d'écart sur ce Mac, donc une attente de 36 minutes. Le cas voisin prenait déjà l'horloge invitée. Après : 13 cas, 3 fois sur 3 ; suite LKD 44 sur 44.
- `w30_scmdev` échouait parce que je le lançais sans son répertoire synthétique (`FEXDARWIN_BIND="/lkdsyn/w=..."` et ses deux arguments, voir le README). Lancé ainsi, il passe avec le moteur d'avant comme avec celui d'après les correctifs. Le témoin dit maintenant quel descripteur l'enfant n'a pas pu ouvrir (`5ccfaf2`).

Aucune correction du moteur n'était nécessaire.

## ELDEN RING : mémoire, l'ensemble de résidence de d3dmetal-native coupé (2026-10-11, 15 h)

Mesure toutes les 5 s pendant deux parties d'une dizaine de minutes, même parcours (titre, chargement de la sauvegarde, jeu), Mac de 36 Go déjà chargé par d'autres apps (13 à 15 Go de swap au départ). Outils hors dépôt : `vm_stat`, `vm.swapusage`, `kern.memorystatus_vm_pressure_level`, IOAccelerator « Alloc system memory », `top` (empreinte et mémoire compressée par processus).

MESURE, partie A (ensemble de résidence, le défaut) : les trois tampons UPLOAD de 4 Gio arrivent à l'écran titre ; mémoire câblée 13,3 Go en moyenne avec des pics à 24,4 Go pendant les chargements ; mémoire GPU allouée 26,3 Go en moyenne ; 4 alertes de pression mémoire ; 3,3 Go envoyés en swap ; 3,9 Go compressés sur les 6 Go du jeu ; 48 i/s.

MESURE, partie B (`DMN_RESIDENCY_SET=0`, chaque tampon de commandes déclare ses ressources) : câblée 12,6 Go en moyenne, 14,0 au plus ; mémoire GPU 14,6 Go (−11,7, les trois tampons) ; aucune alerte ; rien envoyé en swap ; 12 Mo compressés pour le jeu ; 49 i/s ; même part d'images retenues plus de 100 ms (4,7 et 4,8 %) et trois de plus d'une seconde dans les deux. Aucune erreur de rendu vue par l'utilisateur. Réserve : B partait avec 10 Go libres, A avec 0,7 ; la baisse de mémoire GPU et la fin des pics câblés viennent du mode, l'écart de swap et de compression en partie du point de départ.

DÉDUIT : les 13 Go câblés en continu sont l'ensemble de travail du jeu (sa mémoire vidéo sur PC), pas du gaspillage. Changement : le lanceur d'Obelisk passe `DMN_RESIDENCY_SET=0` au serveur de rendu D3DMetal (sailover `0dad6ee`).

## ELDEN RING : où passent les images, et deux essais écartés (2026-10-11, 16 h)

MESURE, en 2560×1440, trois parties de 3 à 6 minutes de jeu, profil par fil (`FEXDARWIN_PROFILE=1`) et charge du GPU toutes les 2 s :

- 32 à 35 i/s ; le GPU est occupé à 33 à 36 % (57 % au plus) et l'attente de barrière au moment de présenter reste sous 1,5 ms : le GPU n'est pas la limite.
- Le jeu prend 6 à 7 cœurs. Les 14 fils `GXWorker` en prennent 5,6 à tourner entre deux tâches : par fil et par seconde, environ 37 000 `sched_yield` (4 µs chacun en jeu) et 74 000 `getrusage(RUSAGE_THREAD)` (1,4 µs), plus des attentes ntsync longues (585 µs en moyenne).
- Le fil principal (`eldenring.exe`) n'utilise que 52 à 56 % d'un cœur : il attend. Par seconde : 20 000 à 26 600 appels ntsync (6,4 µs en moyenne), 3 600 attentes futex (45 à 60 µs), 2 500 à 3 200 lectures de réponses de wineserver (48 µs l'aller-retour), 386 000 `clock_gettime` (0,08 µs l'appel complet d'après `w36_syncpp` : un vDSO ne vaudrait pas la peine).

Écarté, cadence coupée (`guest NPT_WSD_PACING=off`) : 32 i/s contre 35 avec la cadence ; les 2,8 ms d'attente par image ne sont pas sur le chemin critique.

Écarté, attente active de 5 µs avant de dormir dans `fx_sleep` (futex et ntsync). Témoin `w36_syncpp` : l'aller-retour ntsync passe de 8,4 à 3,1 µs, le futex privé de 8,0 à 0,16 µs ; tests LKD, témoins et EAC de Rocket League sans régression. En jeu : 32 i/s, 6,7 cœurs au lieu de 6,0 ; les attentes futex du fil principal raccourcissent (60 à 45 µs) mais ses appels ntsync s'allongent (6,4 à 8,0 µs), l'attente active s'ajoutant à celles qui dorment quand même. Pas de gain : le changement n'est pas gardé.

Reste à mesurer : quelles requêtes le fil principal envoie à wineserver 2 500 fois par seconde.

## Rocket League figé après un changement d'espace (2026-10-22, issue Obelisk #14)

Contexte. Un joueur (JAWE-B15S, M4, macOS 27.2) et l'utilisateur : passer sur un autre espace, cliquer une autre fenêtre puis revenir laisse Rocket League figé pour de bon, seulement sur le moteur Linux. Dans le log de l'utilisateur (16:56), nptunix s'arrête à 411 presents alors que Neptune continue d'envoyer des tampons de sommets : le jeu dessine encore mais ne présente plus.

Cause, en deux temps :
- wsd. Quand macOS ne montre plus la fenêtre (recouverte, sur un autre espace), il ne reprend plus ses drawables, et `[CAMetalLayer nextDrawable]` bloque la file principale jusqu'à 1 s (`allowsNextDrawableTimeout`). Pendant ce temps aucun message n'est lu et aucun EV_RELEASE ne part. La porte de réutilisation de Neptune (100 ms par image) expire plusieurs fois de suite et, au bout de 4, `Present` rend `DXGI_STATUS_OCCLUDED`.
- Neptune. `Present(DXGI_PRESENT_TEST)` rendait OCCLUDED tant que la série valait 4, et seule la porte d'un vrai Present la remettait à zéro. Rocket League (`RocketLeague.exe`, 0x140d695b0), une fois OCCLUDED reçu, n'appelle plus que des Present de test jusqu'à ce qu'un test réponde autre chose : il ne présentait plus jamais.

MESURE (client de test hors dépôt qui imite Neptune : 3 images, porte de 100 ms, une image toutes les 4 ms, PING pour la file principale) :
- Fenêtre recouverte par une seconde fenêtre wsd opaque, ancien wsd : PING au plus 880 ms, 9 expirations de la porte d'affilée. `nextDrawable` a duré 997 ms, puis 1002 ms à un autre essai, au moment où la fenêtre disparaît, et environ 8 ms ensuite tant qu'elle reste cachée. La notification d'occlusion est arrivée avant ce blocage au premier essai, après au second.
- Fenêtre déplacée hors de l'écran : pas de blocage (le display link s'arrête), et le mailbox continue de rendre les images remplacées.
- À l'écran, `nextDrawable` attend environ 8 ms à chaque tick à 240 Hz, donc la file principale est occupée la plupart du temps. Non traité.

Correctifs :
- wsd (`783b3c9`). Une fenêtre sans `NSWindowOcclusionStateVisible` garde son image en attente sans demander de drawable ; une image plus récente la remplace et la rend, comme avant. Une ligne de log à chaque changement d'occlusion. Le blocage d'environ 1 s au moment où la fenêtre disparaît reste possible quand la notification arrive après lui ; seul un `nextDrawable` hors de la file principale l'enlèverait.
- Neptune (`bce5c33`, dépôt neptune). Un Present de test regarde de nouveau l'image que prendra le prochain Present : si elle est rendue, la série est effacée et le test répond S_OK. Swapchains D3D11 et D3D12.

Vérifications : `wsd_overlay` et `wsd_client` passent. Avec le nouveau wsd, la fenêtre hors écran ne donne plus aucune expiration. L'utilisateur a refait le scénario sur Rocket League et Fall Guys : le jeu reprend. Installés à la main dans le runtime de dev : wsd `6e6d4a4b` (sauvegarde `wsd.9cb05490`) et `windows/{d3d11,dxgi,d3d12}.dll` (sauvegardes `*.dll.20261015`).

## Dead by Daylight sur le moteur Linux : limite de fils, une commande Neptune trop courte et une swapchain 10 bits (2026-10-22)

Contexte. Dead by Daylight (Epic, Unreal 5, Easy Anti-Cheat), moteur Linux choisi dans les réglages, DXMT. Easy Anti-Cheat charge son module Linux (`anticheatlauncher.log` : « Easy Anti-Cheat successfully loaded in-game »). Trois blocages l'un après l'autre, chacun trouvé dans le log du jeu.

1. Crash au démarrage. MESURE : `CrashContext.runtime-xml` donne `EXCEPTION_ACCESS_VIOLATION reading address 0x0` à 0 s ; avec `+seh` la faute est dans le jeu (rip 0x14140ecc8, rax 0), juste après `lkd: clone: 300 threads in one process, refusing more (-EAGAIN)`. INFERE : le jeu lit le handle nul du fil refusé. Correctif `4095e5d` : 512 fils par processus. 512 est la taille de `SIG_WAKESET_MAX` (une sortie de groupe réveille encore chaque fil) ; à environ 280 Mio d'espace d'adressage hôte par fil FEX, 512 fils tiennent dans les 576 Gio sous la fenêtre invitée. La règle des tests ne change pas (jamais plus de 300 fils).
2. Écran noir en plein écran. MESURE : `neptune: ring fatal error`, et côté serveur de rendu `failed to peek 8 bytes` puis `dispatch error for cmd_type=0xff004c1b`, soit `ID3D11DeviceContext::OMSetRenderTargetsAndUnorderedAccessViews`. LU : la taille calculée comptait 8 octets par handle même sans tableau ; avec un compte KEEP (0xFFFFFFFF) la somme tronquée à 32 bits valait exactement 8 octets de moins que ce que l'encodeur écrit, et l'hôte lit les commandes bout à bout sans regarder `cmd_size`. Correctif neptune `e34ced6`.
3. Couleurs en bandes, rouge devenu bleu. MESURE : swapchain `fmt=24` (`DXGI_FORMAT_R10G10B10A2_UNORM`) et `shared texture: unmapped DXGI format 24, assuming BGRA8`. Correctifs : neptune `acda52d` (format virgl 8), voile-wine `8f86ea35894` (nptunix, format wsd 3) et wsd `e1adcf7` (`MTLPixelFormatRGB10A2Unorm`, échantillonné vers le drawable BGRA8).

Écarté : installer le VC++ de l'installateur du jeu (14.29) dans le préfixe Linux. Le `msvcp140` de Wine annonce 14.51 et le jeu est lié avec 14.44 ; un `msvcp140` natif plus ancien que 14.40 fait justement planter `_Mtx_lock` sur un pointeur nul.

Vérification : l'utilisateur a lancé le jeu, menus aux bonnes couleurs. Reste : le jeu saccade, non mesuré. Installés à la main dans le runtime de dev : `fexdarwin-run` `f8008d33` (sauvegarde `.2a763995`), wsd `e9530034` (sauvegarde `wsd.e293795e`), `nptunix.so` `772e9151` (sauvegarde `.19f082eb`), `windows/{d3d11,dxgi,d3d12}.dll` (sauvegardes `*.dll.20261022`).

## Plein écran : la fenêtre du jeu restait devant les autres apps après Cmd+Tab (2026-10-23)

Contexte. Rocket League en plein écran sur le moteur Linux : après Cmd+Tab, l'autre app devient active et prend le clavier et la souris, mais l'image du jeu reste devant elle. Le navigateur ouvert par un lien du jeu (`OPEN_URL`, `9d09a1d`) restait caché de la même façon.

Cause. LU : `setFullscreenMode:` mettait la fenêtre au niveau `NSMainMenuWindowLevel + 1` (25) pour toute la durée du plein écran, et un niveau ne change pas quand l'app perd la main. Les options de présentation (Dock et barre de menus masqués) n'y sont pour rien : macOS ne les applique que tant que l'app est active. Le driver Mac de Wine fait l'inverse : `-[WineWindow minimumLevelForActive:]` ne monte une fenêtre plein écran qu'avec l'app active, et `adjustWindowLevels:NO` la redescend dans `applicationWillResignActive:`. Même famille : une fenêtre montrée par-dessus la fenêtre plein écran (dialogue, boîte de crash) restait au niveau normal, donc dessous, comme une fenêtre topmost (niveau 3).

MESURE (client de test hors dépôt : trois fenêtres wsd, plein écran puis topmost puis dialogue, un clic et des Cmd+Tab postés, couches lues dans `CGWindowListCopyWindowInfo`) :
- Ancien wsd : après Cmd+Tab, le jeu reste à 25 devant l'app active et le Dock. Le dialogue est à 0 et le topmost à 3, tous deux sous le jeu. Même sans avoir jamais été active (activation refusée), la fenêtre plein écran couvrait Safari.
- Nouveau wsd : avec wsd active, topmost 26, dialogue 25, jeu 25, le dialogue devant le jeu. Après Cmd+Tab, le jeu et le dialogue passent à 0 derrière l'autre app et le Dock revient. Au retour, on retrouve le même ordre. Une fois la fenêtre du jeu détruite, le dialogue redescend à 0.

Correctif wsd (`f41caa8`). `adjust_levels` reprend `adjustWindowLevels:` : de l'arrière vers l'avant, dans l'ordre du serveur de fenêtres (Space actif, topmost en tête), chaque fenêtre prend le niveau que son état demande et au moins celui des fenêtres derrière elle, et les topmost restent au-dessus des autres. La passe tourne quand wsd perd ou reprend la main, quand le Space actif change, et quand une fenêtre s'affiche, se cache, est détruite, ou entre ou sort du plein écran. Une fenêtre qui s'affiche prend d'abord le niveau de celle de devant, pour être placée devant elle.

Hors de wsd : le driver (`wineobelisk.drv`) n'envoie pas l'ordre des fenêtres (`insert_after` ignoré) et ne lit `WS_EX_TOPMOST` qu'à la création.

Vérification : l'utilisateur a testé Rocket League en plein écran (Cmd+Tab, lien ouvert depuis le jeu, retour au jeu). Installé à la main dans le runtime de dev : wsd `fdb3ba42` dans `v26.3.0-18` (sauvegarde `wsd.v26.3.0-18`).

## Ce que le driver Mac de Wine faisait et que wsd n'avait pas (2026-10-23)

Contexte. Comparaison de `winemac.drv` (celui du moteur Windows) avec wsd et `wineobelisk.drv` : notre driver avait 10 points d'entrée, winemac une cinquantaine. Repris en un lot, sauf les gros chantiers et le moins prioritaire.

MESURE. Un programme de test (`wsdtest.exe`, hors dépôt) lancé par le `launch.sh` d'Obelisk sur un clone APFS du runtime ; côté Mac, un outil poste touches et clics puis lit les couches du serveur de fenêtres, le pointeur, `NSCursor.currentSystemCursor` et `pmset -g assertions`. Avant → après :
- pavé numérique 5 : rien → `VK_NUMPAD5`, scan 0x4c. Touche `@ #` d'un clavier ISO : rien → scan 0x29 ; touche à droite de Maj gauche : scan 0x29 → 0x56.
- Caps Lock basculé quatre fois : état Wine 1,1,1,1,0,0,0,0 → 1,0,1,0 (wineserver bascule à chaque appui, `set_input_key_state`).
- `SetCursorPos(300,250)` : pointeur immobile → en 300,250.
- curseur couleur 32×32 du jeu, `IDC_IBEAM` : flèche du Mac → l'image du jeu avec son point chaud, l'I-beam du Mac.
- popup `WS_EX_NOACTIVATE` montré en `SW_SHOWNOACTIVATE` : devenait fenêtre clé, et l'`EV_FOCUS` qui suivait l'activait aussi dans Wine → laisse le premier plan au jeu.
- `HWND_TOPMOST` après la création : ignoré → niveau 3, puis 0 avec `HWND_NOTOPMOST`.
- `SetForegroundWindow` du jeu vers une fenêtre de derrière : le Mac ne suivait pas → elle passe devant.
- `SPI_SETSCREENSAVEACTIVE` à FALSE puis TRUE : rien → assertion `PreventUserIdleDisplaySleep` prise puis rendue.

Correctifs :
- wsd (`0083ffe`). Caps Lock en appui et relâche à chaque changement ; `WSD_KEY_ISO` ; `CURSOR_WARP`, `CURSOR_DEFINE`, `CURSOR_SET` ; `WINDOW_SHOW` 2, `WSD_STYLE_NOACTIVATE`, `WINDOW_FOCUS`, `TOPMOST` appliqué ; `EV_DISPLAY` ; `SCREEN_SAVER` ; déconnexion, redémarrage ou arrêt du Mac en `NSTerminateLater` (avant : annulés tant qu'un jeu tournait) ; App Nap coupé.
- voile-wine `a3ae9c1e336` (`wineobelisk.drv`). Curseurs (table des curseurs système de winemac, sinon l'image), `SetCursorPos`, `ActivateWindow`, montrer sans activer, `WS_EX_TOPMOST` suivi, `SystemParametersInfo`, `EV_DISPLAY` (le processus du bureau appelle `DisplayModeChanged`), table de touches complète de winemac avec l'échange ISO. Bug trouvé au test : `NtUserGetIconInfo` donne les longueurs en caractères, pas en octets ; l'I-beam n'était pas reconnu.
- obelisk-input (`ded548b`). `IOPMAssertionDeclareUserActivity` sur les boutons et les sticks au-delà d'un tiers, au plus toutes les 5 s.

Régression trouvée par l'utilisateur, corrigée avant le commit. Un jeu en plein écran exclusif D3D (Sugar, DXMT) ouvrait un lien : Safari devenait l'app active, mais restait sous l'image du jeu, et Discord aussi. LU : le swapchain de Neptune met la fenêtre en `HWND_TOPMOST` (`npt_swapchain.c:541`, comme DXMT et Windows), et wsd faisait flotter toute fenêtre topmost, même inactive. Règle reprise de winemac (`TOPMOST_FLOAT_INACTIVE_NONFULLSCREEN`, son réglage par défaut) : une fenêtre topmost plein écran ne flotte plus quand wsd n'est pas actif. MESURE : après Cmd+Tab vers Teams, fenêtre plein écran topmost au niveau 3 devant Teams, puis au niveau 0 derrière avec la règle.

Non testé : App Nap, la déconnexion, la manette, un changement d'écran. Gros chantiers laissés de côté : disposition du clavier (AZERTY, `ToUnicodeEx`), presse-papiers, ordre des fenêtres (`insert_after`) et multi-écran. Reste aussi : Wine n'apprend pas que le jeu passe en arrière-plan (winemac met le premier plan sur le bureau, le jeu reçoit `WM_ACTIVATEAPP(FALSE)`) ; non fait, à décider.

Installés à la main dans le runtime de dev `v26.3.0-18` : `engine/wsd` `bddd403a`, `engine/obelisk-input` `9ed3513b`, `wineobelisk.so` `051329fa` (originaux en `*.v26.3.0-18`).

## Le jeu passe en arrière-plan quand une autre app passe devant (2026-10-23)

Contexte. Le driver ignorait la perte de focus : pour Wine, le jeu restait au premier plan quelle que soit l'app où se trouvait l'utilisateur. Il ne recevait jamais `WM_ACTIVATEAPP(FALSE)` et ne pouvait pas se mettre en pause, couper son son ou se réduire comme sous Windows. winemac le fait (`APP_DEACTIVATED`, `macdrv_app_deactivated` : le premier plan passe au bureau).

MESURE (programme de test hors dépôt par le `launch.sh` d'Obelisk ; retour au jeu en rouvrant l'app comme le Dock, le Cmd+Tab automatisé suivant l'ordre des apps récentes) :
- avant : après Cmd+Tab, premier plan toujours au jeu, aucun `WM_ACTIVATEAPP` ; un jeu qui se réduit à la désactivation ne se réduisait jamais.
- après : départ → `WM_ACTIVATEAPP` off, premier plan au bureau ; retour → `WM_ACTIVATEAPP` on, premier plan au jeu, fenêtre plein écran au niveau 25 ; le jeu qui se réduit disparaît, puis revient restauré avec le premier plan.

Trouvé en route. LU : `set_foreground_window` du serveur (`queue.c:3831`) refuse le premier plan à un processus qui ne l'a pas et dont la file n'a pas d'entrée utilisateur plus récente. Le `NtUserSetForegroundWindow` du thread d'événements ne marchait que parce que le jeu n'avait jamais perdu le premier plan ; une fois le bureau au premier plan, le retour était refusé. Comme `macdrv_window_got_focus`, le focus passe maintenant par un message à la fenêtre, dont le thread prend le premier plan avec `NtUserSetForegroundWindowInternal`. Une fenêtre encore réduite le prend après le `SC_RESTORE` posté avant.

Correctifs :
- wsd (`8d8ab8d`). `EV_APP_DEACTIVATED` à chaque client quand wsd n'est plus actif ; une fenêtre sans bouton de réduction réduite par le client est marquée, et revient avec `WINDOW_MINIMIZE` 0 ; au retour (activation ou clic dans le Dock) sans fenêtre visible, la plus grande réduite revient (`-unminimizeWindowIfNoneVisible` de winemac).
- voile-wine `29abf1029b0`. `EV_APP_DEACTIVATED` : relâche du curseur et premier plan au bureau dans le processus qui l'a ; focus par `WM_OBELISK_TAKE_FOCUS` ; toute fenêtre réduite passe par `WINDOW_MINIMIZE`.

Vérification : l'utilisateur a essayé ses jeux. Installés à la main dans le runtime de dev `v26.3.0-18` : `engine/wsd` `0801e582`, `wineobelisk.so` `da9930a6`.
