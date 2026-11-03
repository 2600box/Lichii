import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { connectDatabase, prepareDatabase, cleanup } from '../server/db.ts';
import { createApp } from '../server/app.ts';
import { readConfig } from '../server/config.ts';
import { hashToken } from '../server/auth.ts';
import { validSubmission, sourceLog } from './fixtures.ts';

const databaseUrl = process.env.TEST_DATABASE_URL;
if (!databaseUrl || !new URL(databaseUrl).pathname.endsWith('/lichii_test')) throw new Error('Set TEST_DATABASE_URL to an isolated PostgreSQL database named lichii_test. Tests delete that database schema.');
const sql = connectDatabase(databaseUrl);
const config = readConfig({ DATABASE_URL: databaseUrl, APP_ORIGIN: 'http://localhost:3000', COOKIE_SECRET: 'test-cookie-secret'.repeat(3), RATE_LIMIT_SECRET: 'test-rate-limit-secret'.repeat(3), NODE_ENV: 'test', TRUST_PROXY: 'true', GITHUB_CLIENT_ID: 'test-client', GITHUB_CLIENT_SECRET: 'test-secret', GITHUB_ALLOWED_ID: '65777760' });
const app = await createApp(config, sql, false);
let sequence = 1;
const headers = () => ({ origin: config.APP_ORIGIN, 'content-type': 'application/json', 'x-real-ip': `198.51.${Math.floor(sequence / 250)}.${sequence++ % 250 + 1}` });
const token = randomBytes(32).toString('hex');
const cookie = `lichii-session=${token}`;

before(async () => {
  await sql`DROP SCHEMA public CASCADE`;
  await sql`CREATE SCHEMA public`;
  await prepareDatabase(sql);
  await app.ready();
});
after(async () => { await app.close(); await sql.end(); });

test('real PostgreSQL migrations/import are concurrent-safe, idempotent and seed zero reports', async () => {
  await Promise.all([prepareDatabase(sql), prepareDatabase(sql)]);
  const response = await app.inject({ method: 'GET', url: '/api/games', headers: headers() });
  assert.equal(response.statusCode, 200);
  const data = response.json();
  assert.equal(data.totalGames, 465); assert.equal(data.totalReports, 0); assert.equal(data.counts.Untested, 465);
  assert.equal(data.games.length, 15); assert.equal(data.pageCount, 31);
  assert.ok(data.games.every((g: { status: string; reportCount: number }) => g.status === 'Untested' && g.reportCount === 0));
  assert.equal((await sql`SELECT * FROM schema_migrations`).length, 1);
  const filtered = await app.inject({ url: '/api/games?q=Mario%20Kart%20World&status=Untested', headers: headers() });
  assert.equal(filtered.json().total, 1);
  const attack = await app.inject({ url: "/api/games?q='OR%201=1--", headers: headers() });
  assert.equal(attack.json().total, 0);
  assert.equal((await app.inject({ url: '/api/health' })).statusCode, 200);
});

test('anonymous submissions persist but public APIs never expose log contents, filenames or notes', async () => {
  const response = await app.inject({ method: 'POST', url: '/api/reports', headers: headers(), payload: validSubmission });
  assert.equal(response.statusCode, 201, response.body);
  const id = response.json().id;
  const publicResponse = await app.inject({ url: `/api/reports/${id}`, headers: headers() });
  assert.equal(publicResponse.statusCode, 200);
  assert.equal(publicResponse.json().name, validSubmission.name);
  assert.equal(publicResponse.json().outcome, 'Runs');
  for (const secret of ['eden_log.txt', 'development-test-build', 'privateLogConsent', 'hasLog', 'internalNote']) assert.equal(publicResponse.body.includes(secret), false);
  const logs = await sql`SELECT content, expires_at FROM private_logs WHERE report_id = ${id}`;
  assert.equal(logs[0]?.content.toString('utf8'), sourceLog);
  const aggregate = await app.inject({ url: '/api/games?q=Mario%20Kart%20World', headers: headers() });
  assert.equal(aggregate.json().games[0].reportCount, 1); assert.equal(aggregate.json().games[0].status, 'Runs');
  await sql`DELETE FROM reports WHERE id = ${id}`;
});

test('malicious uploads, invalid forms, CSRF, request size and wrong MIME are rejected before storage', async () => {
  for (const change of [{ log: { ...validSubmission.log, text: `${sourceLog}<script>alert(1)</script>` } }, { log: { ...validSubmission.log, text: `${sourceLog}\u0000PK` } }, { log: { ...validSubmission.log, name: 'firmware.zip' } }, { log: { ...validSubmission.log, mime: 'image/svg+xml' } }, { name: '' }, { outcome: 'Untested' }, { gameId: 999999999 }, { publicConsent: false }]) {
    const response = await app.inject({ method: 'POST', url: '/api/reports', headers: headers(), payload: { ...validSubmission, ...change } });
    assert.equal(response.statusCode, 400, response.body);
  }
  assert.equal((await app.inject({ method: 'POST', url: '/api/reports', headers: { ...headers(), origin: 'https://evil.example' }, payload: validSubmission })).statusCode, 403);
  assert.equal((await app.inject({ method: 'POST', url: '/api/reports', headers: { ...headers(), 'content-type': 'text/plain' }, payload: 'attack' })).statusCode, 415);
  assert.equal((await app.inject({ method: 'POST', url: '/api/reports', headers: headers(), payload: JSON.stringify({ ...validSubmission, details: 'x'.repeat(1200 * 1024) }) })).statusCode, 413);
  assert.equal((await sql`SELECT * FROM reports`).length, 0);
});

test('all privileged endpoints reject anonymous, forged, expired and non-allowlisted sessions', async () => {
  const id = '00000000-0000-4000-8000-000000000000';
  for (const [method, url] of [['GET', '/api/maintainer/reports'], ['GET', `/api/maintainer/reports/${id}/log`], ['PATCH', `/api/maintainer/reports/${id}`], ['DELETE', `/api/maintainer/reports/${id}/log`]] satisfies ['GET' | 'PATCH' | 'DELETE', string][]) {
    const response = await app.inject({ method, url, headers: headers(), ...(method !== 'GET' ? { payload: {} } : {}) });
    assert.equal(response.statusCode, 401);
  }
  assert.equal((await app.inject({ url: '/api/maintainer/reports', headers: { ...headers(), cookie: 'lichii-session=forged' } })).statusCode, 401);
  await sql`INSERT INTO sessions(token_hash, github_id, github_login, expires_at) VALUES (${hashToken(token)}, '123', 'zortos293', now() + interval '1 hour')`;
  assert.equal((await app.inject({ url: '/api/maintainer/reports', headers: { ...headers(), cookie } })).statusCode, 403);
  await sql`UPDATE sessions SET github_id = '65777760', github_login = 'other'`;
  assert.equal((await app.inject({ url: '/api/maintainer/reports', headers: { ...headers(), cookie } })).statusCode, 403);
  await sql`UPDATE sessions SET github_login = 'zortos293', expires_at = now() - interval '1 second'`;
  assert.equal((await app.inject({ url: '/api/maintainer/reports', headers: { ...headers(), cookie } })).statusCode, 403);
  const disabled = await createApp({ ...config, GITHUB_CLIENT_SECRET: undefined }, sql, false);
  assert.equal((await disabled.inject({ url: '/api/maintainer/reports', headers: { ...headers(), cookie } })).statusCode, 503);
  await disabled.close();
});

test('OAuth state and PKCE protect the callback and redirect destinations are fixed', async () => {
  const start = await app.inject({ url: '/api/auth/login?redirect=https://evil.example', headers: headers() });
  assert.equal(start.statusCode, 302);
  const url = new URL(String(start.headers.location));
  assert.equal(url.origin, 'https://github.com'); assert.equal(url.searchParams.get('code_challenge_method'), 'S256');
  assert.equal(url.searchParams.get('redirect_uri'), `${config.APP_ORIGIN}/api/auth/callback`);
  assert.ok(url.searchParams.get('code_challenge')); assert.ok(url.searchParams.get('state'));
  const setCookie = String(start.headers['set-cookie']);
  assert.ok(setCookie.includes('HttpOnly')); assert.ok(setCookie.includes('SameSite=Lax'));
  const missing = await app.inject({ url: '/api/auth/callback?code=fake&state=fake', headers: headers() });
  assert.equal(missing.headers.location, '/maintainer?error=sign-in-failed');
  const mismatch = await app.inject({ url: '/api/auth/callback?code=fake&state=wrong', headers: { ...headers(), cookie: setCookie.split(';')[0] } });
  assert.equal(mismatch.headers.location, '/maintainer?error=sign-in-failed');
  assert.equal((await sql`SELECT * FROM sessions WHERE expires_at > now()`).length, 0);
});

test('authorized triage, private download, audit, hiding, deletion and retention operate on real rows', async () => {
  await sql`UPDATE sessions SET github_id = '65777760', github_login = 'zortos293', expires_at = now() + interval '1 hour' WHERE token_hash = ${hashToken(token)}`;
  const response = await app.inject({ method: 'POST', url: '/api/reports', headers: headers(), payload: validSubmission });
  const id = response.json().id;
  const queue = await app.inject({ url: '/api/maintainer/reports', headers: { ...headers(), cookie } });
  assert.equal(queue.statusCode, 200); assert.equal(queue.json().reports[0].hasLog, true);
  const log = await app.inject({ url: `/api/maintainer/reports/${id}/log`, headers: { ...headers(), cookie } });
  assert.equal(log.body, sourceLog); assert.equal(log.headers['content-disposition'], 'attachment; filename="eden_log.txt"');
  assert.equal(log.headers['cache-control'], 'no-store'); assert.equal(log.headers['x-content-type-options'], 'nosniff');
  const update = await app.inject({ method: 'PATCH', url: `/api/maintainer/reports/${id}`, headers: { ...headers(), cookie }, payload: { triage: 'hidden', internalNote: 'PRIVATE INTERNAL NOTE' } });
  assert.equal(update.statusCode, 200);
  assert.equal((await app.inject({ url: `/api/reports/${id}`, headers: headers() })).statusCode, 404);
  assert.equal((await app.inject({ url: '/api/games?q=Mario%20Kart%20World', headers: headers() })).json().games[0].status, 'Untested');
  const audit = await sql`SELECT action FROM audit_events WHERE report_id = ${id} ORDER BY id`;
  assert.deepEqual(audit.map(row => row.action), ['read-log', 'triage']);
  await sql`UPDATE private_logs SET expires_at = now() - interval '1 second' WHERE report_id = ${id}`;
  assert.equal((await app.inject({ url: `/api/maintainer/reports/${id}/log`, headers: { ...headers(), cookie } })).statusCode, 404);
  await cleanup(sql); assert.equal((await sql`SELECT * FROM private_logs`).length, 0);
  await sql`INSERT INTO private_logs(report_id, content) VALUES (${id}, ${Buffer.from(sourceLog)})`;
  const deletion = await app.inject({ method: 'DELETE', url: `/api/maintainer/reports/${id}/log`, headers: { ...headers(), cookie }, payload: {} });
  assert.equal(deletion.statusCode, 204); assert.equal((await sql`SELECT * FROM private_logs`).length, 0);
  await sql`DELETE FROM reports WHERE id = ${id}`;
});

test('durable limits trust only valid Railway X-Real-IP and ignore spoofed forwarded chains', async () => {
  const stable = { ...headers(), 'x-real-ip': '203.0.113.40' };
  for (let i = 0; i < 5; i++) {
    const response = await app.inject({ method: 'POST', url: '/api/reports', headers: { ...stable, 'x-forwarded-for': `1.1.1.${i}` }, payload: { ...validSubmission, name: '' } });
    assert.equal(response.statusCode, 400);
  }
  const limited = await app.inject({ method: 'POST', url: '/api/reports', headers: { ...stable, 'x-forwarded-for': '8.8.8.8' }, payload: validSubmission });
  assert.equal(limited.statusCode, 429);
  assert.equal((await app.inject({ url: '/api/games', headers: { 'x-forwarded-for': '8.8.8.8' } })).statusCode, 400);
  assert.equal((await app.inject({ url: '/api/games', headers: { 'x-real-ip': '8.8.8.8, 1.1.1.1' } })).statusCode, 400);
  const local = await createApp({ ...config, TRUST_PROXY: 'false' }, sql, false);
  for (let i = 0; i < 6; i++) {
    const response = await local.inject({ method: 'POST', url: '/api/reports', headers: { ...headers(), 'x-forwarded-for': `1.1.1.${i}` }, remoteAddress: '192.0.2.72', payload: { ...validSubmission, name: '' } });
    assert.equal(response.statusCode, i === 5 ? 429 : 400);
  }
  await local.close();
});
