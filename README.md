# OpenSolECU

Firmware autonome ESP-IDF pour ESP32-C6, avec interface Web locale, Wi-Fi AP + STA,
acquisition APsystems expérimentale et sniffer IEEE 802.15.4 passif.

**Le premier démarrage utilise SNIFFER.** Deux profils matériels sont disponibles :
un profil d'écoute verrouillé et un profil où NORMAL est sélectionnable. Aucune
association avec les micro-onduleurs n'est nécessaire pour observer une ECU-C.
La compilation ne constitue pas une validation de remplacement d'une ECU APsystems.
Voir [le protocole et ses incertitudes](docs/protocol.md).

## Matériel

- ESP32-C6 ; 8 Mo recommandés ; variante 4 Mo fournie et compatible OTA.
- Alimentation USB stable. Aucun CC2530/CC2531, aucune carte SD, aucun cloud.
- Le C6 partage le temps d'antenne 2,4 GHz entre Wi-Fi et IEEE 802.15.4.
- `CONFIG_ESP_COEX_SW_COEXIST_ENABLE` doit rester activé (`sdkconfig.defaults`) : sans lui,
  le sniffer IEEE 802.15.4 ne reçoit plus aucune trame (confirmé face aux exemples officiels
  ESP-IDF `wifi/scan` et `wifi/getting_started/softAP` sur le même matériel). Mais l'activer
  seul ne suffit pas : sans l'appel explicite `esp_coex_wifi_i154_enable()` (fait dans
  `wifi_manager.cpp`, après `esp_wifi_start()`, une fois le radio 802.15.4 déjà initialisé
  par `snifferBegin()`), la coexistence Wi-Fi/802.15.4 reste à moitié configurée et aucun
  client ne peut s'associer à l'AP ni la STA rejoindre un réseau. Les deux options sont
  nécessaires ensemble ; ne retirer ni l'une ni l'autre sans revalider association Wi-Fi
  ET réception 802.15.4 simultanément.
- Certaines cartes ESP32-C6FH4 testées ont une réception radio défaillante dès la sortie
  d'usine (scan Wi-Fi renvoyant 0 réseau même avec un firmware ESP-IDF vierge) : tester
  avec l'exemple `wifi/scan` avant de blâmer le firmware applicatif.

## Construire

ESP-IDF **v5.5.1** et sa chaîne RISC-V sont utilisés. Le composant Espressif mDNS
1.8.2 est inclus dans `components/mdns` ; les builds ne téléchargent pas de dépendance.

Dans un terminal ESP-IDF :

```sh
idf.py set-target esp32c6
idf.py build
idf.py fullclean
idf.py set-target esp32c6
idf.py build
```

Le build par défaut utilise `partitions.csv`, Flash 8 Mo. Pour un **C6 4 Mo en écoute
passive verrouillée** (répertoire et configuration distincts) :

```sh
idf.py -B build-hardware -D SDKCONFIG=sdkconfig.hardware -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.4mb;sdkconfig.sniffer" set-target esp32c6
idf.py -B build-hardware build
```

Sous PowerShell, un raccourci charge l'environnement déjà installé :

```powershell
./tools/idf.ps1 build
./tools/idf.ps1 -IdfArgs @('-B','build-hardware','-D','SDKCONFIG=sdkconfig.hardware','-D','SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.4mb;sdkconfig.sniffer','set-target','esp32c6','build')
```

Pour une version 4 Mo avec NORMAL sélectionnable :

```sh
idf.py -B build-hardware-unlocked -D SDKCONFIG=sdkconfig.hardware-unlocked -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.4mb" set-target esp32c6
idf.py -B build-hardware-unlocked build
```

Le profil omet volontairement `sdkconfig.sniffer`. Ne pas réutiliser le `sdkconfig`
d'une variante différente : les valeurs existantes priment sur les defaults.

## Flasher

Carte 4 Mo, build matériel passif :

```sh
idf.py -B build-hardware -p COM3 -b 115200 flash monitor
```

Commande explicite équivalente (depuis la racine) :

```sh
python -m esptool --chip esp32c6 --port COM3 --baud 115200 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m 0x0 build-hardware/bootloader/bootloader.bin 0x8000 build-hardware/partition_table/partition-table.bin 0xf000 build-hardware/ota_data_initial.bin 0x20000 build-hardware/opensolecu.bin
```

Le flash initial écrit le bootloader, la table de partitions, otadata et app0.
Pour les mises à jour ultérieures, préférer l'OTA afin de conserver l'emplacement
actif et les données. Ne pas changer de table de partitions sans sauvegarde.

## Version et publication

Le numéro de version (`esp_app_desc_t.version`, exposé par `/api/system`) vient de
`version.txt` à la racine, lu par `CMakeLists.txt` avant `project()`. En local ce
fichier reste à `0.0.0-dev` ; `.github/workflows/release.yml` l'écrase avec un
calendaire `AAAA.M.PATCH` (`PATCH` = numéro de run du workflow, jamais réutilisé)
à chaque push sur `main`, build la variante 4 Mo (`sdkconfig.defaults` +
`sdkconfig.4mb`) et publie une GitHub Release avec `opensolecu-4mb.bin` en asset.
Le firmware compare sa propre version à celle de la dernière release via l'API
GitHub (voir [docs/api.md](docs/api.md#mises-à-jour)) — garder le nom de l'asset
synchronisé entre le workflow et `components/updater/updater.cpp` si l'un des
deux change.

## Premier démarrage et deux accès simultanés

Le mot de passe Wi-Fi AP initial est fixe (`OpenSolECU26`). Le port USB série
l'affiche au premier démarrage. L'interface Web et ses API ne demandent aucun
mot de passe administrateur : les commandes sont accessibles depuis le réseau local.

1. Se connecter à **OpenSolECU-XXXX**.
2. Ouvrir **http://192.168.4.1**.
3. Dans Réglages, définir le nom, le fuseau et éventuellement le Wi-Fi domestique.
4. Enregistrer. Le module redémarre et conserve son point d'accès par défaut.

Depuis le LAN : **http://opensolecu.local**, ou l'IP DHCP indiquée dans Système.
Le mode initial est `WIFI_MODE_APSTA`, avec reconnexion STA temporisée. Si l'AP
permanent est désactivé, il est réactivé en secours à la déconnexion STA.
Le scan Wi-Fi ne voit que les réseaux 2,4 GHz supportés par le C6.
Le serveur écoute les deux interfaces. Il n'y a ni NAT ni dépendance Internet
pour consulter/configurer le module. L'adresse directe est à ouvrir manuellement
si le téléphone ne propose pas de page de connexion.

L'heure vient de SNTP ; après synchronisation elle continue sans Internet.
Après un démarrage sans heure fiable, les enregistrements datés restent suspendus.
Europe/Paris est traduit en règle POSIX CET/CEST, DST compris. UTC et des règles
`POSIX:<règle TZ>` personnalisées sont acceptés.

## Sniffer sur installation ECU-C existante

Laisser l'ECU-C alimentée et tous les micro-onduleurs associés. Ouvrir Radio sniffer, scanner
11–26 pendant 3 à 10 secondes par canal, puis sélectionner le canal observé.
Le scan ne transmet rien et ne sélectionne pas automatiquement le canal le plus actif.
Capturer 30 s, 1 min, 5 min ou 10 min et exporter le PCAPNG pour Wireshark.
Ne pas ouvrir Appairer pour cette procédure. [Guide détaillé](docs/sniffer-analysis.md).

## Acquisition APsystems en mode NORMAL

Sur un build non verrouillé, choisir NORMAL, le canal, PAN, identifiant ECU et
numéro de série APsystems de 12 caractères hexadécimaux. L'adresse courte est saisie
**en décimal** (exemple `0x5471` = `21617`). Le bouton Appairer n'est accessible
qu'en NORMAL et change l'association de l'onduleur ; il ne doit pas être utilisé
tant que l'onduleur est associé à une autre ECU.
Le protocole actif dérive de captures publiques et doit encore être confronté
aux réponses radio réelles, particulièrement leur longueur et leur encapsulation.
Les valeurs inconnues sont `null`, jamais des valeurs de démonstration.

Dans **Réglages → Onduleurs et panneaux**, ajouter chaque onduleur par numéro de série,
nom facultatif, adresse courte et modèle `AUTO`, `DS3`, `YC600` ou `QS1`. Jusqu'à
16 onduleurs et 64 entrées PV sont suivis séparément.
L'ordonnanceur NORMAL parcourt la liste ; le bouton d'appairage concerne son premier
onduleur uniquement. Le fonctionnement actif reste expérimental, sans validation d'appairage
sur une installation de production. En SNIFFER, les réponses APS complètes et vérifiées alimentent
chaque onduleur ; les valeurs manquantes restent inconnues. [Détails multi-PV](docs/multi-pv.md).

## Web, données et maintenance

- Dashboard : une carte par onduleur et ses deux ou quatre PV, tension/fréquence AC, température,
  dernière mesure. Une puissance partielle est explicitement indiquée.
- Graphiques : sélection de l'onduleur, navigation par jour via un calendrier ; repli sur le
  résumé consolidé (énergie du jour, pic) quand le détail minute/quart d'heure n'est plus disponible.
- Statistiques : énergie mesurée, pic, moyenne des journées terminées, meilleure journée.
- Export CSV : UTF-8 avec BOM, séparateur `;`, unités dans l'en-tête ; `from/to`
  sont des secondes Unix UTC. Résolutions `minute`, `15min`, `day`.
- Sauvegarde JSON : format, configuration expurgée, historique et statistiques.
  Aucun mot de passe ni hash d'administration. La restauration automatique d'une
  sauvegarde JSON n'est pas exposée en V1.
- OTA manuel : Réglages → Mise à jour manuelle, charger `opensolecu.bin` ESP32-C6.
  Deux partitions OTA, validation ESP-IDF, rollback si démarrage non validé.
- Mise à jour automatique : le firmware vérifie `github.com/rafal83/OpenSolECU`
  (releases) toutes les 6 h et affiche un bandeau + un bouton « Installer » dès
  qu'une version plus récente est disponible ; aucun flash sans clic explicite.
  Versions calendaires `AAAA.M.PATCH` (façon Home Assistant/ESPHome), publiées
  automatiquement par `.github/workflows/release.yml` à chaque merge sur `main`.
- Journal : `/debug`, RAM seulement ; choisir TRACE pour les trames actives.
- Sniffer : `/debug/sniffer`, 256 trames en RAM, navigateur limité à 200 trames.
- Aucun CDN, police distante, MQTT ou Home Assistant. HTML/CSS/JS compressés
  de façon déterministe au build par `tools/build_webui.py`.

La configuration, l'OTA et les commandes sont accessibles sans session ni token
à toute personne connectée au réseau du module. Les mots de passe Wi-Fi ne sont
pas renvoyés par les API de configuration ou les sauvegardes. Les données NVS
ne sont pas chiffrées en Flash.

## Mode mock et tests

```sh
idf.py -B build-mock -D SDKCONFIG=sdkconfig.mock-build -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.mock" set-target esp32c6
idf.py -B build-mock build
```

Le mock est explicitement signalé dans les API, l'UI et les enregistrements. Il
inclut une interruption périodique pour vérifier online/offline. Pour le faire
tourner, choisir NORMAL dans un build mock. SNIFFER conserve la vraie réception
radio et n'injecte pas de fausses captures.

Tests hôte sous Windows, avec Visual Studio C++ Build Tools :

```powershell
./tools/test.ps1
node --check webui/app.js
```

Les tests compilent et exécutent le véritable noyau C++, y compris le décodeur
DS3, YC600 et QS1 avec des réponses publiées, la construction des trames comparée aux captures,
l'intégration, minuit UTC, les anneaux, les coupures NOR, le CRC, la sérialisation,
le parsing MAC/NWK/APS, le blocage TX et la logique AP/STA. Un lecteur Python
indépendant vérifie la sortie PCAPNG. Les tests hôte utilisent UTC car la libc
Windows ne gère pas les règles POSIX DST comme newlib sur ESP32.

Console USB locale : `status`, `sniffer`, `sniffer {"action":"scan","dwell":3}`,
`sniffer {"action":"channel","channel":16}`, `tx-guard-test`, `wifi-scan`,
`wifi {"ssid":"...","password":"..."}` et `time <secondes Unix UTC>`.
Les commandes ne sont pas échoées ; la commande Wi-Fi redémarre le module.

## Documentation

[Architecture](docs/architecture.md) · [Protocole](docs/protocol.md) ·
[Stockage](docs/storage.md) · [Sniffer](docs/sniffer-analysis.md) · [API](docs/api.md).
