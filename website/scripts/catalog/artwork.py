#!/usr/bin/env python3
import concurrent.futures
import io
import json
import urllib.parse
import urllib.request
from datetime import datetime, timezone

from PIL import Image

from discover import ROOT, SGDB, fetch, save

PINNED_COVERS = {5483943: 557479, 5483961: 584982, 5483963: 653603, 5483969: 647697,
                 5483968: 703696, 5483959: 650166, 5484010: 557822, 5209422: 761611,
                 5484094: 616681, 5504602: 803583}


def select(gid, kind):
    body = {'asset_type': kind, 'game_id': [gid], 'page': 0, 'limit': 24, 'order': 'score_desc',
            'static': True, 'animated': False, 'nsfw': False, 'humor': False, 'epilepsy': False,
            'untagged': True, 'dimensions': ['600x900'] if kind == 'grid' else ['all']}
    page = fetch(SGDB + '/api/public/search/assets', body)
    if not page.get('success'):
        raise ValueError(page)
    data = page['data']
    candidates = [a for a in data['assets'] if not any(a.get(k) for k in
        ['nsfw', 'humor', 'epilepsy', 'is_animated', 'is_deleted', 'processing'])
        and a['mime'] in ['image/png', 'image/jpeg', 'image/webp']
        and (kind == 'hero' and a['width'] > a['height'] or kind == 'grid' and a['width'] * 3 == a['height'] * 2)]
    pinned = kind == 'grid' and gid in PINNED_COVERS
    if pinned:
        candidate = fetch(f'{SGDB}/api/public/asset/grid/{PINNED_COVERS[gid]}')['data']['asset']
        assert candidate['game']['id'] == gid
        assert not any(candidate.get(k) for k in ['nsfw', 'humor', 'epilepsy', 'is_animated', 'is_deleted'])
        assert candidate['width'] * 3 == candidate['height'] * 2
        candidates.insert(0, candidate)
    if not candidates:
        return None
    selected = next((a for a in candidates if a.get('language') in ['en', None]), candidates[0])
    url = selected['url']
    if urllib.parse.urlparse(url).hostname != 'cdn2.steamgriddb.com':
        raise ValueError('Unexpected artwork host')
    role = 'cover' if kind == 'grid' else 'hero'
    file = ROOT / 'art' / f'{gid}-{role}.webp'
    file.parent.mkdir(exist_ok=True)
    if not file.exists():
        request = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
        with urllib.request.urlopen(request, timeout=60) as response:
            image = Image.open(io.BytesIO(response.read()))
        image.load()
        if getattr(image, 'is_animated', False):
            raise ValueError('Unexpected animated image')
        original_size = image.size
        if original_size != (selected['width'], selected['height']):
            raise ValueError('Image dimensions disagree with source metadata')
        image = image.convert('RGB')
        image.thumbnail((600, 900) if role == 'cover' else (1920, 1080))
        image.save(file, 'WEBP', quality=85)
    with Image.open(file) as image:
        saved_size = image.size
    return {'assetId': selected['id'], 'assetType': kind, 'author': selected['author'], 'sourceUrl': url,
            'assetPage': f'{SGDB}/{kind}/{selected["id"]}', 'gamePage': f'{SGDB}/game/{gid}',
            'path': '/art/' + file.name, 'width': selected['width'], 'height': selected['height'],
            'exportWidth': saved_size[0], 'exportHeight': saved_size[1], 'sourceMime': selected['mime'],
            'selection': 'Previously visually verified portrait' if pinned else 'First eligible public highest-score artwork, preferring English/unlabelled; not a manual aesthetic judgement',
            'query': body, 'candidateCount': len(candidates), 'totalFilteredAssets': data['total'],
            'pagesInspected': 1, 'derivedFormat': 'WebP, quality 85; hero max width 1920', 'visuallyReviewed': bool(pinned)}


def process(item):
    result = {'sgdbId': item['sgdbId'], 'cover': None, 'hero': None, 'errors': []}
    for kind, role in [('grid', 'cover'), ('hero', 'hero')]:
        try:
            result[role] = select(item['sgdbId'], kind)
        except Exception as error:
            result['errors'].append({'role': role, 'error': str(error)})
    return result


def main():
    catalog = json.loads((ROOT / 'catalog.json').read_text())
    existing = ROOT / 'artwork-provenance.json'
    provenance = json.loads(existing.read_text()) if existing.exists() else {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        futures = [pool.submit(process, x) for x in catalog if str(x['sgdbId']) not in provenance or provenance[str(x['sgdbId'])].get('errors')]
        for i, future in enumerate(concurrent.futures.as_completed(futures), 1):
            result = future.result()
            provenance[str(result['sgdbId'])] = result
            if i % 10 == 0:
                print(f'ART {len(provenance)}/{len(catalog)}', flush=True)
                save('artwork-provenance.json', provenance)
    for item in catalog:
        p = provenance.get(str(item['sgdbId']), {})
        credits = []
        for role in ['cover', 'hero']:
            a = p.get(role)
            if a:
                item[role + 'Path'] = a['path']
                credits.append(role.capitalize() + ': ' + a['author']['name'] + ' / SteamGridDB')
        item['artworkCredit'] = '; '.join(credits) or None
        item['artworkSource'] = SGDB + '/game/' + str(item['sgdbId']) if credits else None
    save('catalog.json', catalog)
    save('artwork-provenance.json', provenance)
    print(f'ART COMPLETE covers={sum(bool(x["coverPath"]) for x in catalog)} heroes={sum(bool(x["heroPath"]) for x in catalog)}', flush=True)


if __name__ == '__main__':
    main()
