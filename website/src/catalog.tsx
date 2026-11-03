import { useState } from 'react';
import { Link, useParams, useSearchParams } from 'react-router-dom';
import { ArrowRight, ArrowUpRight, Database, Info } from 'lucide-react';
import { gameSchema, gamesResponseSchema, reportsResponseSchema, statuses } from '../shared/domain.ts';
import { useApi, useDebounced } from './api.ts';
import { Breadcrumbs, Contribute, Cover, Empty, ErrorNotice, Filters, Loading, Pagination, ReportCard, SearchBox, Status, formatDate } from './components.tsx';

export function Catalog() {
  const [params, setParams] = useSearchParams();
  const search = params.get('q') ?? '';
  const query = useDebounced(search);
  const actual = new URLSearchParams(params);
  actual.set('q', query);
  const state = useApi(`/api/games?${actual}`, gamesResponseSchema);
  const recent = useApi('/api/reports', reportsResponseSchema);
  function filter(key: string, value: string) { const next = new URLSearchParams(params); next.set(key, value); next.delete('page'); setParams(next, { replace: true }); }
  return <>
    <section className="catalog-intro"><div><h1>Will it run?</h1><p>Players test Switch 2 games on Lichii and report what happened, along with their build, game version, CPU, GPU, driver and settings.<br /> Find a title. Check the setup. Read the reports.</p></div><div className="catalog-overview">
      <div className="overview-title"><strong>{state.kind === 'ready' ? `${state.data.totalGames} Switch 2 titles tracked` : 'Switch 2 compatibility'}</strong><span className="mono">{state.kind === 'ready' ? `${state.data.totalReports} community reports` : 'Community tested'}</span></div>
      <svg className="status-track" viewBox="0 0 1000 12" preserveAspectRatio="none" aria-hidden="true">{statuses.map((status, index) => {
        const total = state.kind === 'ready' ? state.data.totalGames : 0;
        const width = total && state.kind === 'ready' ? state.data.counts[status] / total * 1000 : status === 'Untested' ? 1000 : 0;
        const x = total && state.kind === 'ready' ? statuses.slice(0, index).reduce((sum, key) => sum + state.data.counts[key], 0) / total * 1000 : 0;
        return <rect key={status} className={`track-${status.toLowerCase()}`} x={x} y="0" width={width} height="12" />;
      })}</svg>
      <div className="status-legend">{statuses.map(status => <div key={status}><Status value={status} /><strong>{state.kind === 'ready' ? state.data.counts[status] : '—'}</strong></div>)}</div>
      <p className="fine-print">{state.kind === 'ready' && state.data.totalReports === 0 ? 'A fresh start. Every game is untested until someone submits a report.' : 'Each game shows its latest reported outcome, not a guarantee.'}</p>
    </div></section>
    <section aria-label="Games database"><SearchBox value={search} onChange={value => filter('q', value)} placeholder="Search Switch 2 games" /><div className="filter-bar"><Filters status={params.get('status') ?? 'Any'} gpu={params.get('gpu') ?? ''} os={params.get('os') ?? ''} onChange={filter} /><label className="sort-label">Sort <select aria-label="Sort games" value={params.get('sort') ?? 'popular'} onChange={e => filter('sort', e.target.value)}><option value="popular">Most reported</option><option value="title">Title A–Z</option><option value="recent">Recently tested</option></select></label></div>
      {state.kind === 'loading' ? <Loading /> : state.kind === 'error' ? <ErrorNotice message={state.message} /> : <>
        {state.data.games.length === 0 ? <Empty title="No games found">Try another title or clear your filters. Untested games have no hardware information yet.</Empty> : <div className="table-scroll"><table className="game-table"><thead><tr><th>Game</th><th>Latest reported result</th><th className="number-col">Reports</th><th>Test history</th><th>Latest report</th><th><span className="sr-only">Contribute</span></th></tr></thead><tbody>{state.data.games.map(game => <tr key={game.sgdbId}><td><Link to={`/games/${game.slug}`} className="game-cell"><Cover game={game} /><div><strong>{game.title}</strong><span>Nintendo Switch 2</span></div></Link></td><td><Status value={game.status} /></td><td className="number-col mono">{game.reportCount}</td><td className="muted">{game.reportCount ? <Link to={`/games/${game.slug}`}>Read player setups <ArrowUpRight size={13} className="inline" /></Link> : 'No tests yet'}</td><td className="muted">{game.latestAt ? formatDate(game.latestAt) : '—'}</td><td className="row-action"><Link to={`/submit?game=${game.slug}`}>{game.reportCount ? 'Report' : 'Test it'} <ArrowRight size={14} /></Link></td></tr>)}</tbody></table></div>}
        <Pagination page={state.data.page} pageCount={state.data.pageCount} total={state.data.total} onPage={page => { const next = new URLSearchParams(params); next.set('page', String(page)); setParams(next); }} />
      </>}
      <p className="catalog-source"><Info size={14} /><span>Catalog matched to Nintendo’s European Switch 2 listings and SteamGridDB. Coverage may be incomplete. Community cover art is credited on each game page.</span></p>
    </section>
    <section className="bottom-grid"><div><div className="section-heading"><h2>Latest reports</h2><Link to="/reports">All recent reports <ArrowRight size={15} /></Link></div>{recent.kind === 'ready' && recent.data.reports.length > 0 ? recent.data.reports.slice(0, 3).map(r => <ReportCard key={r.id} report={r} />) : recent.kind === 'error' ? <ErrorNotice message={recent.message} /> : recent.kind === 'loading' ? <Loading /> : <Empty title="The first report could be yours">No results have been submitted yet. Share a real test to help the next player.<Link className="text-link mt-4" to="/guide">What makes a useful report? <ArrowRight size={14} /></Link></Empty>}</div><Contribute /></section>
  </>;
}

export function GameDetail() {
  const { slug } = useParams();
  const state = useApi(`/api/games/${encodeURIComponent(slug ?? '')}`, gameSchema);
  const [page, setPage] = useState(1);
  if (state.kind === 'loading') return <Loading />;
  if (state.kind === 'error') return <ErrorNotice message={state.message} />;
  const game = state.data;
  return <><Breadcrumbs items={[{ label: game.title }]} /><section className={`game-hero ${game.heroPath ? 'has-hero' : ''}`}>
    {game.heroPath && <img className="hero-art" src={game.heroPath} alt="" />}<div className="hero-shade" /><div className="hero-content"><Cover game={game} large /><div><span className="platform-pill">Nintendo Switch 2</span><h1>{game.title}</h1></div></div>
    <a className="hero-credit" href={game.artworkSource ?? `https://www.steamgriddb.com/game/${game.sgdbId}`} target="_blank" rel="noreferrer">Art: SteamGridDB community <ArrowUpRight size={13} /></a>
  </section><div className="detail-grid"><div><section className="game-summary"><div><span className="eyebrow">{game.reportCount ? 'Latest community result' : 'Current status'}</span><Status value={game.status} large /><p className="muted">{game.reportCount ? 'One player’s result on their setup. Read individual reports before drawing conclusions.' : 'Nobody has reported a test yet. We don’t know how this game runs on Lichii.'}</p></div><div className="history-empty"><Database size={25} strokeWidth={1.3} /><strong>{game.reportCount ? `${game.reportCount} community ${game.reportCount === 1 ? 'report' : 'reports'}` : 'No test history yet'}</strong><span>{game.reportCount ? 'Build and hardware details are in each report.' : 'Builds, hardware and results will appear with real reports.'}</span></div></section><div className="detail-stats"><div><strong>{game.reportCount}</strong><span>public reports</span></div><div><strong>{game.latestAt ? formatDate(game.latestAt) : 'Not tested'}</strong><span>latest report</span></div><div><strong>Community</strong><span>reported, not verified</span></div></div>
    <section className="report-section"><div className="section-heading"><h2>Community reports <span className="muted">{game.reportCount}</span></h2><Link to={`/submit?game=${game.slug}`}>Submit yours <ArrowRight size={14} /></Link></div><GameReports id={game.sgdbId} page={page} onPage={setPage} /></section>
  </div><div className="detail-sidebar"><Contribute game={game} /><section><h3 className="lined-heading">About the game</h3><dl className="facts"><div><dt>Platform</dt><dd>Nintendo Switch 2</dd></div><div><dt>Publisher</dt><dd>{game.publisher ?? 'Not recorded'}</dd></div><div><dt>Latest version tested</dt><dd>{game.reportCount ? 'See reports' : 'Not tested'}</dd></div><div><dt>Catalog source</dt><dd><a href={`https://www.steamgriddb.com/game/${game.sgdbId}`} target="_blank" rel="noreferrer">SteamGridDB <ArrowUpRight size={13} /></a></dd></div></dl></section><section><h3 className="lined-heading">Art credits</h3><p className="muted text-sm leading-6">{game.artworkCredit ?? 'No artwork has been added for this title yet.'}</p>{game.artworkSource && <a className="text-link mt-3" href={game.artworkSource} target="_blank" rel="noreferrer">Original artwork <ArrowUpRight size={14} /></a>}</section><div className="notice"><Info size={19} /><p>Untested doesn’t mean unplayable. It means we don’t have evidence yet.</p></div></div></div></>;
}
function GameReports({ id, page, onPage }: { id: number; page: number; onPage: (page: number) => void }) {
  const state = useApi(`/api/reports?gameId=${id}&page=${page}`, reportsResponseSchema);
  if (state.kind === 'loading') return <Loading />;
  if (state.kind === 'error') return <ErrorNotice message={state.message} />;
  return state.data.reports.length ? <>{state.data.reports.map(r => <ReportCard key={r.id} report={r} />)}<Pagination {...state.data} onPage={onPage} /></> : <Empty title="No reports for this game yet">Test it on your setup and tell other players what happened. Good news or a crash, both are useful.</Empty>;
}
