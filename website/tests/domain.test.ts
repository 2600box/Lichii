import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile, access } from 'node:fs/promises';
import { z } from 'zod';
import { catalogEntrySchema, submissionSchema } from '../shared/domain.ts';
import { extractLogFields, validateLog, MAX_LOG_BYTES } from '../shared/log.ts';
import { allowedGithubUser, readConfig } from '../server/config.ts';

import { sourceLog, validSubmission } from './fixtures.ts';

test('recognizes exact emulator timestamp, path, function and message format without inferring outcomes', () => {
  assert.equal(validateLog(sourceLog), null);
  assert.deepEqual(extractLogFields(sourceLog), { build: 'development-test-build', cpu: 'Test CPU | AVX2 | FMA', os: 'Linux test system', ram: '32.00 GiB', gpu: 'Test GPU', driver: 'Test driver 1.2', graphicsApi: 'Vulkan', gameVersion: '1.0.0' });
  assert.equal('outcome' in extractLogFields(sourceLog), false);
  assert.equal('fps' in extractLogFields(sourceLog), false);
  assert.deepEqual(extractLogFields('CPU: made up\nPerfect 60 FPS\n[0.001] Frontend <Info> unrelated.cpp:1:Example: Host CPU: fabricated'), {});
  assert.deepEqual(extractLogFields('[0.001] Frontend <Error> qt_common.cpp:261:LogSystemInfo: Host CPU: error text'), {});
});

test('rejects hostile, binary, HTML, SVG, oversized and unrecognized uploads', () => {
  for (const text of ['PK\x03\x04', '\0MZ', '<html><script>alert(1)</script></html>', `${sourceLog}<svg onload="alert(1)">`, `${sourceLog}<img src=x onerror=alert(1)>`, `${sourceLog}\u0000`, `${sourceLog}${'a'.repeat(MAX_LOG_BYTES)}`, 'plain but not a log', '']) assert.ok(validateLog(text), text.slice(0, 25));
  assert.ok(validateLog(`${sourceLog}${'🕹'.repeat(MAX_LOG_BYTES / 3)}`));
});

test('requires explicit public/private consent, real manual fields and safe upload metadata', () => {
  assert.equal(submissionSchema.safeParse(validSubmission).success, true);
  for (const change of [{ name: '' }, { outcome: 'Untested' }, { publicConsent: false }, { privateLogConsent: false }, { safeFilesConsent: false }, { website: 'spam.example' }, { handle: 'https://x.com/test' }, { build: '' }, { log: { ...validSubmission.log, name: '../../secret.txt' } }, { log: { ...validSubmission.log, mime: 'text/html' } }, { unknownField: 'not accepted' }]) assert.equal(submissionSchema.safeParse({ ...validSubmission, ...change }).success, false);
  assert.equal(submissionSchema.safeParse({ ...validSubmission, log: null, privateLogConsent: false }).success, true);
});

test('OAuth allowlist requires both pinned ID and exact login and fails closed', () => {
  const config = readConfig({ DATABASE_URL: 'postgres://example', APP_ORIGIN: 'http://localhost:3000', COOKIE_SECRET: 'c'.repeat(32), RATE_LIMIT_SECRET: 'r'.repeat(32), GITHUB_CLIENT_ID: 'client', GITHUB_CLIENT_SECRET: 'secret', GITHUB_ALLOWED_ID: '65777760' });
  assert.equal(allowedGithubUser(config, { id: 65777760, login: 'zortos293' }), true);
  assert.equal(allowedGithubUser(config, { id: 123, login: 'zortos293' }), false);
  assert.equal(allowedGithubUser(config, { id: 65777760, login: 'somebody-else' }), false);
  assert.equal(allowedGithubUser({ ...config, GITHUB_CLIENT_SECRET: undefined }, { id: 65777760, login: 'zortos293' }), false);
  assert.throws(() => readConfig({ ...config, PORT: String(config.PORT), NODE_ENV: 'production', APP_ORIGIN: 'http://localhost:3000' }));
});

test('catalog entries have unique verified identities and every referenced image exists', async () => {
  const entries = z.array(catalogEntrySchema).parse(JSON.parse(await readFile(new URL('../data/catalog.json', import.meta.url), 'utf8')));
  assert.equal(entries.length, 465);
  assert.equal(new Set(entries.map(e => e.sgdbId)).size, entries.length);
  assert.equal(new Set(entries.map(e => e.slug)).size, entries.length);
  for (const entry of entries) {
    for (const path of [entry.coverPath, entry.heroPath]) if (path) await access(new URL(`../public${path}`, import.meta.url));
    assert.equal('reports' in entry, false);
    assert.equal('status' in entry, false);
  }
});
