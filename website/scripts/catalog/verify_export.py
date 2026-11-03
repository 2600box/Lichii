#!/usr/bin/env python3
import json
from datetime import datetime, timezone
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from discover import ROOT, save


def main():
    catalog = json.loads((ROOT / 'catalog.json').read_text())
    source = json.loads((ROOT / 'source-coverage.json').read_text())
    coverage = json.loads((ROOT / 'coverage.json').read_text())
    members = json.loads((ROOT / 'membership.json').read_text())
    artwork = json.loads((ROOT / 'artwork-provenance.json').read_text())
    assert len({g['sgdbId'] for g in catalog}) == len(catalog)
    assert len({g['slug'] for g in catalog}) == len(catalog)
    assert source['reportedTotal'] == source['uniqueRecords'] == coverage['nintendoRecordsProcessed']
    assert sum(p['returned'] for p in source['pages']) == source['reportedTotal']
    compact = []
    for game in catalog:
        gid = str(game['sgdbId'])
        assert gid in members and gid in artwork
        assert all('Nintendo Switch 2' in m['platform'] for m in members[gid])
        record = {'sgdbId': game['sgdbId'], 'nintendoMembership': members[gid], 'artwork': {}}
        for role in ['cover', 'hero']:
            a = artwork[gid][role]
            if game[role + 'Path'] is None:
                assert a is None
                continue
            assert game[role + 'Path'] == a['path']
            path = ROOT / a['path'].lstrip('/')
            assert path.is_file()
            with Image.open(path) as image:
                image.load()
                assert image.format == 'WEBP'
                assert not getattr(image, 'is_animated', False)
                if role == 'cover':
                    assert image.width * 3 == image.height * 2
            record['artwork'][role] = {k: a[k] for k in ['assetId', 'assetType', 'sourceUrl', 'assetPage',
                'path', 'width', 'height', 'exportWidth', 'exportHeight', 'selection', 'visuallyReviewed']}
            record['artwork'][role]['author'] = a['author']['name']
        compact.append(record)
    unresolved = json.loads((ROOT / 'unmatched-nintendo.json').read_text())
    compact_unmatched = [{'nintendoId': x['nintendo']['fs_id'], 'title': x['nintendo']['title'],
        'sourceUrl': 'https://www.nintendo.com' + x['nintendo']['url'], 'method': x['method'],
        'queries': x.get('queries', []), 'candidates': [{'id': c['id'], 'title': c['name']} for c in x.get('candidates', [])]}
        for x in unresolved]
    request_rows = []
    for path in sorted((ROOT / 'raw').glob('*.json')):
        if len(path.stem) != 64:
            continue
        request = json.loads(path.read_text())
        response = request['response']
        data = response.get('data')
        count = len(data) if isinstance(data, list) else len(data.get('assets', [])) if isinstance(data, dict) else len(response.get('response', {}).get('docs', []))
        request_rows.append({'cacheKey': path.stem, 'url': request['url'], 'body': request['body'],
            'fetchedAt': request['fetchedAt'], 'returnedCount': count})
    coverage.update({'exportedAt': datetime.now(timezone.utc).isoformat(),
        'coversAvailable': sum(bool(g['coverPath']) for g in catalog),
        'heroesAvailable': sum(bool(g['heroPath']) for g in catalog),
        'coversAbsent': sum(g['coverPath'] is None for g in catalog),
        'heroesAbsent': sum(g['heroPath'] is None for g in catalog),
        'artworkErrors': {gid: a['errors'] for gid, a in artwork.items() if a['errors']},
        'requestCount': len(request_rows),
        'firstFetchedAt': min(r['fetchedAt'] for r in request_rows),
        'lastFetchedAt': max(r['fetchedAt'] for r in request_rows),
        'artworkSelection': 'Public score_desc first page, static only; safe flags filtered; English preferred; ten manually checked cover pins',
        'validation': 'Every local asset decoded and dimensions checked. Representative contact sheets reviewed separately, not all artwork.'})
    save('coverage.json', coverage)
    save('handoff/catalog.json', catalog)
    save('handoff/provenance.json', compact)
    save('handoff/unmatched-nintendo.json', compact_unmatched)
    save('handoff/coverage.json', coverage)
    save('handoff/source-coverage.json', source)
    save('request-log.json', request_rows)
    priorities = [5483943, 5483961, 5483963, 5483969, 5483968, 5483959, 5484010, 5209422, 5484094, 5504602]
    chosen = [g for gid in priorities for g in catalog if g['sgdbId'] == gid]
    chosen += [g for g in catalog if g['sgdbId'] not in priorities and g['coverPath']][::max(1, len(catalog)//18)][:14]
    font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', 12)
    w, h, columns = 180, 306, 6
    sheet = Image.new('RGB', (w * columns, h * ((len(chosen)+columns-1)//columns)), 'white')
    draw = ImageDraw.Draw(sheet)
    for index, game in enumerate(chosen):
        x, y = (index % columns) * w, (index // columns) * h
        title = game['title']
        draw.text((x+3, y+2), title[:23], font=font, fill='black')
        draw.text((x+3, y+16), title[23:46], font=font, fill='black')
        if game['coverPath']:
            with Image.open(ROOT / game['coverPath'].lstrip('/')) as image:
                sheet.paste(image.convert('RGB').resize((180,270)), (x,y+36))
    sheet.save(ROOT / 'representative-covers.jpg', quality=90)
    print(json.dumps(coverage, indent=2))


if __name__ == '__main__':
    main()
