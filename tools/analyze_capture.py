"""Decode a PCAP/PCAPNG with the installed Wireshark engine, entirely offline.

Example: python tools/analyze_capture.py capture.pcapng --out dist/analysis
Writes a TSV frame list, full protocol dissection and a Markdown summary.
This tool does not open a serial port, change a capture, or transmit packets.
"""
import argparse
import collections
import csv
import io
import pathlib
import shutil
import subprocess

FIELDS = ['frame.number', 'frame.time_relative', 'frame.len', '_ws.col.protocol',
          '_ws.col.info', 'wpan.dst_pan', 'wpan.src16', 'wpan.src64', 'wpan.dst16',
          'wpan.seq_no', 'wpan.frame_type', 'zbee_aps.profile', 'zbee_aps.cluster',
          'zbee_aps.src', 'zbee_aps.dst', 'udp.srcport', 'udp.dstport',
          '_ws.expert.message']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=pathlib.Path)
    parser.add_argument('--out', type=pathlib.Path, default=pathlib.Path('dist/analysis'))
    parser.add_argument('--tshark', help='Chemin de tshark.exe si nécessaire')
    args = parser.parse_args()
    tshark = args.tshark or shutil.which('tshark')
    fallback = pathlib.Path(r'C:\Program Files\Wireshark\tshark.exe')
    if not tshark and fallback.is_file():
        tshark = str(fallback)
    if not tshark:
        parser.error('Installer Wireshark avec TShark, ou préciser --tshark.')
    if not args.capture.is_file():
        parser.error('Capture introuvable.')
    args.out.mkdir(parents=True, exist_ok=True)
    base = [tshark, '-n', '-r', str(args.capture.resolve())]
    cmd = base + ['-T', 'fields', '-E', 'header=y', '-E', 'quote=d']
    for field in FIELDS:
        cmd += ['-e', field]
    result = subprocess.run(cmd, capture_output=True, encoding='utf-8', errors='replace', timeout=120)
    if result.returncode:
        parser.error(result.stderr.strip())
    (args.out / 'frames.tsv').write_text(result.stdout, encoding='utf-8')
    rows = list(csv.DictReader(io.StringIO(result.stdout), delimiter='\t'))
    with (args.out / 'decodage.txt').open('w', encoding='utf-8') as out:
        subprocess.run(base + ['-V'], stdout=out, stderr=subprocess.PIPE, check=True, timeout=120)
    counts = collections.Counter(r['_ws.col.protocol'].strip() for r in rows)
    pans = collections.Counter(r['wpan.dst_pan'] for r in rows if r['wpan.dst_pan'])
    # Ask the dissector to evaluate the exact predicate, including its own validity checks.
    aps = subprocess.run(base + ['-Y', 'zbee_aps.profile == 0x0f05 && zbee_aps.src == 0x14 && zbee_aps.dst == 0x14',
                                  '-T', 'fields', '-e', 'frame.number'],
                         capture_output=True, text=True, check=True, timeout=120).stdout.splitlines()
    summary = ['# Analyse de capture OpenSolECU', '', f'Fichier : `{args.capture.name}`.',
               f'Trames : **{len(rows)}**. Trames au profil/endpoints APsystems de référence : **{len(aps)}**.',
               '', '| Protocole décodé par Wireshark | Nombre |', '|---|---:|']
    summary += [f'| {name} | {count} |' for name, count in counts.most_common()]
    summary += ['', 'PAN observés (les ACK sans adresse sont exclus) : ' +
                ', '.join(f'`{pan}` : {count}' for pan, count in pans.items()) + '.', '',
                'Voir `frames.tsv` pour le tableau et `decodage.txt` pour les champs octet par octet.', '',
                'Un profil/endpoints compatible est un indice APsystems, pas une preuve du modèle DS3.',
                'Un message protégé sans clé ne permet pas de lire les mesures applicatives.',
                'Le chiffrement peut se trouver dans MAC, Zigbee NWK/APS ou MLE : un MAC non chiffré',
                'ne garantit pas une application lisible. Les ACK ne contiennent pas de mesures.', '',
                'Filtres Wireshark :', '```text', 'wpan.frame_type != 2', 'mle', 'zbee_nwk',
                'zbee_aps.profile == 0x0f05 && zbee_aps.src == 0x14 && zbee_aps.dst == 0x14',
                'zbee_aps.profile == 0x0f05 && zbee_aps.cluster == 0x0006',
                'zbee_aps.profile == 0x0f05 && zbee_aps.cluster == 0x0106', '```', '',
                'Ne pas appliquer directement les offsets du décodeur DS3 à une trame MAC complète.',
                'Il faut d’abord retirer les en-têtes, traiter la sécurité/fragmentation éventuelle,',
                'puis valider le format applicatif et sa somme de contrôle.']
    (args.out / 'rapport.md').write_text('\n'.join(summary) + '\n', encoding='utf-8')
    print(f'{len(rows)} trames, {len(aps)} candidates APsystems. Rapport : {args.out / "rapport.md"}')


if __name__ == '__main__':
    main()
