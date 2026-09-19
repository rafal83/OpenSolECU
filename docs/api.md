# API HTTP locale

Les nombres inconnus sont JSON `null`. Les timestamps sont des secondes UTC,
sauf `timestampUs`/`monotonicUs` du sniffer. Pas de CORS universel.

| Méthode et chemin | Réponse |
|---|---|
| GET /api/status, /api/system | Firmware, heap, horloge, Wi-Fi, radio, Flash |
| GET /api/live | `inverters[]`, deux ou quatre canaux PV, total complet ou partiel |
| GET /api/history?date=YYYYMMDD&serial=... | `{date,resolution,records:[...]}` pour un jour civil |
| GET /api/stats?serial=... | Agrégats Wh, pic et dates de cet onduleur |
| GET /api/config | Configuration sans secrets |
| POST /api/config | Validation puis NVS ; reboot après réponse |
| GET /api/wifi | AP et STA séparément |
| GET /api/wifi/scan | Lancement scan asynchrone, résultats en cache |
| POST /api/wifi | Configuration réseau puis reboot |
| GET /api/events | SSE `event: live` toutes les 5 s |
| GET /api/export.csv?from=0&to=...&resolution=minute | CSV ; `15min` et `day` également |
| GET /api/backup | JSON format 1, configuration expurgée, stats et trois anneaux |
| POST /api/ota | Corps brut binaire ESP-IDF, pas multipart |
| GET /api/update | État de la dernière vérification GitHub Releases |
| POST /api/update/check | Vérifie GitHub Releases maintenant (bloquant) |
| POST /api/update/install | Télécharge et flashe la dernière release ; reboot |
| GET /api/debug | Anneau de log RAM |
| POST /api/pair | Association explicite NORMAL uniquement |
| GET /api/sniffer?after=ID | Métadonnées, scan, appareils, lot de trames |
| GET /api/sniffer/events?after=ID | SSE `event: frames`, lots de 8, 1 Hz |
| POST /api/sniffer/control | Commande passive en queue |
| GET /api/sniffer/export?format=pcapng\|jsonl | Copie de la capture encore en RAM |

## Configuration sans authentification

Les lectures et mutations, y compris l'OTA, ne demandent aucun mot de passe ni
en-tête `Authorization`. `/api/config` annonce `authenticationRequired:false`.
L'ancien endpoint `POST /api/login` est supprimé (404). Les validations JSON,
la vérification des images OTA et le verrouillage SNIFFER restent actifs.

La configuration admet `installation`, `ssid`, `password`, `apSsid`, `apPassword`,
`apEnabled`, `timezone`, `serial`, `ecu`, `inverterId`, `pan`, `channel`,
`pollSeconds`, `logLevel`, `mode` (`NORMAL` ou `SNIFFER`).
Elle admet aussi `inverters`, tableau de 0 à 16 objets `{serial,name,address,model}`.
Le numéro de série comporte 12 caractères hexadécimaux ; les doublons sont
refusés après normalisation en majuscules. Le nom est facultatif (32 octets UTF-8
maximum), l'adresse est un entier décimal de 0 à 65527 (0 : inconnue). `model`
vaut `AUTO`, `DS3`, `YC600` ou `QS1` ; AUTO détecte le format de la réponse.
Les champs `serial` et `inverterId` reflètent le premier onduleur de la liste.
Omettre un mot de passe pour le conserver ; `password:""` configure un STA ouvert.
Le build passif verrouillé refuse NORMAL.

`inverters[]` dans `/api/live` contient `serial`, `name`, `address`, `configured`,
`model`, `configuredModel`, `channelCount`, `channels[]`, `status`, `online`,
`simulated`, `messages`, `last_seen`, les mesures AC et les énergies. Chaque objet
de `channels[]` contient `power`, `voltage` et `current`. `status` vaut `no_data`,
`awaiting_second_sample`, `measured` ou `stale`. La puissance nécessite deux
réponses cohérentes du même onduleur. Les champs `pv1` et `pv2` restent présents
temporairement pour les clients existants.
À la racine, `totalPower` et `todayWh` restent `null` si un onduleur manque ;
`measuredPower`, `measuredTodayWh`, `knownPowerCount`, `inverterCount` et `pvCount`
décrivent les données disponibles. Les autres anciens champs de mesure à la
racine concernent le premier onduleur ; utiliser `inverters[]`.
Les compteurs `decodeDropped` et `decodeRejected` diagnostiquent le traitement passif.

Historique/statistiques : sans paramètre `serial`, le premier onduleur est sélectionné.
Le CSV accepte `serial=...` ; par défaut il exporte tout et ajoute une colonne
`serial`. Les records JSON possèdent aussi `channelCount` et `channels[]`. Le CSV
réserve quatre colonnes `pv1_W` à `pv4_W`.

`/api/history` sans `date` renvoie le jour civil courant. `resolution` vaut `1`
(minute), `2` (15 min) ou `3` (jour), selon la résolution la plus fine encore
disponible pour ce jour ; `0` et `records:[]` si rien n'est mesuré. Une
résolution `3` ne contient qu'un seul enregistrement consolidé (`energyWh`,
`peak`, `peakTime`), sans détail par canal — c'est le repli utilisé une fois
que l'anneau minute/quart d'heure a évincé ce jour-là. `date` est refusé hors
de la plage `[2024-01-01, aujourd'hui]`.

## Mises à jour

`/api/update` reflète le résultat de la dernière vérification, automatique (toutes les 6 h,
après un délai initial de 60 s au démarrage) ou manuelle :
```json
{"checked":true,"available":true,"installing":false,"latestVersion":"2026.9.12","error":"","lastCheck":1758268800}
```
`latestVersion` et `lastCheck` restent vides/`null` tant qu'aucune vérification n'a abouti.
`POST /api/update/check` relance une vérification immédiate (bloque le temps de l'appel réseau,
une à deux secondes) et renvoie le même objet à jour. `POST /api/update/install` télécharge et
flashe la release déjà détectée par la dernière vérification (`404`/`409` si aucune n'est connue
ou si une mise à jour, automatique ou manuelle via `/api/ota`, est déjà en cours), puis redémarre
— les deux chemins de flash partagent le même verrou de réentrance. Les versions suivent le
format calendaire `AAAA.M.PATCH` (façon Home Assistant/ESPHome) ; la comparaison se fait champ
par champ, pas alphabétiquement.

## Capture

```json
{"action":"scan","dwell":5}
```

```json
{"action":"capture","channel":16,"duration":300}
```

`channel` verrouille une écoute continue ; `pause` arrête l'enregistrement en RAM,
`resume` reprend, `clear` efface seulement les buffers RAM. Le périphérique reste
passif dans tous les cas. Pour exporter une fenêtre cohérente, mettre en pause
avant le téléchargement. Une capture longue garde seulement la fin si l'anneau déborde.
