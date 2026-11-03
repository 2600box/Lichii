import { useEffect, useState } from 'react';
import { Link, useSearchParams } from 'react-router-dom';
import { z } from 'zod';
import { ArrowRight, Download, Github, LockKeyhole, LogOut, Search, ShieldCheck, Trash2 } from 'lucide-react';
import { maintainerResponseSchema, type maintainerReportSchema } from '../shared/domain.ts';
import { api, useApi } from './api.ts';
import { Empty, ErrorNotice, Loading, Pagination, Status, formatDate } from './components.tsx';
import { ReportSetup } from './reports.tsx';

const statusSchema = z.object({ configured: z.boolean(), authenticated: z.boolean() });
const okSchema = z.object({ ok: z.boolean() });
type MaintainerReport = z.infer<typeof maintainerReportSchema>;

export function Maintainer() {
  const state = useApi('/api/auth/status', statusSchema);
  const [params] = useSearchParams();
  if (state.kind === 'loading') return <Loading />;
  if (state.kind === 'error') return <ErrorNotice message={state.message} />;
  if (!state.data.authenticated) return <div className="login-page"><div className="login-icon"><ShieldCheck size={34} strokeWidth={1.5} /></div><h1 className="page-title">Maintainer access</h1><p>Private logs and triage are restricted to the authorized Lichii maintainer.</p><div className="panel"><Github size={28} /><h2>Sign in with GitHub</h2><p>Only <strong>zortos293</strong>, verified by account ID, can access this area. No community accounts are needed to submit reports.</p>{params.has('error') && <div className="notice error" role="alert">{params.get('error') === 'not-allowed' ? 'This GitHub account is not authorized.' : 'Sign-in could not be completed. Please try again.'}</div>}{state.data.configured ? <a className="button primary" href="/api/auth/login"><Github size={18} /> Continue with GitHub <ArrowRight size={16} /></a> : <div className="notice"><LockKeyhole size={19} /><p>Maintainer sign-in is not configured yet. Private data remains locked.</p></div>}</div><Link className="text-link" to="/">Back to the games database <ArrowRight size={15} /></Link></div>;
  return <MaintainerQueue />;
}
function MaintainerQueue() {
  const [page, setPage] = useState(1);
  const [revision, setRevision] = useState(0);
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const state = useApi(`/api/maintainer/reports?page=${page}&revision=${revision}`, maintainerResponseSchema);
  const selected = state.kind === 'ready' ? state.data.reports.find(r => r.id === selectedId) : undefined;
  async function logout() { await api('/api/auth/logout', okSchema, { method: 'POST', body: '{}' }); window.location.assign('/maintainer'); }
  return <><div className="maintainer-banner"><ShieldCheck size={19} /><strong>Maintainer view</strong><span>Private. Reporters and visitors cannot access this data.</span><button className="text-link" onClick={() => { void logout(); }}><LogOut size={15} /> Sign out</button></div><div className="page-heading"><div><h1 className="page-title">Report triage</h1><p className="page-description">Review real reports. Keep private logs private.</p></div></div>{state.kind === 'loading' ? <Loading /> : state.kind === 'error' ? <ErrorNotice message={state.message} /> : <><div className="queue-list">{state.data.reports.length === 0 ? <Empty title="The queue is clear">There are no reports to review yet.</Empty> : state.data.reports.map(report => <button key={report.id} className={`queue-item ${selectedId === report.id ? 'selected' : ''}`} onClick={() => setSelectedId(report.id)}><Status value={report.outcome} /><div><strong>{report.summary}</strong><span>{report.gameTitle} · {report.name} · {formatDate(report.createdAt)}</span></div><span className="triage-badge">{report.triage}</span>{report.hasLog && <LockKeyhole size={15} aria-label="Private log attached" />}<ArrowRight size={16} /></button>)}</div><Pagination {...state.data} onPage={page => { setPage(page); setSelectedId(null); }} />{selected && <Triage key={`${selected.id}-${revision}`} report={selected} onChanged={() => setRevision(value => value + 1)} />}</>}</>;
}
function Triage({ report, onChanged }: { report: MaintainerReport; onChanged: () => void }) {
  const [triage, setTriage] = useState(report.triage);
  const [internalNote, setInternalNote] = useState(report.internalNote);
  const [log, setLog] = useState<string | null>(null);
  const [query, setQuery] = useState('');
  const [level, setLevel] = useState('All');
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState('');
  const [confirmDelete, setConfirmDelete] = useState(false);
  useEffect(() => { setLog(null); }, [report.id]);
  async function readLog() {
    setBusy(true); setMessage('');
    try {
      const response = await fetch(`/api/maintainer/reports/${report.id}/log`, { credentials: 'same-origin', cache: 'no-store' });
      if (!response.ok) throw new Error('The log could not be opened. Your session may have expired, or the log is no longer available.');
      setLog(await response.text());
    } catch (error) { setMessage(error instanceof Error ? error.message : 'Could not read log.'); }
    finally { setBusy(false); }
  }
  async function save() {
    setBusy(true); setMessage('');
    try { await api(`/api/maintainer/reports/${report.id}`, okSchema, { method: 'PATCH', body: JSON.stringify({ triage, internalNote }) }); onChanged(); }
    catch (error) { setMessage(error instanceof Error ? error.message : 'Could not save.'); }
    finally { setBusy(false); }
  }
  async function deleteLog() {
    setBusy(true); setMessage('');
    try {
      const response = await fetch(`/api/maintainer/reports/${report.id}/log`, { method: 'DELETE', credentials: 'same-origin', headers: { 'Content-Type': 'application/json' }, body: '{}' });
      if (!response.ok) throw new Error('Could not delete the log. Please check your session.');
      setLog(null); onChanged();
    } catch (error) { setMessage(error instanceof Error ? error.message : 'Could not delete.'); }
    finally { setBusy(false); }
  }
  const lines = log?.split('\n').map((text, i) => ({ text, line: i + 1 })).filter(line => (level === 'All' || line.text.includes(`<${level}>`)) && line.text.toLowerCase().includes(query.toLowerCase()));
  return <section className="triage-detail"><div className="section-heading"><h2>{report.summary}</h2>{report.triage !== 'hidden' && <Link to={`/reports/${report.id}`}>Open public view <ArrowRight size={15} /></Link>}</div><div className="detail-grid"><div><div className="private-log-panel"><div className="log-file-head"><LockKeyhole size={17} /><strong>Private log</strong>{report.hasLog && <a className="text-link ml-auto" href={`/api/maintainer/reports/${report.id}/log`} download="eden_log.txt"><Download size={15} /> Download .txt</a>}</div>{report.hasLog ? <>{log === null ? <div className="empty"><p>Opening this log creates an access audit record.</p><button className="button" onClick={() => { void readLog(); }} disabled={busy}>Open private log</button></div> : <><div className="log-controls"><select aria-label="Log level" value={level} onChange={e => setLevel(e.target.value)}>{['All', 'Critical', 'Error', 'Warning', 'Info', 'Debug'].map(value => <option key={value}>{value}</option>)}</select><label><Search size={15} /><input aria-label="Search private log" placeholder="Search the log" value={query} onChange={e => setQuery(e.target.value)} /></label></div><div className="log-lines" tabIndex={0}>{lines?.map(line => <div key={line.line} className={line.text.includes('<Critical>') || line.text.includes('<Error>') ? 'error-line' : line.text.includes('<Warning>') ? 'warning-line' : ''}><span>{line.line}</span><code>{line.text}</code></div>)}{lines?.length === 0 && <p>No matching lines.</p>}</div></>}</> : <Empty title="No private log available">No log was attached, or it has been deleted or expired.</Empty>}</div><article className="report-body"><section><h3 className="lined-heading">Reporter’s description</h3><p className="preserve-lines">{report.details}</p></section><section><h3 className="lined-heading">Reproduction steps</h3><p className="preserve-lines">{report.reproduction || 'Not supplied.'}</p></section><section><h3 className="lined-heading">Settings</h3><p className="preserve-lines">{report.settings}</p></section></article></div><aside className="detail-sidebar"><section><h3 className="lined-heading">Triage</h3><label className="field mb-5"><span>State</span><select value={triage} onChange={e => { const result = z.enum(['new', 'reviewed', 'needs-info', 'hidden']).safeParse(e.target.value); if (result.success) setTriage(result.data); }}><option value="new">New</option><option value="reviewed">Reviewed</option><option value="needs-info">Needs information</option><option value="hidden">Hidden from public</option></select></label><label className="field"><span>Internal note</span><textarea rows={5} value={internalNote} onChange={e => setInternalNote(e.target.value)} maxLength={6000} /><small>Only the maintainer can see this note.</small></label><button className="button primary mt-4" disabled={busy} onClick={() => { void save(); }}>Save triage</button>{message && <p className="field-error mt-3" role="alert">{message}</p>}</section><ReportSetup report={report} />{report.hasLog && <div className="panel"><h3>Log retention</h3><p>Logs expire after 90 days. Deleting this file now cannot be undone. The public report stays.</p>{confirmDelete ? <div className="flex flex-wrap gap-3 mt-4"><button className="button danger" disabled={busy} onClick={() => { void deleteLog(); }}>Delete permanently</button><button className="button" onClick={() => setConfirmDelete(false)}>Cancel</button></div> : <button className="text-link mt-4" onClick={() => setConfirmDelete(true)}><Trash2 size={15} /> Delete log now</button>}</div>}</aside></div></section>;
}
