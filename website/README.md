# Lichii compatibility website

A standalone Switch 2 community report site. All application files live in `website/`; the emulator is unchanged.

The launch follows the [dark Paper design](https://app.paper.design/file/01M6GQ17RX2F3XN1N9H2Z01KMA/p-2-0), using self-hosted [Paper Mono](https://paper.design/mono) throughout. The font comes from [paper-design/paper-mono at e6eaece](https://github.com/paper-design/paper-mono/tree/e6eaeceaef02e77e3db997711e07a16378de2bd7) under the SIL Open Font License 1.1, included at `public/fonts/OFL.txt`. The database starts with **465 catalog entries and zero reports**. Every title is untested until a real report is submitted. There are no generated players, statistics, performance figures or log files.

## Run locally

Requires Node.js 24 and PostgreSQL 16 or later.

1. Run `npm ci` in `website/`.
2. Copy `.env.example` to `.env`. Set the database URL and generate two different secrets with `openssl rand -hex 32`.
3. Keep `APP_ORIGIN=http://localhost:3000` and `TRUST_PROXY=false` for local use.
4. Run `npm run build`, then `npm start`.

The server binds `0.0.0.0` and reads `PORT`, defaulting to 3000. Startup runs migrations, imports the checked-in catalog and removes expired private data. `npm run dev` watches server changes; rebuild the frontend and restart the server after changing frontend files. `.env` is loaded by the start and dev commands and is excluded from git and Docker.

Migrations and imports share a PostgreSQL transaction advisory lock. They are safe to run concurrently and on every restart. Imports upsert catalog metadata by SteamGridDB ID and never create, replace or delete reports. `npm run db:prepare` runs the same preparation separately. It does not require the HTTP server.

## Deploy on Railway

- Use `website` as the service root and Docker build context. `Dockerfile` builds the frontend and runs the server as the non-root `node` user.
- Attach PostgreSQL and set `DATABASE_URL` to its private connection string. Keep database traffic on the private network. Use a verified TLS configuration if connecting over a public database endpoint; the app never disables certificate verification.
- Set `APP_ORIGIN` to the exact HTTPS origin without a trailing slash, such as `https://your-service.up.railway.app`.
- Set `COOKIE_SECRET` and `RATE_LIMIT_SECRET` to separate random values of at least 32 characters.
- Set `NODE_ENV=production` and `TRUST_PROXY=true` only behind Railway’s edge.
- Use `/api/health` as the health check. It queries PostgreSQL and returns 503 if the database is unavailable.
- No volume is needed for this app. Reports, private log bytes, sessions, rate limits and audits are all in PostgreSQL.
- No separate predeploy command is needed. Startup prepares the database under a lock before accepting traffic. Configure database backups separately according to the operator’s retention policy.

Configure the service in Railway’s dashboard or API with root directory `/website`, Dockerfile auto-detection, health check `/api/health`, a 120-second health-check timeout, and restart policy `ON_FAILURE` with three retries. New Railway services no longer accept `railway.json` config-as-code. A CLI source upload should upload the `website` directory, not the emulator repository.

### GitHub OAuth

Create a GitHub OAuth App with the callback:

```text
https://YOUR_ORIGIN/api/auth/callback
```

Set `GITHUB_CLIENT_ID`, `GITHUB_CLIENT_SECRET`, `GITHUB_ALLOWED_ID=65777760` and `GITHUB_ALLOWED_LOGIN=zortos293`. The login is deliberately restricted to this account. The immutable numeric ID and current login must both match the identity returned by GitHub’s authenticated `/user` endpoint. Never substitute the username for the ID check.

Missing OAuth configuration disables maintainer access rather than bypassing authentication. The public site remains usable. Real OAuth testing requires the configured callback origin and the account owner’s browser. Local tests verify the flow’s state, PKCE, cookie and allowlist logic without using production credentials.

The application uses `oauth4webapi` for authorization response validation and the PKCE-protected code exchange. OAuth attempts are signed, browser-bound and single-use, with a ten-minute expiry. Session tokens are cryptographically random, stored hashed in PostgreSQL, expire after eight hours, and use Secure/HttpOnly/SameSite=Lax cookies in production. Every private route independently checks the current configured allowlist. OAuth access tokens are not stored or returned to the browser. Redirect destinations are fixed.

### Trusted proxy boundary

Fastify does not trust `X-Forwarded-For`. In Railway mode, rate limits use only the single `X-Real-IP` address overwritten by Railway’s edge, validated with Node’s IP parser. Missing or invalid values fail closed for API requests, except the health check. Local mode ignores all forwarded headers and uses the connection address.

Only Railway’s edge and trusted services in the private project may reach the app in this mode. Do not expose its container port directly or put an untrusted private-network service beside it. Such a caller could set `X-Real-IP`; this is a deployment trust boundary, not a portable proxy setting. Reverify the edge behavior before moving to another hosting provider. Railway’s guidance: [trusted proxy configuration](https://station.railway.com/questions/engineering-confirmation-secure-proxy-t-d05079b3).

## Test

Use a separate PostgreSQL database named `lichii_test`. The integration test **drops and recreates its public schema** and refuses other database names. Do not point it at production or a development database containing work.

```sh
npm run check
TEST_DATABASE_URL=postgresql://USER:PASSWORD@127.0.0.1:5432/lichii_test npm test
npm run build
npm audit
```

Tests run migrations and real parameterized queries. They cover repeat/concurrent imports, zero reports, search, report persistence, private/public separation, malformed uploads, MIME/size limits, CSRF, SQL-injection-shaped search, OAuth state and PKCE, immutable-ID rejection, expired/forged sessions, every privileged endpoint, audit events, triage/hiding, log deletion/expiry, and durable rate limits with spoofed forwarding headers.

## Catalog and artwork

`data/catalog.json` is the validated import contract:

```ts
{
  sgdbId: number;
  slug: string;
  title: string;
  coverPath: string | null;
  heroPath: string | null;
  artworkCredit: string | null;
  artworkSource: string | null;
  releaseDate?: string | null;
  publisher?: string | null;
}
```

Artwork paths must be local `/art/NAME.webp`, `.png`, `.jpg` or `.jpeg` paths. All imported artwork is decoded and reencoded to WebP by the collection scripts, rather than serving upstream HTML or arbitrary original files. Missing art remains missing, with a neutral placeholder. Real Lichii branding is copied from the project’s branding assets, with no color inversion.

The snapshot matches Nintendo Europe’s English Switch 2 index against SteamGridDB. It covers 466 Nintendo records mapped to 465 unique SGDB IDs; 226 Nintendo records remain unresolved. The catalog includes announced games and some editions or bundles. It is **not an exhaustive worldwide Switch 2 list**. SteamGridDB has no console-platform enumeration endpoint, so unresolved records are not proven absent there. The UI states this limit.

`data/provenance.json` preserves Nintendo membership, artwork IDs, authors and original URLs. `coverage.json`, `source-coverage.json` and `unmatched-nintendo.json` record coverage and the unresolved review queue. These files are not public API payloads. Art credits are shown per game. Shared SGDB entries may have artwork from an original edition rather than a dedicated Switch 2 cover. Attribution does not itself provide copyright clearance.

See `scripts/catalog/README.md` to reproduce discovery, matching and image conversion. Regenerate deliberately, review the unmatched queue and artwork, replace the checked-in export, then deploy. There is no automatic upstream sync and no API-key dependency at runtime.

## Reporting and maintenance

1. Pick a game and choose **Submit a report**.
2. Optionally select `eden_log.txt`, found through **File → Open Lichii Folders → Log Folder**. It is still named `eden_log.txt` in the emulator.
3. Read the local preview and remove personal data in a text editor if necessary. Apply detected fields only after review.
4. Enter missing build, game version, hardware, settings, outcome, progress, summary and reproduction details. Unknown information must be marked unknown, not guessed.
5. Enter a name and optional X handle. Consent to the public report and, if attached, the private log. Submit once.

The parser only recognizes known source-file log lines for build, CPU, OS, RAM, Vulkan device/driver/API and booted game version. Its timestamp and function-name syntax match `src/common/logging.cpp`. It does not infer compatibility, performance or FPS. Settings and outcome remain manual. Parsed values are untrusted observations, never authenticated telemetry.

The public report contains only explicitly entered fields. Logs are separate PostgreSQL `bytea` rows and have no public URL, filename or API field. Maintainers sign in at `/maintainer`, select a report, view or download a private log, save a triage state/internal note, hide spam, or delete a log. Private reads and mutations create audits without log contents.

Logs become inaccessible at 90 days. Hourly and startup cleanup deletes expired rows from the live database. Public reports remain. Backups have their own infrastructure retention policy. The privacy page explains the distinction. There are no author accounts or self-service editing tokens; removal requests go to the maintainer.

## Launch security and scope

- JSON bodies are bounded before parsing to 1,100 KiB. Logs are at most 512 KiB of UTF-8 text, checked both in the browser and on the server, with a database byte-length constraint.
- Archives, binaries, HTML/SVG, unexpected content types and malformed report fields are rejected. Uploaded filenames are validated then discarded. Screenshot uploads are intentionally not supported.
- React escapes all report/log content. There is no raw HTML rendering. CSP permits only local scripts/styles/images/fonts and prohibits embedding, objects and external connections. Additional headers include `nosniff`, no-referrer, HSTS in production and a restrictive Permissions Policy.
- All mutations require the exact configured Origin and JSON content type. Cross-site Fetch Metadata is rejected. SQL uses parameterized queries; the only raw SQL is the checked-in migration file.
- Submission attempts are limited to five per IP per hour, OAuth starts to twenty per IP per hour, and other APIs to 240 requests per IP per minute. A global 1,000-report daily limit bounds storage abuse. Limits are atomic and durable across restarts. IP-derived keys are HMAC hashes, not raw addresses, and expire.
- The application has no analytics and suppresses request/body/error serialization that could disclose logs, tokens or queries. Error responses do not expose stack traces or database errors.
- Anonymous content is still untrusted. Rate limits and a honeypot reduce abuse but cannot stop distributed determined spam. The maintainer can hide reports. No automatic consensus, deduplication, issue matching, voting, benchmark inference or screenshot service is claimed.

## Code map

- `shared/domain.ts`: Zod boundary schemas and shared report/catalog types.
- `shared/log.ts`: bounded log validation and conservative local extraction.
- `server/app.ts`: HTTP security boundary, public APIs and protected maintainer APIs.
- `server/auth.ts`: GitHub OAuth and database-backed sessions.
- `server/db.ts` and `server/migrations/`: locked preparation, import and retention.
- `src/`: React routes, accessible forms and dark responsive Tailwind UI.
- `tests/`: pure validation/parser checks and isolated PostgreSQL integration tests.
