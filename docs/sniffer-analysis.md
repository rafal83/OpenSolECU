# Sniffer passif : utilisation, classification et validation

## Garantie de conception RX ONLY

Sur le build `sdkconfig.sniffer`, NORMAL est refusé même si NVS contient une
ancienne configuration active. Dans le build général, le défaut premier boot
reste SNIFFER. Les changements de mode s'appliquent au redémarrage.

Le branchement passif de `ESP32C6Radio::begin()` :

1. active le périphérique IEEE 802.15.4 ;
2. appelle `esp_ieee802154_set_promiscuous(true)` avant la première réception ;
3. règle uniquement le canal local de réception et RX-on-idle ;
4. commence RX, sans PAN, adresse, coordinateur, réseau ni commande APsystems.

Dans ESP-IDF **v5.5.1**, `esp_ieee802154_set_promiscuous(true)` désactive
`auto_ack_rx`, `auto_ack_tx` et `enhance_ack_tx` dans le PIB. Cela désactive aussi
les ACK matériels ; aucune exception d'ACK logiciel n'est utilisée. Ce comportement
doit être revérifié lors d'un changement de version ESP-IDF.

`send()` refuse le passif à l'entrée et devant l'appel matériel via une politique
testée. `requestPair()` refuse le mode passif. L'ordonnanceur d'acquisition est
suspendu et la tâche radio entre exclusivement dans `snifferRun()`. La commande
USB `tx-guard-test` tente le chemin logiciel et doit retourner PASS avec un log
ERROR indiquant que l'émission a été rejetée. Elle ne force aucune émission.

Ces propriétés logicielles ne remplacent pas une mesure RF indépendante de
l'absence d'émission. Le Wi-Fi AP/STA continue évidemment d'émettre des trames
Wi-Fi pour l'accès à l'interface ; RX ONLY concerne IEEE 802.15.4.

## Procédure avec ECU-C et plusieurs DS3 existants

1. Garder ECU-C allumée, DS3 associés et alimentés par leurs panneaux.
2. Flasher le build passif adapté à la Flash du C6, le poser à proximité.
3. Se connecter à l'AP OpenSolECU, ouvrir `http://192.168.4.1/debug/sniffer`.
4. Lancer directement un scan 11–26, 5 s/canal, sans connexion administrateur.
5. Lire les profils/endpoints détectés, pas seulement le nombre de trames.
6. S'il n'y a aucun poll pendant le scan, refaire une capture fixe plus longue :
   une ECU peut interroger les onduleurs seulement toutes les quelques minutes.
7. Verrouiller le canal puis capturer 30/60/300/600 secondes.
8. Après fin, exporter le PCAPNG/JSONL. Pour une écoute continue, cliquer Pause
   avant export ; cela stabilise le contenu RAM.
9. Dans Wireshark, examiner les messages inconnus, les PAN et les adresses ;
   comparer les octets utiles aux [preuves de protocole](protocol.md).

Aucun reset, pair, unpair, retrait d'onduleur ou commande à l'ECU-C n'est nécessaire.

## Données observées et inférées

Le parser lit les modes MAC courts/étendus, PAN séparés/comprimés, type, FCF,
séquence et flags. Les champs absents sont `null`. Pour les versions MAC 2015,
IE ou layouts non implémentés, les octets sont conservés et marqués **unparsed**,
sans inventer la table de présence des PAN. Les payloads protégés sont **encrypted**.

NWK et APS ne sont interprétés que lorsque leurs préconditions sont satisfaites.
Les profils autres que `0F05` ne sont pas APsystems par défaut. Même `0F05` n'est
qu'un indice : on exige aussi les endpoints `14`, puis des structures/commandes
cohérentes pour une classification plus précise.

| Classe | Correspondance |
|---|---|
| UNKNOWN | Frame trop courte ou non reconnue |
| IEEE802154 | MAC reconnu, aucun Zigbee applicatif affirmé |
| ZIGBEE | NWK exploitable, application inconnue/protégée |
| APSYSTEMS_UNKNOWN | Profil/endpoints compatibles, commande non reconnue |
| APSYSTEMS_ECU_TO_INVERTER | Enveloppe FBFB provenant du NWK 0000 |
| APSYSTEMS_INVERTER_TO_ECU | Enveloppe compatible provenant d'un autre NWK |
| APSYSTEMS_PAIR | Clusters de la séquence d'appairage publique |
| APSYSTEMS_POLL | Cluster 0006 et requête BB de 19 octets |
| APSYSTEMS_RESPONSE | Cluster 0106 et en-tête de réponse DS3 5CBBBB |

**KNOWN FRAME** : payload de poll et première séquence de pair testés contre
captures publiques, compteurs ignorés dans la comparaison.
**SIMILAR FRAME** : profil/endpoints/enveloppe concordent mais contenu diffère.
**UNKNOWN FRAME** : données insuffisantes, chiffrement ou layout non reconnu.
Les échanges ECU-C/DS3 de votre site doivent encore confirmer ces correspondances.

## Appareils et cycles

Les adresses sont DETECTED ; les rôles restent INFERRED. Un émetteur de poll est
un **ECU candidate**, sans distinction inventée entre ECU-C/R/B. Une réponse dont
le format paraît DS3 donne **DS3 candidate**, avec serial possible extrait.
Une adresse IEEE non présente dans la trame n'est jamais fabriquée à partir de
l'adresse courte. Les appareils non classifiés restent UNKNOWN.

Les délais sont une corrélation temporelle par PAN/adresse réseau : dernier poll
vers un destinataire, puis réponse en moins de 10 s. Ils sont donc heuristiques,
et non une preuve de corrélation transactionnelle. Intervalles et temps de réponse
sont calculés à partir des timestamps monotones de capture, indépendamment du NTP.

## RAM et trafic navigateur

- 256 trames, 125 octets MAC utiles maximum par trame, métadonnées fixes.
- Aucun enregistrement automatique de trames en Flash.
- RX ISR → queue bornée de 24, sans JSON ni appel réseau dans l'ISR.
- 24 appareils maximum ; compteurs d'adresses/PAN limités par canal avec drapeau
  de troncature. Le compteur de queue RX perdue est exposé dans les diagnostics radio.
- 200 trames maximum dans le navigateur ; SSE limité à 8 trames/s par client,
  deux streams globaux maximum. L'anneau peut écraser les frames avant leur affichage.
- Les captures de 10 minutes contiennent seulement les dernières trames si le
  trafic dépasse la capacité. Exporter des fenêtres plus courtes pour éviter cela.

## Formats d'export

PCAPNG : une interface par canal 11–26 ; linktype **230**, IEEE 802.15.4 sans FCS.
Le pilote ESP remplace les octets FCS reçus par les métadonnées radio : ils sont
retirés, jamais présentés comme une somme de contrôle originale. Chaque bloc EPB
conserve l'horodatage microseconde et un commentaire canal/RSSI/LQI. Un lecteur
Python indépendant vérifie les tailles de blocs, le linktype, les octets, le
timestamp et les métadonnées produits par l'écrivain C++.

Sans synchronisation NTP/manuelle, le timestamp PCAP est monotone depuis le boot
et le commentaire indique **unsynchronized** ; la date 1970 éventuellement affichée
par Wireshark n'est pas une date de capture réelle. JSONL fournit `timestampUs=0`
et `monotonicUs` séparément. Les détails JSON incluent MAC/NWK/APS, octets bruts,
offset/longueur du payload et état d'analyse.

## Coexistence Wi-Fi

`CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y` arbitre le matériel partagé. Le scan change
le canal 802.15.4 seulement, sans modifier le Wi-Fi. Le C6 n'est pas deux radios
indépendantes : le trafic Web/Wi-Fi peut entraîner des pertes de capture. Réduire
les clients SSE, éviter le scan Wi-Fi et les gros téléchargements pendant une
capture importante. Une absence de frames ne prouve ni l'absence d'un appareil
ni le mauvais canal ; elle peut aussi résulter du cycle de polling, de la portée
ou du partage du temps d'antenne.
