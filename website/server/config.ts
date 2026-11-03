import { z } from 'zod';

const envSchema = z.object({
  DATABASE_URL: z.string().min(1),
  APP_ORIGIN: z.url(),
  COOKIE_SECRET: z.string().min(32),
  RATE_LIMIT_SECRET: z.string().min(32),
  PORT: z.coerce.number().int().min(1).max(65535).default(3000),
  NODE_ENV: z.enum(['production', 'development', 'test']).default('development'),
  TRUST_PROXY: z.enum(['true', 'false']).default('false'),
  GITHUB_CLIENT_ID: z.string().optional(),
  GITHUB_CLIENT_SECRET: z.string().optional(),
  GITHUB_ALLOWED_ID: z.string().regex(/^\d+$/).optional(),
  GITHUB_ALLOWED_LOGIN: z.literal('zortos293').default('zortos293'),
});

export function readConfig(env: NodeJS.ProcessEnv) {
  const parsed = envSchema.safeParse(env);
  if (!parsed.success) throw new Error(`Invalid configuration keys: ${parsed.error.issues.map(i => i.path.join('.')).join(', ')}`);
  const value = parsed.data;
  const url = new URL(value.APP_ORIGIN);
  if (url.origin !== value.APP_ORIGIN || url.username || url.password) throw new Error('APP_ORIGIN must be an origin without a path.');
  if (value.NODE_ENV === 'production' && url.protocol !== 'https:') throw new Error('Production requires an HTTPS APP_ORIGIN.');
  return value;
}
export type Config = ReturnType<typeof readConfig>;

export function oauthConfigured(config: Config) {
  return Boolean(config.GITHUB_CLIENT_ID && config.GITHUB_CLIENT_SECRET && config.GITHUB_ALLOWED_ID);
}

export function allowedGithubUser(config: Config, user: { id: number; login: string }) {
  return oauthConfigured(config) && String(user.id) === config.GITHUB_ALLOWED_ID && user.login.toLowerCase() === config.GITHUB_ALLOWED_LOGIN;
}
