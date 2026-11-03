# Nintendo Switch 2 catalogue export

This is an intersection of Nintendo Europe's English Switch 2 index and matched SteamGridDB entries. It is not a worldwide exhaustive Switch 2 list. SGDB has no console-platform catalogue endpoint. Its official API's `platform` means storefront IDs, with no Switch 2 value. An API key does not supply missing platform membership.

## Import into the website

Copy `handoff/catalog.json` into the site's catalogue data and the contents of `art/` into its public `art/` directory. Every path in the catalogue is `/art/<sgdbId>-cover.webp` or `/art/<sgdbId>-hero.webp`; absent art is `null`. Copy `handoff/provenance.json`, `handoff/coverage.json`, and `handoff/source-coverage.json` if the website retains attribution and coverage evidence. `handoff/unmatched-nintendo.json` is a separate review queue, not games to import.

Do not copy `raw/`, full match responses, logs, or contact sheets into website data. No reports, testing outcomes, ratings, or compatibility claims are seeded. SGDB is an artwork source, not compatibility evidence.

`catalog.json` is an array with `sgdbId`, `slug`, `title`, `coverPath`, `heroPath`, `artworkCredit`, and `artworkSource`. `releaseDate` and `publisher` are present only when Nintendo supplies unambiguous metadata. Titles use Nintendo's edition names, not generic SGDB names. Source timestamps from SGDB are not Switch 2 release dates. Multiple Nintendo records can share one SGDB identity; provenance retains all membership evidence.

## Reproduce

Requires Python 3 and Pillow. Run from this directory:

```sh
python3 discover.py
python3 refine.py
python3 artwork.py
python3 verify_export.py
```

The scripts write only beside themselves. Requests are cached by URL and JSON body in `raw/<sha256>.json`. Repeat runs reuse responses and downloaded files. For a fresh upstream snapshot, use a new empty directory containing these four scripts. A cached run reproduces that snapshot, not today's upstream state.

Discovery exhausts Nintendo's reported total using pages of 100, verifies stable totals, unique Nintendo IDs, and Switch 2 membership on every record. The query is `type:GAME AND system_type:nintendoswitch2`, not the broader Switch wildcard. `source-coverage.json` records every page and `request-log.json` records each query, time, and response count.

Matching accepts unique normalized exact titles, platform-label removal for a shared SGDB edition entry, exact-name duplicate disambiguation by Nintendo release year, or explicit reviewed edition/arcade-port aliases. Fuzzy search candidates are never accepted automatically. Multiple matches stay unresolved. Generic artwork can depict the original release when SGDB uses a shared ID for a Switch 2 port; that does not change the Nintendo edition being catalogued.

Artwork uses the website's observed public `search/assets` endpoint, with `score_desc`, static files, and explicit NSFW/humor/epilepsy exclusions. Selection is from the first 24 filtered results, preferring English, not an exhaustive aesthetic comparison. Ten priority covers are pinned to previously visually reviewed assets. Other assets are decoded and dimension-checked, but not all manually viewed. Full-sized CDN originals are converted to quality-85 WebP; heroes are bounded to 1920 pixels wide. Asset IDs, original URLs, and authors remain in provenance. No source image URL is fabricated.

## Limits

- Nintendo's regional index is authoritative for its own records, not worldwide availability. It includes announced titles and may classify editions or bundles as games.
- SGDB autocomplete is ranked and has no exposed pagination. Unmatched means unresolved, not proven absent from SGDB.
- Nintendo and SGDB change over time; the export records its snapshot and does not promise continuous sync.
- Null artwork is honest missing data. Do not substitute another game's image.
- Community artwork attribution does not grant copyright clearance. Preserve source credits and assess use separately.
