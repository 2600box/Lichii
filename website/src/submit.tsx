import { useEffect, useState, type ChangeEvent, type FormEvent, type ReactNode } from 'react';
import { Link, useNavigate, useSearchParams } from 'react-router-dom';
import { ArrowRight, Check, FileText, LockKeyhole, Upload, X } from 'lucide-react';
import { z } from 'zod';
import { gameSchema, gamesResponseSchema, outcomeDescriptions, outcomes, type Game, type ReportFields } from '../shared/domain.ts';
import { extractLogFields, MAX_LOG_BYTES, validateLog } from '../shared/log.ts';
import { api, ApiError, useApi, useDebounced } from './api.ts';
import { Breadcrumbs, Cover, ErrorNotice, Loading, PrivateNote, Status } from './components.tsx';

type Draft = Omit<ReportFields, 'outcome'> & { outcome: ReportFields['outcome'] | '' };
const initialDraft: Draft = { gameId: 0, name: '', handle: '', build: '', gameVersion: '', cpu: '', gpu: '', driver: '', os: '', ram: '', graphicsApi: 'Unknown', settings: '', outcome: '', progress: '', summary: '', details: '', reproduction: '', updates: '', mods: '' };
const createdSchema = z.object({ id: z.uuid() });

export function SubmitReport() {
  const [params] = useSearchParams();
  const slug = params.get('game');
  return slug ? <SelectedGame slug={slug} /> : <ChooseGame />;
}
function ChooseGame() {
  const [query, setQuery] = useState('');
  const debounced = useDebounced(query);
  const state = useApi(`/api/games?q=${encodeURIComponent(debounced)}`, gamesResponseSchema);
  return <div className="narrow-page"><Breadcrumbs items={[{ label: 'Submit a report' }]} /><h1 className="page-title">What did you play?</h1><p className="page-description">Choose a Switch 2 game to start your report. No account needed.</p><label className="field mt-8"><span>Find your game</span><input autoFocus type="search" placeholder="Search the catalog…" value={query} onChange={e => setQuery(e.target.value)} maxLength={150} /></label><div className="game-picker">{state.kind === 'loading' ? <Loading /> : state.kind === 'error' ? <ErrorNotice message={state.message} /> : state.data.games.length === 0 ? <p className="muted py-8">No matching games. Try a shorter title.</p> : state.data.games.map(game => <Link key={game.sgdbId} to={`/submit?game=${game.slug}`}><Cover game={game} /><div><strong>{game.title}</strong><span>Nintendo Switch 2</span></div><ArrowRight size={18} /></Link>)}</div></div>;
}
function SelectedGame({ slug }: { slug: string }) {
  const state = useApi(`/api/games/${encodeURIComponent(slug)}`, gameSchema);
  if (state.kind === 'loading') return <Loading />;
  if (state.kind === 'error') return <ErrorNotice message={state.message} />;
  return <ReportForm key={state.data.sgdbId} game={state.data} />;
}

function ReportForm({ game }: { game: Game }) {
  const navigate = useNavigate();
  const [draft, setDraft] = useState<Draft>({ ...initialDraft, gameId: game.sgdbId });
  const [log, setLog] = useState<{ name: string; mime: '' | 'text/plain' | 'application/octet-stream'; text: string } | null>(null);
  const [preview, setPreview] = useState<Partial<ReportFields>>({});
  const [showLog, setShowLog] = useState(false);
  const [publicConsent, setPublicConsent] = useState(false);
  const [safeFilesConsent, setSafeFilesConsent] = useState(false);
  const [privateLogConsent, setPrivateLogConsent] = useState(false);
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState('');
  const [errors, setErrors] = useState<Record<string, string[]>>({});
  const [applied, setApplied] = useState(false);
  const [honeypot, setHoneypot] = useState('');
  const update = (key: keyof Draft, value: string) => setDraft(current => ({ ...current, [key]: value }));
  const detected = Object.entries(preview);

  async function chooseFile(event: ChangeEvent<HTMLInputElement>) {
    const file = event.target.files?.[0];
    event.target.value = '';
    setMessage('');
    if (!file) return;
    if (file.size > MAX_LOG_BYTES) { setMessage('Your log is too large. Use a plain-text excerpt of 512 KB or less.'); return; }
    if (!/^[\w .-]+\.(txt|log)$/i.test(file.name) || !['', 'text/plain', 'application/octet-stream'].includes(file.type)) { setMessage('Choose a .txt or .log plain-text file. Archives, images and binaries aren’t accepted.'); return; }
    try {
      const text = new TextDecoder('utf-8', { fatal: true }).decode(await file.arrayBuffer());
      const problem = validateLog(text);
      if (problem) { setMessage(problem); return; }
      setLog({ name: file.name, mime: 'text/plain', text });
      setPreview(extractLogFields(text));
      setShowLog(true);
      setApplied(false);
      setPrivateLogConsent(false);
    } catch { setMessage('This file is not valid UTF-8 text. Choose eden_log.txt, not an archive or binary.'); }
  }
  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    setBusy(true); setMessage(''); setErrors({});
    try {
      const result = await api('/api/reports', createdSchema, { method: 'POST', body: JSON.stringify({ ...draft, log, publicConsent, safeFilesConsent, privateLogConsent, website: honeypot }) });
      navigate(`/submitted/${result.id}`);
    } catch (error) {
      setMessage(error instanceof Error ? error.message : 'Your report could not be submitted. Please try again.');
      if (error instanceof ApiError) setErrors(error.fields);
    } finally { setBusy(false); }
  }
  function field(key: Exclude<keyof Draft, 'gameId' | 'outcome' | 'graphicsApi'>, label: string, options: { optional?: boolean; long?: boolean; hint?: string; max?: number } = {}) {
    const error = errors[key]?.join(' ');
    return <label className="field" key={key}><span>{label}{options.optional && <small> optional</small>}{applied && key in preview && <small className="detected">from log, please check</small>}</span>{options.long ? <textarea name={key} value={draft[key]} onChange={e => update(key, e.target.value)} required={!options.optional} maxLength={options.max ?? 3000} rows={4} aria-invalid={Boolean(error)} aria-describedby={error ? `${key}-error` : undefined} /> : <input name={key} value={draft[key]} onChange={e => update(key, e.target.value)} required={!options.optional} maxLength={options.max ?? 160} aria-invalid={Boolean(error)} aria-describedby={error ? `${key}-error` : undefined} autoComplete="off" />}{options.hint && <small>{options.hint}</small>}{error && <small id={`${key}-error`} className="field-error">{error}</small>}</label>;
  }
  return <><Breadcrumbs items={[{ label: game.title, to: `/games/${game.slug}` }, { label: 'Submit a report' }]} /><header className="form-title"><h1 className="page-title">Submit a report</h1><div className="game-cell"><Cover game={game} /><div><strong>{game.title}</strong><span>Nintendo Switch 2 · <Link to="/submit" className="text-link">Change game</Link></span></div></div></header><div className="form-grid"><form onSubmit={event => { void submit(event); }} className="report-form">
    <FormSection number="01" title="Start with your log file" note="We read what we can find. You check it.">
      <p className="muted text-sm mb-4">Optional, but useful. Choose <strong className="text-white">eden_log.txt</strong>, plain text, up to 512 KB. Nothing leaves your browser until you submit.</p>
      {log ? <div className="log-preview"><div className="log-file-head"><FileText size={19} /><div><strong>{log.name}</strong><span>{Math.ceil(new TextEncoder().encode(log.text).length / 1024)} KB · local preview · not uploaded</span></div><button type="button" className="text-link" onClick={() => setShowLog(!showLog)}>{showLog ? 'Hide preview' : 'Show preview'}</button><button type="button" className="icon-button" aria-label="Remove log" onClick={() => { setLog(null); setPreview({}); setApplied(false); setPrivateLogConsent(false); }}><X size={17} /></button></div>{showLog && <pre tabIndex={0}>{log.text}</pre>}<div className="log-warning"><LockKeyhole size={14} /><span>Review this file for usernames, file paths and personal information. Remove anything sensitive in a text editor, then choose the cleaned file.</span></div></div> : <label className="upload-box"><Upload size={26} strokeWidth={1.3} /><strong>Choose a log file</strong><span>eden_log.txt · .txt or .log · up to 512 KB</span><input type="file" accept=".txt,.log,text/plain" onChange={event => { void chooseFile(event); }} /></label>}
      {log && <div className="detected-panel"><div><strong>{detected.length ? `${detected.length} fields found in your log` : 'No recognized fields found'}</strong><span>Values are unverified. Review before using.</span></div>{detected.length ? <><dl>{detected.map(([key, value]) => <div key={key}><dt>{key}</dt><dd>{value}</dd></div>)}</dl><button type="button" className="button compact" onClick={() => { setDraft(current => ({ ...current, ...preview, gameId: game.sgdbId })); setApplied(true); }}>{applied ? <><Check size={15} /> Applied. Check the fields below.</> : 'Use these fields'}</button></> : <p>Fill everything in manually. A different log format is not evidence about compatibility.</p>}<p>Outcome, progress and settings are yours to fill in. A log cannot decide whether a game is playable.</p></div>}
      {message && <div className="notice error mt-4" role="alert">{message}</div>}
    </FormSection>
    <FormSection number="02" title="Build and game version" note="Exact versions, not “latest”."><div className="fields-grid">{field('build', 'Lichii build', { max: 100, hint: 'Help → About Lichii shows the full version.' })}{field('gameVersion', 'Game version', { max: 60, hint: 'The version in your library or Booting game log line.' })}{field('updates', 'Update or DLC installed', { optional: true, max: 500 })}{field('mods', 'Mods or cheats', { optional: true, max: 500, hint: 'A test with mods off is easiest to compare.' })}</div></FormSection>
    <FormSection number="03" title="Hardware and system"><div className="fields-grid">{field('cpu', 'CPU')}{field('gpu', 'GPU')}{field('driver', 'GPU driver version')}{field('os', 'Operating system')}{field('ram', 'RAM', { optional: true, max: 60 })}</div><p className="fine-print mt-3">If a value is unavailable, write “Unknown”. Don’t guess.</p></FormSection>
    <FormSection number="04" title="Settings"><label className="field mb-5"><span>Graphics API</span><select value={draft.graphicsApi} onChange={e => { const result = z.enum(['Vulkan', 'OpenGL', 'Unknown']).safeParse(e.target.value); if (result.success) setDraft(current => ({ ...current, graphicsApi: result.data })); }}><option>Unknown</option><option>Vulkan</option><option>OpenGL</option></select></label>{field('settings', 'Settings used', { long: true, hint: 'Resolution, accuracy, async shaders and anything changed from defaults. Write Unknown where necessary.' })}</FormSection>
    <FormSection number="05" title="Outcome" note="Pick the one that fits best."><fieldset><legend className="sr-only">How did the game run?</legend><div className="outcome-grid">{outcomes.map(outcome => <label key={outcome} className={`outcome-option ${draft.outcome === outcome ? 'selected' : ''}`}><input type="radio" name="outcome" value={outcome} required checked={draft.outcome === outcome} onChange={() => setDraft(current => ({ ...current, outcome }))} /><Status value={outcome} /><span>{outcomeDescriptions[outcome]}</span></label>)}</div></fieldset>{errors.outcome && <p className="field-error">Choose an outcome.</p>}{field('progress', 'How far did you get?', { max: 500, hint: 'Say what you played, for how long, and where you stopped.' })}</FormSection>
    <FormSection number="06" title="What happened" note="Public. Write it for the next player."><div className="flex flex-col gap-5">{field('summary', 'Summary', { max: 120 })}{field('details', 'Details', { long: true, max: 6000, hint: 'Describe what you saw. Don’t include personal information.' })}{field('reproduction', 'Steps to reproduce', { long: true, optional: true, max: 3000 })}</div><p className="fine-print mt-4">Screenshot uploads aren’t supported at launch. Describe the issue in words.</p></FormSection>
    <FormSection number="07" title="About you" note="No account needed."><div className="fields-grid">{field('name', 'Name', { max: 60, hint: 'Public. A nickname is fine.' })}{field('handle', 'X handle', { optional: true, max: 16, hint: 'Public, so the maintainer can ask about your report.' })}</div></FormSection>
    <FormSection number="08" title="Check and submit"><div className="consent-list"><label><input type="checkbox" required checked={safeFilesConsent} onChange={e => setSafeFilesConsent(e.target.checked)} /><span>My report and attachments contain no game files, keys, firmware, saves or private account details.</span></label>{log && <label><input type="checkbox" required checked={privateLogConsent} onChange={e => setPrivateLogConsent(e.target.checked)} /><span>I reviewed the log preview and consent to the maintainer reading it. It is private and retained for up to 90 days.</span></label>}<label><input type="checkbox" required checked={publicConsent} onChange={e => setPublicConsent(e.target.checked)} /><span>I understand my name, optional X handle, report text, hardware and settings will be public.</span></label></div><div className="honeypot" aria-hidden="true"><label>Website<input value={honeypot} onChange={e => setHoneypot(e.target.value)} tabIndex={-1} autoComplete="off" /></label></div>{message && <div role="alert" className="notice error mb-4">{message}</div>}<div className="submit-row"><button className="button primary" type="submit" disabled={busy}>{busy ? 'Submitting…' : 'Submit report'}<ArrowRight size={16} /></button><PrivateNote /></div><p className="fine-print mt-4">There is no author account or edit link. Double-check before publishing. <Link to="/privacy">Privacy & removal requests</Link></p></FormSection>
  </form><aside className="form-sidebar"><div className="panel"><LockKeyhole size={24} className="text-accent mb-4" /><h3>Your report is public.<br />Your log isn’t.</h3><p>Only the authorized Lichii maintainer can open your log. Its contents and original filename never appear on public report pages.</p><p>Log files are deleted after 90 days. The public report remains.</p></div><div><h3 className="lined-heading">Before you submit</h3><ol className="numbered-list"><li><span>1</span><p>Restart Lichii, reproduce the problem, then stop the game.</p></li><li><span>2</span><p>Open <strong>File → Open Lichii Folders → Log Folder</strong>.</p></li><li><span>3</span><p>Choose <code>eden_log.txt</code> before launching Lichii again. A new launch can rotate the old log.</p></li><li><span>4</span><p>Check the preview. Remove personal information before uploading.</p></li></ol><Link className="text-link" to="/guide">Read the full reporting guide <ArrowRight size={15} /></Link></div><div className="notice"><p>Never attach game files, keys, firmware, saves, zipped folders or account details. Logs are optional. A clear manual report is welcome.</p></div></aside></div></>;
}
function FormSection({ number, title, note, children }: { number: string; title: string; note?: string; children: ReactNode }) {
  return <section className="form-section"><div className="form-section-heading"><h2><span>{number}</span>{title}</h2>{note && <p>{note}</p>}</div>{children}</section>;
}
