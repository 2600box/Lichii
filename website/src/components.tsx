import { ArrowRight, ChevronLeft, ChevronRight, FileText, Gamepad2, LockKeyhole, Search, ShieldCheck } from 'lucide-react';
import { Link, NavLink, useLocation } from 'react-router-dom';
import { useEffect, type ReactNode } from 'react';
import { statuses, type Game, type PublicReport } from '../shared/domain.ts';

export function Layout({ children }: { children: ReactNode }) {
  const location = useLocation();
  useEffect(() => { window.scrollTo(0, 0); }, [location.pathname]);
  return <><a className="skip-link" href="#main">Skip to content</a><header className="site-header"><div className="header-inner">
    <Link to="/" className="brand" aria-label="Lichii compatibility home"><img src="/branding/lockup.png" width="122" height="56" alt="Lichii" /><span>compat · Switch 2</span></Link>
    <nav aria-label="Main navigation"><NavLink to="/" end>Games</NavLink><NavLink to="/reports">Recent reports</NavLink><NavLink to="/guide">How to report</NavLink></nav>
    <Link to="/submit" className="button primary header-cta">Submit a report</Link>
  </div></header><main id="main" className="page">{children}</main><footer className="site-footer"><p>Lichii is an independent, experimental emulator, not affiliated with Nintendo.</p><div><Link to="/privacy">Privacy & safety</Link><Link to="/maintainer"><ShieldCheck size={14} /> Maintainer</Link></div></footer></>;
}
export function Status({ value, large = false }: { value: Game['status']; large?: boolean }) {
  return <span className={`status status-${value.toLowerCase()} ${large ? 'status-large' : ''}`}><i aria-hidden="true" />{value}</span>;
}
export function Cover({ game, large = false }: { game: Pick<Game, 'title' | 'coverPath'>; large?: boolean }) {
  return game.coverPath ? <img className={`cover ${large ? 'cover-large' : ''}`} src={game.coverPath} alt={`${game.title} cover`} loading={large ? 'eager' : 'lazy'} /> : <div className={`cover cover-missing ${large ? 'cover-large' : ''}`} aria-label="Cover art unavailable"><Gamepad2 size={large ? 40 : 22} /></div>;
}
export function Empty({ title, children, action }: { title: string; children: ReactNode; action?: ReactNode }) {
  return <div className="empty"><div className="empty-icon"><FileText size={25} strokeWidth={1.4} /></div><h3>{title}</h3><p>{children}</p>{action}</div>;
}
export function Loading() { return <div role="status" className="loading"><span className="loading-dot" />Loading…</div>; }
export function ErrorNotice({ message }: { message: string }) { return <div role="alert" className="notice error"><strong>We couldn’t load that.</strong><p>{message}</p><button className="text-link" onClick={() => window.location.reload()}>Try again</button></div>; }
export function Pagination({ page, pageCount, total, onPage }: { page: number; pageCount: number; total: number; onPage: (page: number) => void }) {
  return <div className="pagination"><span>{total === 0 ? 'No results' : `Showing ${(page - 1) * 15 + 1}–${Math.min(page * 15, total)} of ${total}`}</span><div><button className="button compact" disabled={page <= 1} onClick={() => onPage(page - 1)} aria-label="Previous page"><ChevronLeft size={15} /></button><span className="page-number">{page} <span className="muted">/ {pageCount}</span></span><button className="button compact" disabled={page >= pageCount} onClick={() => onPage(page + 1)} aria-label="Next page"><ChevronRight size={15} /></button></div></div>;
}
export function SearchBox({ value, onChange, placeholder }: { value: string; onChange: (value: string) => void; placeholder: string }) {
  return <label className="search-box"><Search size={23} strokeWidth={1.6} /><input type="search" aria-label={placeholder} value={value} onChange={e => onChange(e.target.value)} placeholder={placeholder} maxLength={150} /><kbd aria-hidden="true">⌕</kbd></label>;
}
export function Filters({ status, gpu, os, onChange }: { status: string; gpu: string; os: string; onChange: (key: string, value: string) => void }) {
  return <div className="filter-group"><label>Status <select aria-label="Filter by status" value={status} onChange={e => onChange('status', e.target.value)}><option value="Any">Any result</option>{statuses.map(s => <option key={s}>{s}</option>)}</select></label><label>GPU <select aria-label="Filter by GPU" value={gpu} onChange={e => onChange('gpu', e.target.value)}><option value="">Any vendor</option><option>NVIDIA</option><option>AMD</option><option>Intel</option><option>Apple</option></select></label><label>OS <select aria-label="Filter by operating system" value={os} onChange={e => onChange('os', e.target.value)}><option value="">Any</option><option>Windows</option><option>Linux</option><option>Android</option><option>macOS</option></select></label></div>;
}
export function ReportCard({ report }: { report: PublicReport }) {
  return <article className="report-card"><div className="flex flex-wrap items-center gap-3"><Status value={report.outcome} /><Link to={`/reports/${report.id}`} className="report-title">{report.summary}</Link></div><p className="report-game"><Link to={`/games/${report.gameSlug}`}>{report.gameTitle}</Link><span>by {report.name} · {formatDate(report.createdAt)}</span></p><p className="report-excerpt">{report.details}</p><div className="tags"><span>{report.build}</span><span>v{report.gameVersion.replace(/^v/i, '')}</span><span>{report.gpu}</span><span>{report.os}</span><span>{report.graphicsApi}</span></div><Link className="text-link report-open" to={`/reports/${report.id}`}>Open report <ArrowRight size={15} /></Link></article>;
}
export function Contribute({ game }: { game?: Game }) {
  return <aside className="contribute panel"><h2>{game ? `Played ${game.title} on Lichii?` : 'Played a Switch 2 game on Lichii? Tell people how it ran.'}</h2><p className="muted">A useful report starts with what you actually tested.</p><div className="contribute-steps"><div><strong>eden_log.txt</strong><span>Private to the maintainer</span></div><div><strong>Build and hardware</strong><span>Read from the log, you check</span></div><div><strong>Outcome</strong><span>You pick it</span></div><div><strong>What happened</strong><span>You write it</span></div></div><Link className="button primary" to={game ? `/submit?game=${game.slug}` : '/submit'}>Submit a report</Link><Link className="button" to="/guide">How to report <ArrowRight size={15} /></Link><p className="fine-print">No account needed, just a name. Never upload game files, keys, firmware, saves or personal information.</p></aside>;
}
export function PrivateNote() { return <span className="private-note"><LockKeyhole size={14} /> Log files are never public</span>; }
export function Breadcrumbs({ items }: { items: { label: string; to?: string }[] }) {
  return <nav aria-label="Breadcrumb" className="breadcrumbs"><Link to="/">Games</Link>{items.map((item, i) => <span key={i}>/ {item.to ? <Link to={item.to}>{item.label}</Link> : <strong>{item.label}</strong>}</span>)}</nav>;
}
export function formatDate(value: string) { return new Date(value).toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' }); }
