#!/usr/bin/env python3
import concurrent.futures
import json
import re
from datetime import datetime, timezone

from discover import ROOT, clean, export, norm, save, search

ALIASES = {
    'Cyberpunk 2077: Ultimate Edition': 'Cyberpunk 2077',
    'ELDEN RING Tarnished Edition': 'Elden Ring',
    'Star Wars Outlaws Gold Edition': 'Star Wars Outlaws',
    'Super Mario Bros. Wonder – Nintendo Switch 2 Edition + Meetup in Bellabel Park': 'Super Mario Bros. Wonder',
    '007 First Light - Deluxe Edition': '007 First Light',
    'Tales of ARISE - Beyond the Dawn Edition': 'Tales of Arise',
    'HITMAN World of Assassination – Signature Edition': 'Hitman World of Assassination',
    'GRID Legends: Deluxe Edition': 'GRID Legends',
    'Lies of P: Complete Edition': 'Lies of P',
    'Stellar Blade Complete Edition': 'Stellar Blade',
    'Resident Evil 4 Gold Edition': 'Resident Evil 4',
    'Resident Evil 2 Deluxe Edition': 'Resident Evil 2',
    'Resident Evil Village Gold Edition': 'Resident Evil Village',
    'Resident Evil 7 biohazard Gold Edition': 'Resident Evil 7: Biohazard',
    'South of Midnight Weaver\'s Edition': 'South of Midnight',
    'FATAL FURY: City of the Wolves - STANDARD Edition': 'Fatal Fury: City of the Wolves',
}


def refine(row):
    title = clean(row['nintendo']['title'])
    if row['sgdb']:
        return row
    candidates = row.get('candidates', [])
    exact = [g for g in candidates if norm(g['name']) == norm(title)]
    if len(exact) > 1:
        year = (row['nintendo'].get('date_from') or '')[:4]
        same_year = [g for g in exact if g.get('release_date') and datetime.fromtimestamp(g['release_date'], timezone.utc).strftime('%Y') == year]
        if len(same_year) == 1:
            return dict(row, sgdb=same_year[0], method='exact_title_disambiguated_by_nintendo_release_year')
    alias = ALIASES.get(title)
    if not alias:
        alias = re.sub(r'^(Arcade Archives 2|Console Archives)\s+', '', title)
        if alias == title:
            return row
    found = search(alias)
    exact = [g for g in found if norm(g['name']) == norm(alias)]
    if len(exact) == 1:
        return dict(row, sgdb=exact[0], method='explicit_port_or_edition_alias_to_shared_game', alias=alias,
                    queries=row.get('queries', []) + [alias])
    return dict(row, alias=alias, queries=row.get('queries', []) + [alias],
                candidates=list({g['id']: g for g in candidates + found}.values()))


def main():
    rows = json.loads((ROOT / 'matches.json').read_text())
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        rows = list(pool.map(refine, rows))
    save('matches.json', rows)
    save('matching-rules.json', {'normalization': 'Case, punctuation, Unicode diacritics, and trademark marks; ampersand becomes and',
        'acceptedMethods': ['Unique exact normalized title', 'Unique exact normalized title without Nintendo Switch 2 Edition',
            'Duplicate exact titles disambiguated by matching Nintendo release year',
            'Explicit named edition aliases or Arcade Archives 2 / Console Archives port labels'],
        'aliases': ALIASES, 'ambiguities': 'Multiple exact candidates remain unmatched; no fuzzy matches are accepted'})
    export(rows)


if __name__ == '__main__':
    main()
