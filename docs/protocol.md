# APsystems : preuves et limites du portage

## Références examinées

- [ESP32-read-APS-inverters](https://github.com/patience4711/ESP32-read-APS-inverters),
  commit `7b0ff63eeb277a487ed123ffeef70d76902666b1` : `ZIGBEE_COORDINATOR.ino`,
  `ZIGBEE_PAIR.ino`, `ZIGBEE_POLLING.ino`, `ZIGBEE_HELPERS.ino`, `AAA_DECODE.ino`.
- [Archive firmware liée par la référence](https://github.com/patience4711/read-APSystems-YC600-QS1-DS3/blob/main/cc25xx_firmware.zip) :
  CC2530ZNP-with-SBL, variantes CC2591/CC2592 DS3 ; anciens CC2530/CC2531 YC/QS.
- [Déclaration de l'auteur sur les modifications non publiées](https://github.com/Koenkk/zigbee2mqtt/issues/4221#issuecomment-860025930).
- [Captures publiques ECU-R, YC600/QS1](https://github.com/Koenkk/zigbee2mqtt/files/5321247/ECU-R_switch_on_paired.zip)
  et [appairage](https://github.com/Koenkk/zigbee2mqtt/files/5306674/YC600_zigbee_2.zip).
- API et implémentation locales ESP-IDF v5.5.1 :
  `components/ieee802154/include/esp_ieee802154.h` et `esp_ieee802154.c`.

Les dépôts/archive d'analyse sont dans `reference/`, exclus de la distribution.
Les firmwares CC25xx de l'auteur ont des conditions non commerciales ; ils ne
sont ni liés au firmware OpenSolECU ni nécessaires à son exécution.

## Choix radio

**IEEE 802.15.4 bas niveau**, pas ESP Zigbee SDK. Les ECU emploient des messages
APsystems et une procédure d'association propriétaire, et le firmware ZNP requis
modifie le réseau et AF_DATA_REQUEST_EXT. Un simple endpoint Zigbee standard ne
reproduit donc pas la référence. Le pilote C6 produit les PSDU MAC ; il ne reçoit
jamais un message `FE...` UART comme s'il s'agissait d'une trame radio.

| Élément | État | Preuve / interprétation |
|---|---|---|
| UART ESP ↔ CC25xx | KNOWN | 115200, 8N1 |
| Enveloppe ZNP | KNOWN | `FE LEN CMD0 CMD1 DATA FCS`, FCS = XOR LEN..DATA |
| Channel de référence | KNOWN | masque `00000100` little endian = bit 16, captures canal 16 |
| Canal de votre ECU-C | TO VERIFY | scan passif ; aucun canal universel imposé |
| PAN de référence | KNOWN | `D8 A3` sur le fil, valeur numérique `0xA3D8` |
| IEEE ECU de référence | KNOWN | `FF FF 80 97 1B 01 A3 D8` sur le fil |
| Adresse réseau ECU | KNOWN | `0x0000` dans les captures |
| Endpoint source/destination | KNOWN | `0x14` |
| Profile ID | KNOWN | `0x0F05` (`05 0F` sur le fil) |
| Poll / réponse | KNOWN | clusters `0x0006` / `0x0106` |
| Pair | KNOWN | clusters `020D`, `020C`, `010F`, `0101`, diffusion PAN `FFFF` |
| Formation des trames ECU | KNOWN | tests octet par octet contre les captures, compteurs normalisés |
| Application de ces en-têtes au DS3/ECU-C | INFERRED | code DS3 utilise les mêmes commandes AF ; capture propre requise |
| Chiffrement DS3 installé | TO VERIFY | le sniffer conserve les en-têtes lisibles, ne déchiffre pas |
| FCS IEEE 802.15.4 | KNOWN | généré/vérifié par le matériel ; octets RX remplacés par RSSI/LQI par IDF |
| Retries/timeout OpenSolECU | INFERRED | choix logiciel : 3 tentatives, attente réponse 1100 ms chacune |
| Temporisation pair référence | KNOWN | 1500 ms après chaque commande |
| Reprise réseau, routage multi-sauts | TO VERIFY | route requests émises ; pas une pile mesh Zigbee complète |

## Commandes

Le polling applicatif, après retrait de l'enveloppe UART/AF, est :

```text
<ECU serial inversé, 6 octets> FB FB 06 BB 00 00 00 00 00 00 C1 FE FE
```

`06` et les octets de commande contribuent à la somme applicative. La somme
de la requête vaut `0xC1`. Les préfixes/suffixes FBFB/FEFE ne sont pas le framing
MAC. La requête de polling est testée contre la trame 38 du PCAP public.

L'appairage émet successivement les séquences observées, avec le serial de l'onduleur,
le PAN attribué et l'identifiant ECU. La réponse est corrélée au serial et à son
adresse NWK source ; aucune adresse courte n'est fabriquée depuis le numéro de série.
Ce mécanisme n'est **jamais** exécuté dans le sniffer. Les commandes de limitation
de puissance, de redémarrage et de changement de profil réseau ne sont pas exposées.

Les compteurs MAC, NWK et APS sont indépendants et incrémentés. Le parseur vérifie
les bornes, modes d'adressage, version réseau, PAN/source/destination, profil,
endpoint et cluster avant d'accepter une réponse de mesure.

## Décodeurs APsystems

Le décodeur accepte trois modèles : DS3, YC600 et QS1. `AUTO` reconnaît l'en-tête
DS3 `FB FB 5C` ou l'en-tête YC600/QS1 `FB FB 51`, puis utilise la famille 4xx/8xx
du numéro de série pour distinguer YC600 et QS1. Une sélection explicite dans la
configuration désactive cette inférence.

### DS3

Réponse publiée, extraite de `AAA_DECODE.ino` : `tests/fixtures/ds3-0.hex`.
Le payload commence au serial de 6 octets. Il contient `FB FB 5C BB BB` et une
somme big endian de 16 bits avant `FE FE`. La somme de cet échantillon est `3969`.

| Offset depuis le serial | Octets | Traitement issu du code de référence |
|---|---:|---|
| 26 / 28 | 2 / 2, BE | PV1/PV2 voltage = valeur / 48 |
| 30 / 32 | 2 / 2, BE | PV1/PV2 courant = valeur × 0,0125 |
| 34 | 2, BE | tension AC = valeur / 3,8 |
| 36 | 2, BE | fréquence AC = valeur / 100 |
| 38 | 2, BE | temps de l'onduleur, secondes |
| 48 | 2, BE | température = valeur / 40 − 26,5 ; conversion confrontée aux valeurs ECU-C |
| 50 / 54 | 4 / 4, BE | compteurs énergie × 0,0000166 Wh |

Ces offsets/calibrations sont **KNOWN dans le code de référence**, leur validité
sur toutes variantes DS3 et l'ordre physique des entrées sont **TO VERIFY**. Des
commentaires de la référence inversent les noms des canaux ; OpenSolECU suit ses
instructions exécutables et affiche l'ordre sans prétendre l'avoir mesuré.

**Limite de longueur identifiée initialement** : la réponse UART publiée
contient 105 octets applicatifs. Avec les en-têtes classiques observés (MAC 9 +
NWK 8 + APS 8), cela ferait 130 octets hors FCS, alors que le PSDU 802.15.4
standard est limité à 127 octets FCS compris. La façon dont le firmware CC25xx
DS3 transporte/reconstitue ce message n'était pas démontrée par les seules sources
disponibles au démarrage du projet.

Le réassembleur prend en charge les extensions de fragmentation Zigbee APS avant
le décodage DS3. Les ACK APS et les fragments incomplets ne créent pas de mesures.
La puissance utilise deux réponses cohérentes du même numéro de série ; elle
n'est pas déduite d'un simple fragment ni du produit tension/courant. Le
remplacement actif de l'ECU reste expérimental.

### YC600 et QS1

Les fixtures `tests/fixtures/yc600.hex` et `tests/fixtures/qs1.hex` proviennent des
réponses publiées par le projet de référence. Ces payloads font 94 octets depuis
le serial, portent l'en-tête `FB FB 51` et terminent par `00 00 FE FE`. YC600
contient deux canaux et QS1 quatre.

| Offset depuis le serial | Traitement |
|---|---|
| 10 | température = uint16 BE × 0,2752 − 258,7 |
| 12 | fréquence = 50 000 000 / uint24 BE |
| 16–27 | canaux PV empaquetés sur 12 bits ; QS1 utilise les quatre, YC600 les deux derniers |
| 28 | tension AC = uint16 BE / 1,3277 / 4 |
| 17 (YC600) / 30 (QS1) | temps de l'onduleur, uint16 BE |
| 37 + 5 × canal | compteur énergie uint24 BE × 8,311 / 3600 Wh |

Les échantillons de référence mettent le champ de somme à zéro. Le décodeur
accepte donc zéro pour YC600/QS1, ou une somme additive correcte si elle est
présente. Un DS3 exige toujours sa somme réelle. Les bornes électriques et la
cohérence temporelle sont vérifiées avant publication.

## Validation

Les notions sont séparées : **compiled**, **software tested**, **protocol inferred**,
**hardware tested**. Le build réussi et les tests de payload n'impliquent ni un
appairage DS3 réussi ni une équivalence électrique ou radio à une ECU officielle.
Les essais physiques doivent documenter leurs captures et l'état de la carte,
sans confondre trafic radio inconnu et trafic APsystems.
