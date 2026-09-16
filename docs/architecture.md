# Architecture et exécution

ESP-IDF 5.5.1, C++ sans exceptions/RTTI embarqués, FreeRTOS sur le cœur principal C6.

```text
radio 802.15.4 ISR → queue RX (24) → tâche radio
                                      ├─ SNIFFER → anneau RAM → API/SSE/export
                                      │      └─ copie APS → queue fleet (12)
                                      └─ NORMAL → poll onduleur → résultat identifié
                                                               │
acquisition / mock → mesure avec serial → queue fleet (12) ←─────┘
                                             │
fleet → réassemblage APS → APSystemsDecoder + Aggregator par serial (16 max)
                                             │
                              queue stockage (12) → storage → Flash

Wi-Fi AP + STA → esp_http_server → REST / SSE / ressources embarquées
USB console → diagnostics locaux et commandes sniffer
```

| Composant | Responsabilité |
|---|---|
| aps_radio | API abstraite APSRadio ; pilote C6, tampons ISR, durées TX, politique RX-only |
| aps_protocol | Encapsulation MAC/NWK/APS, commandes APsystems, analyse passive, PCAPNG |
| inverter | Types, décodeurs DS3/YC600/QS1, intégration, agrégation, sérialisation, journal NOR portable |
| acquisition | Ordonnancement par onduleur, retries, résultats identifiés, mode mock |
| fleet | États et agrégateurs par numéro de série, décodage passif, snapshots mutex |
| sniffer | Scan passif, statistiques bornées, capture RAM, commandes de capture |
| storage | Adaptateur esp_partition et tâche d'écriture |
| statistics | Agrégats de calendrier depuis les journées mesurées |
| wifi_manager | APSTA, fallback AP, retry STA et scan asynchrone |
| time_manager | SNTP et règle locale TZ |
| config | Configuration versionnée NVS et validation JSON |
| logging | 64 lignes en RAM, ERROR à TRACE |
| api / web_server | REST sans authentification, validation des mutations, SSE, OTA, assets gzip |

La tâche radio ne fait aucune écriture Flash ni requête réseau. Les ISR copient
les octets et métadonnées dans une queue puis libèrent le buffer du pilote avec
`esp_ieee802154_receive_handle_done`. Les résultats TX proviennent des callbacks,
pas du simple retour `ESP_OK` de la demande d'émission. Le buffer TX reste valide
jusqu'au callback ou à l'arrêt explicite de la radio sur timeout.

En SNIFFER, le chemin d'initialisation ne passe pas par les commandes réseau.
La réception démarre seulement après le passage promiscuous qui désactive les
ACK automatiques dans le pilote IDF épinglé. La tâche sniffer devient propriétaire
exclusif de la radio. La compilation `CONFIG_OPENSOLECU_FORCE_SNIFFER` force aussi
la lecture de configuration et rejette les demandes NORMAL, y compris après OTA
avec une configuration antérieure NORMAL.

Les changements réseau/mode sont sauvegardés puis appliqués au redémarrage.
L'interface n'affiche RX ONLY que lorsque la tâche sniffer est réellement active.

L'HTTP principal accepte les deux interfaces. Les connexions SSE sont traitées
dans des tâches distinctes ; deux streams simultanés au maximum, 8 trames/s par
stream sniffer, lots limités. Les sessions SSE sont renouvelées pour récupérer
les ressources des clients partis. Les exports ne collectent pas toute la Flash
en JSON dans le heap ; ils sérialisent chaque enregistrement successivement.

Le sniffer conserve au maximum 24 appareils, 24 sources/destinations et 16 PAN
par canal. Les dépassements sont signalés. Un rôle ECU est une inférence ; aucune
adresse courte n'est transformée artificiellement en adresse IEEE.

Le réassembleur conserve quatre messages simultanés, quatre blocs de 125 octets
maximum chacun, pendant dix secondes. Son identité utilise canal, PAN, source et
destination **NWK**, endpoints, profil, cluster et compteur APS : un relais MAC
ne crée pas un autre onduleur. Doublons identiques, ACK, messages chiffrés,
conflits et messages incomplets ne produisent pas de nouvelles mesures.
