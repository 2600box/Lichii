import { readConfig } from './config.ts';
import { connectDatabase, prepareDatabase, cleanup } from './db.ts';
import { createApp } from './app.ts';

const config = readConfig(process.env);
const sql = connectDatabase(config.DATABASE_URL);
await prepareDatabase(sql);
const app = await createApp(config, sql);
const timer = setInterval(() => { void cleanup(sql).catch(() => app.log.error('Retention cleanup failed')); }, 3600000);
timer.unref();
const stop = async () => { clearInterval(timer); await app.close(); await sql.end(); };
process.once('SIGTERM', () => { void stop(); });
process.once('SIGINT', () => { void stop(); });
await app.listen({ host: '0.0.0.0', port: config.PORT });
