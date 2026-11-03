import { readConfig } from './config.ts';
import { connectDatabase, prepareDatabase } from './db.ts';

const config = readConfig(process.env);
const sql = connectDatabase(config.DATABASE_URL);
try { await prepareDatabase(sql); } finally { await sql.end(); }
