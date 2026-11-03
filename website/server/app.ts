import Fastify from 'fastify';
import cookie from '@fastify/cookie';
import helmet from '@fastify/helmet';
import staticFiles from '@fastify/static';
import { createHmac, randomUUID } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { isIP } from 'node:net';
import { z } from 'zod';
import { gameSchema, publicReportSchema, reportFieldsSchema, submissionSchema, triageSchema, maintainerReportSchema } from '../shared/domain.ts';
import { validateLog } from '../shared/log.ts';
import type { Config } from './config.ts';
import type { Database } from './db.ts';
import { authHandlers } from './auth.ts';

const querySchema = z.object({
  q: z.string().trim().max(150).default(''),
  status: z.enum(['Any', 'Perfect', 'Playable', 'Runs', 'Loads', 'Unplayable', 'Untested']).default('Any'),
  gpu: z.string().trim().max(100).default(''), os: z.string().trim().max(100).default(''),
  build: z.string().trim().max(100).default(''),
  sort: z.enum(['popular', 'title', 'recent']).default('popular'),
  page: z.coerce.number().int().min(1).max(10000).default(1),
  gameId: z.coerce.number().int().positive().optional(),
});
const uuidParams = z.object({ id: z.uuid() });

export async function createApp(config: Config, sql: Database, serveStatic = true) {
  const app = Fastify({
    logger: { level: 'warn', serializers: { req: () => ({}), res: () => ({}), err: () => ({ type: 'Error', message: 'Request failed', stack: '' }) } },
    disableRequestLogging: true,
    bodyLimit: 1100 * 1024,
    requestTimeout: 15000,
    connectionTimeout: 20000,
    trustProxy: false,
    routerOptions: { maxParamLength: 200 },
  });
  await app.register(cookie, { secret: config.COOKIE_SECRET });
  await app.register(helmet, {
    contentSecurityPolicy: { directives: {
      defaultSrc: ["'self'"], scriptSrc: ["'self'"], styleSrc: ["'self'"], imgSrc: ["'self'"],
      fontSrc: ["'self'"], connectSrc: ["'self'"], objectSrc: ["'none'"], frameAncestors: ["'none'"],
      baseUri: ["'none'"], formAction: ["'self'"], upgradeInsecureRequests: config.NODE_ENV === 'production' ? [] : null,
    } },
    referrerPolicy: { policy: 'no-referrer' },
    crossOriginResourcePolicy: { policy: 'same-origin' },
    hsts: config.NODE_ENV === 'production' ? { maxAge: 31536000 } : false,
  });
  app.addHook('onRequest', async (request, reply) => {
    reply.header('Permissions-Policy', 'camera=(), microphone=(), geolocation=()');
    if (request.url.startsWith('/api/')) reply.header('Cache-Control', 'no-store');
    if (['POST', 'PUT', 'PATCH', 'DELETE'].includes(request.method)) {
      if (request.headers.origin !== config.APP_ORIGIN) return reply.code(403).send({ error: 'This request must come from the Lichii website.' });
      if (request.headers['sec-fetch-site'] === 'cross-site') return reply.code(403).send({ error: 'Cross-site requests are not allowed.' });
      if (request.headers['content-type']?.split(';')[0] !== 'application/json') return reply.code(415).send({ error: 'Send application/json.' });
    }
    if (!request.url.startsWith('/api/') || request.url === '/api/health') return;
    const submitting = request.method === 'POST' && request.url.split('?')[0] === '/api/reports';
    const auth = request.url.startsWith('/api/auth/login');
    const period = submitting || auth ? 3600 : 60;
    const limit = submitting ? 5 : auth ? 20 : 240;
    const bucket = Math.floor(Date.now() / (period * 1000));
    const edgeIp = request.headers['x-real-ip'];
    if (config.TRUST_PROXY === 'true' && (typeof edgeIp !== 'string' || !isIP(edgeIp))) return reply.code(400).send({ error: 'A valid trusted proxy address is required.' });
    const ip = config.TRUST_PROXY === 'true' && typeof edgeIp === 'string' ? edgeIp : request.ip;
    const key = createHmac('sha256', config.RATE_LIMIT_SECRET).update(`${ip}:${submitting ? 'submit' : auth ? 'auth' : 'read'}:${bucket}`).digest('hex');
    const rows = await sql`INSERT INTO rate_limits(key, count, expires_at) VALUES (${key}, 1, now() + ${period} * interval '1 second')
      ON CONFLICT(key) DO UPDATE SET count = rate_limits.count + 1 RETURNING count`;
    if (Number(rows[0]?.count) > limit) return reply.header('Retry-After', String(period)).code(429).send({ error: 'Too many requests. Please try again later.' });
  });
  app.setErrorHandler((error, request, reply) => {
    if (error instanceof z.ZodError) return reply.code(400).send({ error: 'Check the highlighted fields.', fields: error.flatten().fieldErrors });
    const code = error instanceof Error && 'statusCode' in error && typeof error.statusCode === 'number' ? error.statusCode : 500;
    if (code >= 500) app.log.error({ requestId: request.id }, 'Request failed');
    return reply.code(code >= 400 && code < 600 ? code : 500).send({ error: code === 413 ? 'The request is too large. Logs must be 512 KB or smaller.' : code === 400 ? 'The request could not be read.' : code === 415 ? 'Unsupported request format.' : 'The service is temporarily unavailable. Please try again.' });
  });
  const auth = authHandlers(sql, config);
  await auth.register(app);

  app.get('/api/health', async (_request, reply) => {
    try { await sql`SELECT 1 FROM games LIMIT 1`; return { ok: true }; }
    catch { return reply.code(503).send({ ok: false }); }
  });

  const gameRows = sql`SELECT g.catalog,
    (SELECT count(*)::int FROM reports r WHERE r.game_id = g.sgdb_id AND r.triage <> 'hidden') AS report_count,
    COALESCE((SELECT r.fields->>'outcome' FROM reports r WHERE r.game_id = g.sgdb_id AND r.triage <> 'hidden' ORDER BY r.created_at DESC, r.id DESC LIMIT 1), 'Untested') AS status,
    (SELECT r.created_at FROM reports r WHERE r.game_id = g.sgdb_id AND r.triage <> 'hidden' ORDER BY r.created_at DESC, r.id DESC LIMIT 1) AS latest_at,
    g.sgdb_id, g.title, g.slug FROM games g`;
  const mapGame = (row: Record<string, unknown>) => gameSchema.parse({ ...z.record(z.string(), z.unknown()).parse(row.catalog), reportCount: row.report_count, status: row.status, latestAt: row.latest_at instanceof Date ? row.latest_at.toISOString() : null });

  app.get('/api/games', async request => {
    const q = querySchema.parse(request.query);
    const where = sql`position(lower(${q.q}) in lower(title || ' ' || COALESCE(catalog->>'publisher', ''))) > 0 AND (${q.status} = 'Any' OR status = ${q.status})
      AND (${q.gpu} = '' AND ${q.os} = '' AND ${q.build} = '' OR EXISTS(SELECT 1 FROM reports r WHERE r.game_id = catalog_rows.sgdb_id AND r.triage <> 'hidden'
        AND position(lower(${q.gpu}) in lower(r.fields->>'gpu')) > 0 AND position(lower(${q.os}) in lower(r.fields->>'os')) > 0 AND position(lower(${q.build}) in lower(r.fields->>'build')) > 0))`;
    const ordering = q.sort === 'title' ? sql`lower(title)` : q.sort === 'recent' ? sql`latest_at DESC NULLS LAST, lower(title)` : sql`report_count DESC, CASE sgdb_id WHEN 5483943 THEN 0 WHEN 5483961 THEN 1 WHEN 5483963 THEN 2 ELSE 3 END, lower(title)`;
    const [rows, total, counts, reportTotal] = await Promise.all([
      sql`WITH catalog_rows AS (${gameRows}) SELECT * FROM catalog_rows WHERE ${where} ORDER BY ${ordering} LIMIT 15 OFFSET ${(q.page - 1) * 15}`,
      sql`WITH catalog_rows AS (${gameRows}) SELECT count(*)::int AS total FROM catalog_rows WHERE ${where}`,
      sql`WITH catalog_rows AS (${gameRows}) SELECT status, count(*)::int AS count FROM catalog_rows GROUP BY status`,
      sql`SELECT count(*)::int AS total FROM reports WHERE triage <> 'hidden'`,
    ]);
    const distribution = { Perfect: 0, Playable: 0, Runs: 0, Loads: 0, Unplayable: 0, Untested: 0 };
    for (const row of counts) {
      const status = gameSchema.shape.status.parse(row.status);
      distribution[status] = z.number().parse(row.count);
    }
    const number = Number(total[0]?.total ?? 0);
    return { games: rows.map(mapGame), page: q.page, pageCount: Math.max(1, Math.ceil(number / 15)), total: number, totalGames: Object.values(distribution).reduce((a, b) => a + b, 0), totalReports: Number(reportTotal[0]?.total ?? 0), counts: distribution };
  });

  app.get('/api/games/:slug', async (request, reply) => {
    const { slug } = z.object({ slug: z.string().max(180) }).parse(request.params);
    const rows = await sql`WITH catalog_rows AS (${gameRows}) SELECT * FROM catalog_rows WHERE slug = ${slug}`;
    if (!rows[0]) return reply.code(404).send({ error: 'Game not found.' });
    return mapGame(rows[0]);
  });

  const publicColumns = sql`r.id, r.fields, r.created_at, r.triage, g.title AS game_title, g.slug AS game_slug`;
  const mapReport = (row: Record<string, unknown>) => publicReportSchema.parse({
    ...reportFieldsSchema.parse(row.fields), id: row.id, createdAt: row.created_at instanceof Date ? row.created_at.toISOString() : '', triage: row.triage,
    gameTitle: row.game_title, gameSlug: row.game_slug,
  });

  app.get('/api/reports', async request => {
    const q = querySchema.parse(request.query);
    const where = sql`r.triage <> 'hidden' AND (${q.gameId ?? 0} = 0 OR r.game_id = ${q.gameId ?? 0})
      AND (${q.status} = 'Any' OR r.fields->>'outcome' = ${q.status})
      AND position(lower(${q.q}) in lower(g.title || ' ' || (r.fields->>'summary'))) > 0
      AND position(lower(${q.gpu}) in lower(r.fields->>'gpu')) > 0
      AND position(lower(${q.os}) in lower(r.fields->>'os')) > 0
      AND position(lower(${q.build}) in lower(r.fields->>'build')) > 0`;
    const [rows, totals] = await Promise.all([
      sql`SELECT ${publicColumns} FROM reports r JOIN games g ON g.sgdb_id = r.game_id WHERE ${where} ORDER BY r.created_at DESC, r.id DESC LIMIT 15 OFFSET ${(q.page - 1) * 15}`,
      sql`SELECT count(*)::int AS total FROM reports r JOIN games g ON g.sgdb_id = r.game_id WHERE ${where}`,
    ]);
    const total = Number(totals[0]?.total ?? 0);
    return { reports: rows.map(mapReport), page: q.page, pageCount: Math.max(1, Math.ceil(total / 15)), total };
  });

  app.get('/api/reports/:id', async (request, reply) => {
    const { id } = uuidParams.parse(request.params);
    const rows = await sql`SELECT ${publicColumns} FROM reports r JOIN games g ON g.sgdb_id = r.game_id WHERE r.id = ${id} AND r.triage <> 'hidden'`;
    if (!rows[0]) return reply.code(404).send({ error: 'Report not found.' });
    return mapReport(rows[0]);
  });

  app.post('/api/reports', async (request, reply) => {
    const data = submissionSchema.parse(request.body);
    if (data.log) {
      const problem = validateLog(data.log.text);
      if (problem) return reply.code(400).send({ error: problem, fields: { log: [problem] } });
    }
    const game = await sql`SELECT sgdb_id FROM games WHERE sgdb_id = ${data.gameId}`;
    if (!game.length) return reply.code(400).send({ error: 'Choose a game in the Switch 2 catalog.' });
    const { log, publicConsent, safeFilesConsent, privateLogConsent, website, ...fields } = data;
    const id = randomUUID();
    const day = Math.floor(Date.now() / 86400000);
    const stored = await sql.begin(async tx => {
      const volume = await tx`INSERT INTO rate_limits(key, count, expires_at) VALUES (${`global-submit:${day}`}, 1, now() + interval '1 day') ON CONFLICT(key) DO UPDATE SET count = rate_limits.count + 1 RETURNING count`;
      if (Number(volume[0]?.count) > 1000) return false;
      await tx`INSERT INTO reports(id, game_id, fields) VALUES (${id}, ${fields.gameId}, ${tx.json(fields)})`;
      if (log) await tx`INSERT INTO private_logs(report_id, content) VALUES (${id}, ${Buffer.from(log.text, 'utf8')})`;
      return true;
    });
    if (!stored) return reply.code(429).send({ error: 'The daily submission limit has been reached. Please try again tomorrow.' });
    return reply.code(201).send({ id });
  });

  await app.register(async privateApp => {
    privateApp.addHook('onRequest', auth.requireMaintainer);
    privateApp.get('/api/maintainer/reports', async request => {
      const q = querySchema.parse(request.query);
      const [rows, totals] = await Promise.all([
        sql`SELECT ${publicColumns}, r.internal_note, EXISTS(SELECT 1 FROM private_logs l WHERE l.report_id = r.id AND l.expires_at > now()) AS has_log
          FROM reports r JOIN games g ON g.sgdb_id = r.game_id ORDER BY r.created_at DESC, r.id DESC LIMIT 15 OFFSET ${(q.page - 1) * 15}`,
        sql`SELECT count(*)::int AS total FROM reports`,
      ]);
      const total = Number(totals[0]?.total ?? 0);
      return { reports: rows.map(row => maintainerReportSchema.parse({ ...mapReport(row), internalNote: row.internal_note, hasLog: row.has_log })), page: q.page, pageCount: Math.max(1, Math.ceil(total / 15)), total };
    });
    privateApp.patch('/api/maintainer/reports/:id', async (request, reply) => {
      const { id } = uuidParams.parse(request.params);
      const data = triageSchema.parse(request.body);
      const rows = await sql.begin(async tx => {
        const updated = await tx`UPDATE reports SET triage = ${data.triage}, internal_note = ${data.internalNote} WHERE id = ${id} RETURNING id`;
        if (updated.length) await tx`INSERT INTO audit_events(actor, action, report_id) VALUES (${config.GITHUB_ALLOWED_ID ?? ''}, 'triage', ${id})`;
        return updated;
      });
      if (!rows.length) return reply.code(404).send({ error: 'Report not found.' });
      return { ok: true };
    });
    privateApp.get('/api/maintainer/reports/:id/log', async (request, reply) => {
      const { id } = uuidParams.parse(request.params);
      const rows = await sql.begin(async tx => {
        const logs = await tx`SELECT content FROM private_logs WHERE report_id = ${id} AND expires_at > now()`;
        if (logs.length) await tx`INSERT INTO audit_events(actor, action, report_id) VALUES (${config.GITHUB_ALLOWED_ID ?? ''}, 'read-log', ${id})`;
        return logs;
      });
      const log = rows[0]?.content;
      if (!Buffer.isBuffer(log)) return reply.code(404).send({ error: 'This log was not attached, was deleted, or has expired.' });
      return reply.header('Content-Disposition', 'attachment; filename="eden_log.txt"').type('text/plain; charset=utf-8').send(log);
    });
    privateApp.delete('/api/maintainer/reports/:id/log', async (request, reply) => {
      const { id } = uuidParams.parse(request.params);
      await sql.begin(async tx => {
        await tx`DELETE FROM private_logs WHERE report_id = ${id}`;
        await tx`INSERT INTO audit_events(actor, action, report_id) VALUES (${config.GITHUB_ALLOWED_ID ?? ''}, 'delete-log', ${id})`;
      });
      return reply.code(204).send();
    });
  });

  if (serveStatic) {
    await app.register(staticFiles, { root: fileURLToPath(new URL('../dist', import.meta.url)), wildcard: false, index: false, maxAge: 3600 });
    app.setNotFoundHandler((request, reply) => {
      if (request.url.startsWith('/api/') || request.method !== 'GET' || request.url.split('?')[0]?.includes('.')) return reply.code(404).send({ error: 'Not found.' });
      return reply.header('Cache-Control', 'no-cache').sendFile('index.html');
    });
  }
  return app;
}
