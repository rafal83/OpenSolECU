# Stockage format 3

Le même layout de données convient aux variantes 4 et 8 Mo. La Flash est accédée
par `esp_partition`, sans filesystem ni NVS pour les mesures.

| Résolution | Secteurs 4 KiB | Places | Rétention minimale après effacement du secteur le plus ancien |
|---|---:|---:|---|
| 1 minute | 70 | 2 940 | 2 899 minutes, plus de 48 h |
| 15 minutes | 70 | 2 940 | 2 899 quarts d'heure, plus de 30 jours |
| 1 jour | 80 | 3 360 | 3 319 jours mesurés, environ 9 ans |

Total réservé : **901 120 octets**, 220 secteurs. La partition 4 Mo fournit
917 504 octets ; celle de 8 Mo 4 063 232 octets. L'espace supplémentaire de la
version 8 Mo reste disponible pour une évolution du format.

Chaque secteur contient 42 enregistrements de 96 octets (4 032 octets), puis
64 octets inutilisés. Aucun enregistrement ne chevauche deux secteurs. Le début
d'un nouveau secteur est effacé avant écriture ; tous les autres ajouts n'effacent
aucun secteur. Les queues absorbent les instants où plusieurs résolutions ferment.

## Format explicite little endian

| Octets | Champ |
|---|---|
| 0–3 | magic `OSOL` |
| 4 | version 3 |
| 5 | résolution 1 / 2 / 3 |
| 6 | bit 0 : simulation |
| 7 | nombre de canaux PV, 0 à 4 |
| 8–15 | séquence uint64 |
| 16–23 | début de période, secondes Unix UTC |
| 24–31 | durée et couverture mesurée en secondes |
| 32–35 | date locale YYYYMMDD |
| 36–43 | PV1 à PV4, uint16 à 0,1 W ; `FFFF` = inconnu |
| 44–55 | pic de journée et son heure |
| 56–79 | énergie période, total mesuré, journée : float64 Wh |
| 80–85 | numéro de série sur 6 octets |
| 86–91 | réservés zéro |
| 92–95 | CRC32 IEEE sur les 92 premiers octets |

Au boot, chaque anneau est parcouru. Seuls magic/version/CRC valides sont lus.
La plus grande séquence identifie le dernier record. Une écriture interrompue
est ignorée ; les emplacements partiellement programmés sont sautés avant reprise,
car une NOR ne peut pas réécrire un bit de 0 à 1 sans effacement. Les tests injectent
des coupures à chaque octet d'une écriture et vérifient la reprise et le record précédent.

La mesure live est en RAM. Les écritures d'historique minute ne se produisent pas
toutes les 5 secondes. Au rythme nominal, chaque secteur minute est réutilisé
environ tous les deux jours ; vérifier l'endurance réelle de la Flash du module.

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

NVS contient exclusivement la configuration. Le format 3 ne relit pas les anciens
records : ils sont ignorés puis remplacés à mesure que les secteurs tournent. Les
mises à jour OTA n'effacent ni NVS ni la partition storage. Les exports JSON ne
contiennent aucun secret.
