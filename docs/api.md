# API HTTP locale

Les nombres inconnus sont JSON `null`. Les timestamps sont des secondes UTC,
sauf `timestampUs`/`monotonicUs` du sniffer. Pas de CORS universel.

| Méthode et chemin | Réponse |
|---|---|
| GET /api/status, /api/system | Firmware, heap, horloge, Wi-Fi, radio, Flash |
| GET /api/live | `inverters[]`, deux ou quatre canaux PV, total complet ou partiel |
| GET /api/history?range=today\|7d\|30d\|12m&serial=... | `{records:[...]}` pour l'onduleur sélectionné |
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
