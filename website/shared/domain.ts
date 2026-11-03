import { z } from 'zod';

export const outcomes = ['Perfect', 'Playable', 'Runs', 'Loads', 'Unplayable'] as const;
export const statuses = [...outcomes, 'Untested'] as const;
export const outcomeDescriptions: Record<(typeof outcomes)[number], string> = {
  Perfect: 'No issues found during your test.',
  Playable: 'Can be finished with minor issues.',
  Runs: 'Starts and plays, but major issues stop progress.',
  Loads: 'Reaches a menu or scene, then stops.',
  Unplayable: 'Crashes on boot or shows a black screen.',
};
const text = (max: number) => z.string().trim().min(1).max(max).refine(v => !/[\u0000-\u0008\u000b\u000c\u000e-\u001f]/u.test(v), 'Remove control characters.');
const optionalText = (max: number) => z.union([text(max), z.literal('')]).default('');
export const reportFieldsSchema = z.object({
  gameId: z.number().int().positive(),
  name: text(60),
  handle: z.string().trim().regex(/^@?[A-Za-z0-9_]{1,15}$|^$/, 'Use an X handle, not a URL.').default(''),
  build: text(100), gameVersion: text(60), cpu: text(160), gpu: text(160), driver: text(160), os: text(160),
  ram: optionalText(60), graphicsApi: z.enum(['Vulkan', 'OpenGL', 'Unknown']),
  settings: text(3000), outcome: z.enum(outcomes), progress: text(500),
  summary: text(120), details: text(6000), reproduction: optionalText(3000),
  updates: optionalText(500), mods: optionalText(500),
}).strict();
export type ReportFields = z.infer<typeof reportFieldsSchema>;
export const logUploadSchema = z.object({
  name: z.string().max(100).regex(/^[\w .-]+\.(txt|log)$/i),
  mime: z.enum(['text/plain', 'application/octet-stream', '']),
  text: z.string().min(1).max(512 * 1024),
}).strict();
export const submissionSchema = reportFieldsSchema.extend({
  publicConsent: z.literal(true), safeFilesConsent: z.literal(true), privateLogConsent: z.boolean(),
  log: logUploadSchema.nullable(), website: z.literal(''),
}).strict().refine(v => !v.log || v.privateLogConsent, { message: 'Consent is required for a private log.', path: ['privateLogConsent'] });
const localArt = z.string().regex(/^\/art\/[a-zA-Z0-9_-]+\.(png|jpg|jpeg|webp)$/).nullable();
export const catalogEntrySchema = z.object({
  sgdbId: z.number().int().positive(), slug: z.string().regex(/^[a-z0-9]+(?:-[a-z0-9]+)*$/).max(180),
  title: text(300), coverPath: localArt.default(null), heroPath: localArt.default(null),
  artworkCredit: text(1000).nullable().default(null),
  artworkSource: z.url().refine(v => new URL(v).hostname === 'www.steamgriddb.com' || new URL(v).hostname === 'steamgriddb.com').nullable().default(null),
  releaseDate: z.string().nullable().optional(), publisher: z.string().nullable().optional(),
});
export type CatalogEntry = z.infer<typeof catalogEntrySchema>;
export const gameSchema = catalogEntrySchema.extend({
  reportCount: z.number().int().nonnegative(), status: z.enum(statuses), latestAt: z.string().nullable(),
});
export type Game = z.infer<typeof gameSchema>;
export const publicReportSchema = reportFieldsSchema.extend({
  id: z.uuid(), createdAt: z.string(), gameTitle: z.string(), gameSlug: z.string(),
  triage: z.enum(['new', 'reviewed', 'needs-info', 'hidden']),
});
export type PublicReport = z.infer<typeof publicReportSchema>;
export const pageSchema = z.object({ page: z.number(), pageCount: z.number(), total: z.number() });
export const gamesResponseSchema = pageSchema.extend({
  games: z.array(gameSchema), totalGames: z.number(), totalReports: z.number(),
  counts: z.record(z.enum(statuses), z.number()),
});
export const reportsResponseSchema = pageSchema.extend({ reports: z.array(publicReportSchema) });
export const maintainerReportSchema = publicReportSchema.extend({ internalNote: z.string(), hasLog: z.boolean() });
export const maintainerResponseSchema = pageSchema.extend({ reports: z.array(maintainerReportSchema) });
export const triageSchema = z.object({ triage: z.enum(['new', 'reviewed', 'needs-info', 'hidden']), internalNote: z.string().trim().max(6000) }).strict();
