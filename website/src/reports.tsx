import { Link, useParams, useSearchParams } from 'react-router-dom';
import { ArrowRight, Check, LockKeyhole, MessageSquare, ShieldCheck } from 'lucide-react';
import { publicReportSchema, reportsResponseSchema, type PublicReport } from '../shared/domain.ts';
import { useApi, useDebounced } from './api.ts';
import { Breadcrumbs, Empty, ErrorNotice, Filters, Loading, Pagination, PrivateNote, ReportCard, SearchBox, Status, formatDate } from './components.tsx';

export function RecentReports() {
  const [params, setParams] = useSearchParams();
  const search = params.get('q') ?? '';
  const debounced = useDebounced(search);
  const query = new URLSearchParams(params); query.set('q', debounced);
  const state = useApi(`/api/reports?${query}`, reportsResponseSchema);
  function filter(key: string, value: string) { const next = new URLSearchParams(params); next.set(key, value); next.delete('page'); setParams(next, { replace: true }); }
  return <><div className="page-heading"><div><h1 className="page-title">Recent reports</h1><p className="page-description">Real tests. Specific builds. The details that help the next player.</p></div><Link className="button primary" to="/submit">Submit a report <ArrowRight size={16} /></Link></div><SearchBox value={search} onChange={value => filter('q', value)} placeholder="Search reports or games" /><div className="filter-bar"><Filters status={params.get('status') ?? 'Any'} gpu={params.get('gpu') ?? ''} os={params.get('os') ?? ''} onChange={filter} /><span className="muted text-sm">Newest first</span></div><div className="reports-wide">{state.kind === 'loading' ? <Loading /> : state.kind === 'error' ? <ErrorNotice message={state.message} /> : <>{state.data.reports.length ? state.data.reports.map(report => <ReportCard key={report.id} report={report} />) : <Empty title="No reports here yet">{params.size ? 'No reports match these filters. Try a broader search.' : 'The catalog is ready. The testing starts with you. Share what happened on your setup.'}<Link className="button primary mt-6" to="/submit">Write the first report <ArrowRight size={16} /></Link></Empty>}<Pagination {...state.data} onPage={page => { const next = new URLSearchParams(params); next.set('page', String(page)); setParams(next); }} /></>}</div><div className="notice mt-10"><ShieldCheck size={20} /><p>Reports are community observations, not guarantees or official compatibility ratings. The latest result can differ from another build or setup.</p></div></>;
}
export function PublicReportPage() {
  const { id } = useParams();
  const state = useApi(`/api/reports/${encodeURIComponent(id ?? '')}`, publicReportSchema);
  if (state.kind === 'loading') return <Loading />;
  if (state.kind === 'error') return <ErrorNotice message={state.message} />;
  const report = state.data;
  return <><Breadcrumbs items={[{ label: report.gameTitle, to: `/games/${report.gameSlug}` }, { label: 'Community report' }]} /><div className="public-report-heading"><div className="flex flex-wrap items-center gap-4 mb-5"><Status value={report.outcome} /><Link to={`/games/${report.gameSlug}`}>{report.gameTitle}</Link><span className="muted">{formatDate(report.createdAt)}</span></div><h1 className="page-title">{report.summary}</h1><div className="report-byline"><span className="avatar">{report.name.slice(0, 1).toUpperCase()}</span><div><strong>{report.name}</strong>{report.handle && <a href={`https://x.com/${report.handle.replace(/^@/, '')}`} target="_blank" rel="noreferrer">@{report.handle.replace(/^@/, '')}</a>}</div><span className="triage-badge">{report.triage === 'new' ? 'Community report · not reviewed' : report.triage === 'needs-info' ? 'Maintainer requested more information' : 'Reviewed by maintainer'}</span></div></div><div className="detail-grid"><article className="report-body"><section><h2 className="lined-heading">What happened</h2><p className="preserve-lines">{report.details}</p></section><section><h2 className="lined-heading">How far they got</h2><p className="preserve-lines">{report.progress}</p></section><section><h2 className="lined-heading">Steps to reproduce</h2><p className="preserve-lines">{report.reproduction || 'No reproduction steps supplied.'}</p></section><section><h2 className="lined-heading">Settings</h2><p className="preserve-lines">{report.settings}</p></section><div className="notice"><LockKeyhole size={20} /><p>Any attached log is private to the maintainer. Public pages never include log contents or filenames.</p></div></article><aside className="detail-sidebar"><ReportSetup report={report} /><div className="panel"><MessageSquare size={24} className="text-accent mb-4" /><h3>Different result on your setup?</h3><p>Builds and hardware matter. Add your own test so other players can compare.</p><Link className="button primary mt-5" to={`/submit?game=${report.gameSlug}`}>Submit your result <ArrowRight size={15} /></Link></div><PrivateNote /></aside></div></>;
}
export function ReportSetup({ report }: { report: PublicReport }) {
  return <section><h3 className="lined-heading">Tested on this setup</h3><dl className="facts">{[
    ['Lichii build', report.build], ['Game version', report.gameVersion], ['CPU', report.cpu], ['GPU', report.gpu], ['GPU driver', report.driver], ['Operating system', report.os], ['RAM', report.ram || 'Not supplied'], ['Graphics API', report.graphicsApi], ['Updates / DLC', report.updates || 'Not supplied'], ['Mods / cheats', report.mods || 'Not supplied'],
  ].map(([label, value]) => <div key={label}><dt>{label}</dt><dd>{value}</dd></div>)}</dl></section>;
}
export function Submitted() {
  const { id } = useParams();
  const state = useApi(`/api/reports/${encodeURIComponent(id ?? '')}`, publicReportSchema);
  if (state.kind === 'loading') return <Loading />;
  if (state.kind === 'error') return <ErrorNotice message={state.message} />;
  return <div className="success-page"><div className="success-icon"><Check size={40} strokeWidth={1.5} /></div><span className="eyebrow">Report published</span><h1>Thanks for testing.</h1><p>Your report for <strong>{state.data.gameTitle}</strong> is now public.<br />One more real result for the next player.</p><div className="success-card panel"><Status value={state.data.outcome} /><h2>{state.data.summary}</h2><span className="muted">{state.data.name} · {formatDate(state.data.createdAt)}</span><div className="tags mt-5"><span>{state.data.build}</span><span>{state.data.gpu}</span><span>{state.data.os}</span></div></div><div className="flex flex-wrap justify-center gap-3"><Link className="button primary" to={`/reports/${state.data.id}`}>View your public report <ArrowRight size={16} /></Link><Link className="button" to={`/games/${state.data.gameSlug}`}>Back to the game</Link></div><div className="success-private"><PrivateNote /><p>If you attached a log, only the authorized maintainer can read it. It will be deleted within 90 days. Your public report stays.</p></div></div>;
}
