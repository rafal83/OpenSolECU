# Stockage format 4

Le même layout de données convient aux variantes 4 et 8 Mo. La Flash est accédée
par `esp_partition`, sans filesystem ni NVS pour les mesures.

Quatre anneaux, dimensionnés pour un usage à ~3 onduleurs configurés (les trois
premiers se partagent leur capacité entre tous les onduleurs configurés ; le
dernier ne dépend pas de leur nombre) :

| Résolution | Secteurs 4 KiB | Places (×64/secteur) | Rétention minimale après effacement du secteur le plus ancien |
|---|---:|---:|---|
| 1 minute (par onduleur) | 135 | 8 640 | 8 577 minutes, environ 2 jours à 3 onduleurs |
| 15 minutes (par onduleur) | 248 | 15 872 | 15 809 quarts d'heure, environ 55 jours à 3 onduleurs |
| 1 jour, par onduleur | 2 | 128 | 65 jours mesurés à 3 onduleurs |
| 1 jour, tous onduleurs combinés (`dayAll`) | 29 | 1 856 | 1 793 jours, environ 4,9 ans |

Total réservé : **1 695 744 octets**, 414 secteurs. La partition 4 Mo fournit
1 703 936 octets ; celle de 8 Mo 4 063 232 octets. L'espace supplémentaire de la
version 8 Mo reste disponible pour une évolution du format.

Le détail par onduleur (minute, quart d'heure, jour) est volontairement limité
dans le temps : passé sa fenêtre, `dayAll` prend le relais pour le total combiné
(voir plus bas), mais la ventilation par onduleur individuel n'est plus
disponible pour les jours trop anciens — un choix délibéré pour maximiser la
rétention du détail récent plutôt que de la diluer sur tous les onduleurs pour
toujours.

Chaque secteur contient 64 enregistrements de 64 octets (4 096 octets), sans
octet perdu. Aucun enregistrement ne chevauche deux secteurs. Le début
d'un nouveau secteur est effacé avant écriture ; tous les autres ajouts n'effacent
aucun secteur. Les queues absorbent les instants où plusieurs résolutions ferment.

## Format explicite little endian

| Octets | Champ |
|---|---|
| 0 | version format (rejette l'enregistrement si différente — actuellement 4) |
| 1 | résolution 1 / 2 / 3 / 4 (minute / quart d'heure / jour / jour combiné) |
| 2 | bit 0 : simulation |
| 3 | nombre de canaux PV, 0 à 4 |
| 4–7 | séquence uint32 |
| 8–11 | début de période, secondes Unix UTC (uint32, valide jusqu'en 2106) |
| 12–19 | durée et couverture mesurée en secondes (uint32 chacun) |
| 20–23 | date locale YYYYMMDD |
| 24–31 | PV1 à PV4, uint16 à 0,1 W ; `FFFF` = inconnu |
| 32–39 | pic de journée (float32) et son heure (uint32) |
| 40–51 | énergie période, total mesuré, journée : float32 Wh chacun |
| 52–57 | numéro de série sur 6 octets (vide pour un enregistrement `dayAll`) |
| 58–59 | réservés zéro |
| 60–63 | CRC32 IEEE sur les 60 premiers octets |

Pas de migration entre versions de format : un enregistrement d'une autre
version est traité comme absent (même sort qu'une écriture interrompue). Une
mise à jour de firmware qui change ce format perd donc l'historique existant —
accepté tant que le projet reste expérimental.

Au boot, chaque anneau est parcouru. Seuls version/CRC valides sont lus.
La plus grande séquence identifie le dernier record. Une écriture interrompue
est ignorée ; les emplacements partiellement programmés sont sautés avant reprise,
car une NOR ne peut pas réécrire un bit de 0 à 1 sans effacement. Les tests injectent
des coupures à chaque octet d'une écriture et vérifient la reprise et le record précédent.

La mesure live est en RAM. Les écritures d'historique minute ne se produisent pas
toutes les 5 secondes. Vérifier l'endurance réelle de la Flash du module au
rythme d'écriture effectif de votre configuration (nombre d'onduleurs).

## Anneau combiné (`dayAll`)

Une fois par jour (avec un délai de sécurité de 2 jours pour laisser à chaque
onduleur le temps de clôturer sa propre journée), le firmware additionne les
enregistrements « jour » de tous les onduleurs configurés pour une date donnée
et écrit un unique enregistrement combiné (`serial` vide, résolution 4) dans
l'anneau `dayAll`. Idempotent : l'écriture est déclenchée périodiquement (au
plus une fois toutes les 10 minutes) et la déduplication à l'écriture (même
mécanisme que l'anneau jour par-onduleur) absorbe les répétitions après reboot.

Les fonctions qui calculent une énergie combinée sur plusieurs jours
(`statsAllJson`, `consolidatedDayAll`) lisent d'abord l'anneau jour par-onduleur
puis complètent avec `dayAll` pour les journées qui en sont sorties — c'est ce
qui permet à `yearWh`/`monthWh` de rester exacts bien au-delà de la fenêtre de
détail par onduleur.

## Énergie et temps

Le DS3 fournit deux compteurs bruts, le YC600 deux et le QS1 quatre. La conversion
DS3 observée est `raw × 1.66 / 100000 Wh` ; YC600/QS1 utilisent
`raw × 8.311 / 3600 Wh`. Les deltas donnent l'énergie ; la puissance est calculée
sur la différence du temps de l'onduleur. Le premier échantillon, les resets,
les compteurs invalides, les doublons et les gaps ne produisent pas de puissance
inventée. Le mock utilise une intégration trapézoïdale sur temps monotone.

Les intervalles mesurés sont répartis aux frontières de minute et de journée
locale. Le début de journée et sa durée sont calculés par `localtime_r/mktime`
avec `tm_isdst=-1` : une journée Paris peut durer 23, 24 ou 25 heures. Les clés
UTC restent non ambiguës lors de l'heure répétée en automne. Les inconnues ne
sont pas des zéros et la couverture indique combien de secondes ont été mesurées.

Les totaux et le pic de journée sont repris depuis le dernier checkpoint minute.
Une coupure peut perdre la partie non persistée (environ une minute), et le premier
échantillon après reboot sert à recaler les compteurs. Les périodes offline
ne sont pas reconstituées. Les statistiques portent sur les journées conservées,
tandis que le total mesuré est cumulatif. Sans première synchronisation horaire,
l'énergie peut être comptée en RAM mais n'est pas affectée à une date inventée.

NVS contient exclusivement la configuration. Le format 4 ne relit pas les anciens
records : ils sont ignorés puis remplacés à mesure que les secteurs tournent. Les
mises à jour OTA n'effacent ni NVS ni la partition storage. Les exports JSON ne
contiennent aucun secret.
