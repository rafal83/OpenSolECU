# Plusieurs onduleurs APsystems et tous les PV

Le tableau de bord suit jusqu'à **16 micro-onduleurs APsystems**. Un DS3 ou un
YC600 expose deux entrées PV ; un QS1 en expose quatre. La capacité maximale est
donc de 64 entrées. Chaque onduleur conserve son numéro de série, son modèle, ses
mesures, ses compteurs et son historique propres.

Dans Réglages → Onduleurs et panneaux, le modèle peut être `DS3`, `YC600`, `QS1`
ou `AUTO`. AUTO distingue les payloads `FB FB 5C` des DS3 et `FB FB 51` des
YC600/QS1 ; pour ces deux derniers, la famille du numéro de série départage le
YC600 (4xx) du QS1 (8xx). Le choix explicite reste disponible si un numéro de
série ne suit pas cette convention.

En écoute passive, le réassembleur APS accepte les réponses complètes de 94 ou
105 octets. Le décodeur vérifie le numéro de série, l'enveloppe, la longueur et
la somme applicative disponible avant de publier une mesure. Un onduleur absent
de la configuration peut être découvert en RAM ; il faut l'ajouter aux réglages
pour le conserver après redémarrage.

Les deux ou quatre PV restent affichés même sans réponse. « — » indique une
valeur inconnue. Une première réponse donne les tensions et courants ; la
puissance est calculée avec les variations des compteurs entre deux réponses
cohérentes, au plus quinze minutes d'écart en passif. Après quinze minutes sans
réponse, la mesure expire. Le total du site n'est complet que si chaque onduleur
possède une puissance connue.

## Conservation des données

Le blob NVS `inventory` utilise le schéma version 2 avec le modèle de chaque
onduleur. Les enregistrements de mesure utilisent le format 3. Il n'existe pas
de migration des schémas précédents : l'inventaire doit être recréé et les anciens
records de `storage` sont ignorés, puis remplacés lors de la rotation des secteurs.

Chaque enregistrement garde une longueur de 96 octets. Quatre puissances sont
encodées à 0,1 W, accompagnées de `channelCount`. Les journaux restent partagés :
2 940 records minute, 2 940 quarts d'heure et 3 360 journées. La rétention par
onduleur dépend donc du nombre d'appareils qui produisent des mesures.

## Validation logicielle

Les tests hôte décodent des payloads publiés pour DS3, YC600 et QS1. Ils vérifient
la détection AUTO, le modèle forcé, les deux ou quatre compteurs indépendants, la
puissance calculée sur deux réponses, la sérialisation à quatre canaux et le
réassemblage APS. Le test navigateur utilise un inventaire mixte DS3/YC600/QS1 de
huit PV et vérifie les vues ordinateur et mobile.

Les fixtures de référence couvrent DS3, YC600 et QS1. Le mode NORMAL reste
expérimental et doit être validé avec le matériel ciblé avant utilisation.
