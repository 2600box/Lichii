import { createHash, randomBytes } from 'node:crypto';
import type { FastifyInstance, FastifyReply, FastifyRequest } from 'fastify';
import * as oauth from 'oauth4webapi';
import { z } from 'zod';
import { allowedGithubUser, oauthConfigured, type Config } from './config.ts';
import type { Database } from './db.ts';

export const hashToken = (token: string) => createHash('sha256').update(token).digest('hex');
const githubUserSchema = z.object({ id: z.number().int().positive(), login: z.string() });
const oauthCookieSchema = z.object({ state: z.string(), verifier: z.string() });
const githubServer: oauth.AuthorizationServer = {
  issuer: 'https://github.com',
  authorization_endpoint: 'https://github.com/login/oauth/authorize',
  token_endpoint: 'https://github.com/login/oauth/access_token',
};

export function authHandlers(sql: Database, config: Config) {
  const secure = new URL(config.APP_ORIGIN).protocol === 'https:';
  const sessionName = secure ? '__Host-lichii-session' : 'lichii-session';
  const oauthName = secure ? '__Host-lichii-oauth' : 'lichii-oauth';
  const cookie = { path: '/', secure, httpOnly: true, sameSite: 'lax' as const };

  async function requireMaintainer(request: FastifyRequest, reply: FastifyReply) {
    if (!oauthConfigured(config)) return reply.code(503).send({ error: 'Maintainer sign-in is not configured.' });
    const token = request.cookies[sessionName];
    if (!token || !/^[a-f0-9]{64}$/.test(token)) return reply.code(401).send({ error: 'Sign in with the authorized GitHub account.' });
    const rows = await sql`SELECT github_id, github_login FROM sessions WHERE token_hash = ${hashToken(token)} AND expires_at > now()`;
    const session = rows[0];
    if (!session || session.github_id !== config.GITHUB_ALLOWED_ID || session.github_login !== config.GITHUB_ALLOWED_LOGIN) {
      return reply.code(403).send({ error: 'This account is not authorized.' });
    }
  }

  async function register(app: FastifyInstance) {
    app.get('/api/auth/status', async request => {
      const token = request.cookies[sessionName];
      if (!oauthConfigured(config) || !token || !/^[a-f0-9]{64}$/.test(token)) return { configured: oauthConfigured(config), authenticated: false };
      const rows = await sql`SELECT 1 FROM sessions WHERE token_hash = ${hashToken(token)} AND github_id = ${config.GITHUB_ALLOWED_ID ?? ''} AND github_login = ${config.GITHUB_ALLOWED_LOGIN} AND expires_at > now()`;
      return { configured: true, authenticated: rows.length === 1 };
    });

    app.get('/api/auth/login', async (request, reply) => {
      if (!oauthConfigured(config) || !config.GITHUB_CLIENT_ID) return reply.code(503).send({ error: 'GitHub sign-in is not configured yet.' });
      const state = oauth.generateRandomState();
      const verifier = oauth.generateRandomCodeVerifier();
      const challenge = await oauth.calculatePKCECodeChallenge(verifier);
      await sql`INSERT INTO oauth_attempts(state_hash, expires_at) VALUES (${hashToken(state)}, now() + interval '10 minutes')`;
      reply.setCookie(oauthName, JSON.stringify({ state, verifier }), { ...cookie, signed: true, maxAge: 600 });
      const url = new URL('https://github.com/login/oauth/authorize');
      url.search = new URLSearchParams({ client_id: config.GITHUB_CLIENT_ID, redirect_uri: `${config.APP_ORIGIN}/api/auth/callback`, state, code_challenge: challenge, code_challenge_method: 'S256', scope: 'read:user' }).toString();
      return reply.redirect(url.href);
    });

    app.get('/api/auth/callback', async (request, reply) => {
      reply.clearCookie(oauthName, cookie);
      if (!oauthConfigured(config) || !config.GITHUB_CLIENT_ID || !config.GITHUB_CLIENT_SECRET) return reply.code(503).send({ error: 'GitHub sign-in is not configured.' });
      try {
        const raw = request.cookies[oauthName];
        if (!raw) throw new Error('Missing state');
        const signed = request.unsignCookie(raw);
        if (!signed.valid || !signed.value) throw new Error('Invalid state');
        const { state, verifier } = oauthCookieSchema.parse(JSON.parse(signed.value));
        const client = { client_id: config.GITHUB_CLIENT_ID };
        const parameters = oauth.validateAuthResponse(githubServer, client, new URL(request.url, config.APP_ORIGIN), state);
        const consumed = await sql`DELETE FROM oauth_attempts WHERE state_hash = ${hashToken(state)} AND expires_at > now() RETURNING state_hash`;
        if (consumed.length !== 1) throw new Error('Expired state');
        const response = await oauth.authorizationCodeGrantRequest(githubServer, client, oauth.ClientSecretPost(config.GITHUB_CLIENT_SECRET), parameters, `${config.APP_ORIGIN}/api/auth/callback`, verifier, { signal: AbortSignal.timeout(10000) });
        const tokens = await oauth.processAuthorizationCodeResponse(githubServer, client, response);
        const profile = await fetch('https://api.github.com/user', { headers: { Authorization: `Bearer ${tokens.access_token}`, Accept: 'application/vnd.github+json', 'User-Agent': 'Lichii-compatibility' }, signal: AbortSignal.timeout(10000) });
        if (!profile.ok) throw new Error('GitHub profile unavailable');
        const user = githubUserSchema.parse(await profile.json());
        if (!allowedGithubUser(config, user)) return reply.redirect('/maintainer?error=not-allowed');
        const token = randomBytes(32).toString('hex');
        const previous = request.cookies[sessionName];
        await sql.begin(async tx => {
          if (previous) await tx`DELETE FROM sessions WHERE token_hash = ${hashToken(previous)}`;
          await tx`INSERT INTO sessions(token_hash, github_id, github_login, expires_at) VALUES (${hashToken(token)}, ${String(user.id)}, ${user.login.toLowerCase()}, now() + interval '8 hours')`;
          await tx`INSERT INTO audit_events(actor, action) VALUES (${String(user.id)}, 'sign-in')`;
        });
        reply.setCookie(sessionName, token, { ...cookie, maxAge: 8 * 3600 });
        return reply.redirect('/maintainer');
      } catch {
        return reply.redirect('/maintainer?error=sign-in-failed');
      }
    });

    app.post('/api/auth/logout', async (request, reply) => {
      const token = request.cookies[sessionName];
      if (token) await sql`DELETE FROM sessions WHERE token_hash = ${hashToken(token)}`;
      reply.clearCookie(sessionName, cookie);
      return { ok: true };
    });
  }
  return { requireMaintainer, register };
}
