"use client";
// The admin page, in two halves.
//
// Dashboard: everything the service holds, which is deliberately more than the
// public pages show. Those list approved players only; this one counts every
// player, including the ones still one click from appearing, so it answers the
// question the public pages cannot: is anyone actually playing this?
//
// Mail: the Microsoft Graph credentials (write-only) and the mail log.
//
// Both guarded by middleware.ts (HTTP Basic with ADMIN_PASSWORD).
import { useEffect, useState } from "react";

type Status = { settings: Record<string, { set: boolean; updated_at: string | null; from_env: boolean }>; mail_mode: string; mail_log: { id: number; subject: string; status: number | null; detail: string | null; sent_at: string }[] };

type Overview = {
  totals: { players: number; approved: number; waiting: number; accounts: number; machines: number; runs: number; hidden_runs: number; seconds: number; kills: number; blocks: number; best_score: number; runs_today: number; runs_week: number };
  registrations: { status: string; n: number }[];
  daily: { day: string; runs: number; players: number }[];
  versions: { version: string; runs: number; players: number; last: string }[];
  platforms: { platform: string; runs: number }[];
  players: { id: string; name: string; approved: boolean; runs: number; best: number; seconds: number; machines: number; trophies: number; created_at: string; last_seen: string }[];
  accounts: { id: string; email: string; players: number; runs: number; created_at: string; last_seen: string }[];
  waiting: { id: string; player_id: string; name: string; machine: string; email: string; runs: number; created_at: string; expires_at: string }[];
  hardware: { counted: number; os: { name: string; n: number }[]; gpu: { name: string; n: number }[]; pads: { name: string; n: number }[]; avg_cores: number; avg_ram_gb: number };
  recent: { id: string; at: string; name: string; approved: boolean; score: number; level: number; fights: number; kills: number; red_lines: number; duration_s: number; version: string; platform: string; death_cause: string; voxels: boolean; brutal: boolean }[];
  trophies: { id: string; n: number }[];
};

const FIELDS: { key: string; label: string; hint: string; secret?: boolean }[] = [
  { key: "graph_tenant_id", label: "Tenant ID", hint: "Entra admin center > App registrations > your app > Directory (tenant) ID" },
  { key: "graph_client_id", label: "Client ID", hint: "Application (client) ID of the app registration" },
  { key: "graph_client_secret", label: "Client secret", hint: "Certificates & secrets > New client secret (the value, not the ID). Write-only: it is never shown again.", secret: true },
  { key: "graph_sender", label: "Sender mailbox", hint: "A mailbox in the tenant the app may send as, e.g. redline@yourdomain.com" },
];

const num = (n: number) => n.toLocaleString("en-US");
const hours = (s: number) => (s >= 3600 ? `${Math.round(s / 360) / 10}h` : `${Math.round(s / 60)}m`);
const when = (iso: string) => iso.replace("T", " ").slice(0, 16);

export default function AdminPage() {
  const [tab, setTab] = useState<"dash" | "mail">("dash");
  const [status, setStatus] = useState<Status | null>(null);
  const [ov, setOv] = useState<Overview | null>(null);
  const [form, setForm] = useState<Record<string, string>>({});
  const [msg, setMsg] = useState<{ ok: boolean; text: string } | null>(null);
  const [testTo, setTestTo] = useState("");

  async function load() {
    const r = await fetch("/api/admin/settings", { cache: "no-store" });
    if (r.ok) setStatus((await r.json()) as Status);
    else setMsg({ ok: false, text: `could not load settings (${r.status})` });
    const o = await fetch("/api/admin/overview", { cache: "no-store" });
    if (o.ok) setOv((await o.json()) as Overview);
  }
  useEffect(() => { void load(); }, []);

  async function save(e: React.FormEvent) {
    e.preventDefault();
    const r = await fetch("/api/admin/settings", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(form) });
    const j = (await r.json()) as { ok?: boolean; updated?: number; error?: string };
    setMsg(r.ok ? { ok: true, text: `saved ${j.updated} value(s)` } : { ok: false, text: j.error ?? `error ${r.status}` });
    setForm({});
    void load();
  }

  async function test(e: React.FormEvent) {
    e.preventDefault();
    setMsg({ ok: true, text: "sending..." });
    const r = await fetch("/api/admin/testmail", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ to: testTo }) });
    const j = (await r.json()) as { ok?: boolean; status?: number; detail?: string; error?: string };
    setMsg({ ok: !!j.ok, text: j.ok ? `sent (Graph ${j.status})` : `${j.error ?? j.detail ?? "failed"} (status ${j.status ?? r.status})` });
    void load();
  }

  const peak = ov ? Math.max(1, ...ov.daily.map((d) => d.runs)) : 1;

  return (
    <>
      <h1>Admin</h1>
      <div className="tabs">
        <a className={tab === "dash" ? "on" : ""} onClick={() => setTab("dash")} style={{ cursor: "pointer" }}>Dashboard</a>
        <a className={tab === "mail" ? "on" : ""} onClick={() => setTab("mail")} style={{ cursor: "pointer" }}>Mail &amp; settings</a>
      </div>
      {msg ? <p className={msg.ok ? "ok" : "bad"}>{msg.text}</p> : null}

      {tab === "dash" ? (
        !ov ? <div className="empty">Loading…</div> : (
          <>
            <p className="sub">Everything collected, including players who have not approved their address yet. The public pages show approved players only.</p>
            <div className="cards">
              <div className="card"><div className="big">{num(ov.totals.players)}</div><div className="label">Players · {ov.totals.approved} approved, {ov.totals.waiting} waiting</div></div>
              <div className="card"><div className="big">{num(ov.totals.runs)}</div><div className="label">Games posted{ov.totals.hidden_runs ? ` · ${ov.totals.hidden_runs} hidden` : ""}</div></div>
              <div className="card"><div className="big">{num(ov.totals.runs_today)}</div><div className="label">Games in 24 hours</div></div>
              <div className="card"><div className="big">{num(ov.totals.runs_week)}</div><div className="label">Games this week</div></div>
              <div className="card"><div className="big">{hours(ov.totals.seconds)}</div><div className="label">Played in total</div></div>
              <div className="card"><div className="big">{num(ov.totals.best_score)}</div><div className="label">Best score posted</div></div>
              <div className="card"><div className="big">{num(ov.totals.kills)}</div><div className="label">Demons slain</div></div>
              <div className="card"><div className="big">{num(ov.totals.machines)}</div><div className="label">Machines · {ov.totals.accounts} addresses</div></div>
            </div>

            {ov.waiting.length ? (
              <>
                <h2>Waiting for approval</h2>
                <p className="sub">One click on the emailed link and these appear everywhere. Declining deletes them.</p>
                <table><thead><tr><th>Player</th><th>Address</th><th>Machine</th><th className="num">Games held</th><th>Asked</th><th>Link expires</th></tr></thead><tbody>
                  {ov.waiting.map((w) => <tr key={w.id}><td>{w.name}</td><td><a href={`mailto:${w.email}`}>{w.email}</a></td><td>{w.machine || "unknown"}</td><td className="num">{w.runs}</td><td>{when(w.created_at)}</td><td>{when(w.expires_at)}</td></tr>)}
                </tbody></table>
              </>
            ) : null}

            <h2>Last 14 days</h2>
            {ov.daily.length ? (
              <table><thead><tr><th>Day</th><th className="num">Games</th><th className="num">Players</th><th style={{ width: "60%" }}></th></tr></thead><tbody>
                {ov.daily.map((d) => (
                  <tr key={d.day}>
                    <td>{d.day}</td><td className="num">{d.runs}</td><td className="num">{d.players}</td>
                    <td><div style={{ background: "var(--red-dark)", height: 10, borderRadius: 3, width: `${Math.round((100 * d.runs) / peak)}%`, minWidth: 2 }} /></td>
                  </tr>
                ))}
              </tbody></table>
            ) : <div className="empty">No games posted in the last fortnight.</div>}

            <div className="grid2">
              <div>
                <h2>Versions in the wild</h2>
                <table><thead><tr><th>Version</th><th className="num">Games</th><th className="num">Players</th><th>Last seen</th></tr></thead><tbody>
                  {ov.versions.length ? ov.versions.map((v) => <tr key={v.version}><td>{v.version}</td><td className="num">{v.runs}</td><td className="num">{v.players}</td><td>{when(v.last)}</td></tr>)
                    : <tr><td colSpan={4} className="empty">Nothing yet.</td></tr>}
                </tbody></table>
                <h2>Platforms</h2>
                <table><thead><tr><th>Platform</th><th className="num">Games</th></tr></thead><tbody>
                  {ov.platforms.map((p) => <tr key={p.platform}><td>{p.platform}</td><td className="num">{p.runs}</td></tr>)}
                </tbody></table>
                <h2>Registrations</h2>
                <table><thead><tr><th>Status</th><th className="num">Count</th></tr></thead><tbody>
                  {ov.registrations.length ? ov.registrations.map((r) => <tr key={r.status}><td>{r.status}</td><td className="num">{r.n}</td></tr>)
                    : <tr><td colSpan={2} className="empty">None.</td></tr>}
                </tbody></table>
              </div>
              <div>
                <h2>What they play on</h2>
                <p className="sub">From {ov.hardware.counted} machine{ov.hardware.counted === 1 ? "" : "s"}, decrypted here and nowhere else. Average {ov.hardware.avg_cores.toFixed(1)} cores, {ov.hardware.avg_ram_gb.toFixed(1)} GB.</p>
                <table><thead><tr><th>Operating system</th><th className="num">Machines</th></tr></thead><tbody>
                  {ov.hardware.os.length ? ov.hardware.os.map((o) => <tr key={o.name}><td>{o.name}</td><td className="num">{o.n}</td></tr>)
                    : <tr><td colSpan={2} className="empty">Nothing recorded.</td></tr>}
                </tbody></table>
                <table><thead><tr><th>GPU</th><th className="num">Machines</th></tr></thead><tbody>
                  {ov.hardware.gpu.map((g) => <tr key={g.name}><td>{g.name}</td><td className="num">{g.n}</td></tr>)}
                </tbody></table>
                {ov.hardware.pads.length ? (
                  <table><thead><tr><th>Gamepad</th><th className="num">Machines</th></tr></thead><tbody>
                    {ov.hardware.pads.map((g) => <tr key={g.name}><td>{g.name}</td><td className="num">{g.n}</td></tr>)}
                  </tbody></table>
                ) : null}
              </div>
            </div>

            <h2>Players</h2>
            <table><thead><tr><th>Name</th><th></th><th className="num">Games</th><th className="num">Best</th><th className="num">Played</th><th className="num">Machines</th><th className="num">Trophies</th><th>First seen</th><th>Last seen</th></tr></thead><tbody>
              {ov.players.length ? ov.players.map((p) => (
                <tr key={p.id}>
                  <td>{p.approved ? <a href={`/player/${p.id}`}>{p.name}</a> : p.name}</td>
                  <td>{p.approved ? <span className="tier top">approved</span> : <span className="tier">waiting</span>}</td>
                  <td className="num">{p.runs}</td><td className="num">{num(p.best)}</td><td className="num">{hours(p.seconds)}</td>
                  <td className="num">{p.machines}</td><td className="num">{p.trophies}</td>
                  <td>{when(p.created_at)}</td><td>{when(p.last_seen)}</td>
                </tr>
              )) : <tr><td colSpan={9} className="empty">Nobody has registered yet.</td></tr>}
            </tbody></table>

            <h2>Recent games</h2>
            <table><thead><tr><th>When</th><th>Player</th><th className="num">Score</th><th className="num">Level</th><th className="num">Fights</th><th className="num">Kills</th><th className="num">Length</th><th>Ended</th><th>Version</th><th>Platform</th></tr></thead><tbody>
              {ov.recent.length ? ov.recent.map((r) => (
                <tr key={r.id}>
                  <td>{when(r.at)}</td>
                  <td>{r.name}{r.approved ? "" : " (waiting)"}</td>
                  <td className="num">{num(r.score)}</td><td className="num">{r.level}</td><td className="num">{r.fights}</td><td className="num">{r.kills}</td>
                  <td className="num">{hours(r.duration_s)}</td>
                  <td>{r.death_cause === "killed" ? "killed" : r.death_cause === "stack" ? "stack" : r.death_cause}</td>
                  <td>{r.version}</td><td>{r.platform}</td>
                </tr>
              )) : <tr><td colSpan={10} className="empty">No games posted yet.</td></tr>}
            </tbody></table>

            <div className="grid2 lead">
              <div>
                <h2>Addresses</h2>
                <p className="sub">Stored as AES-256-GCM ciphertext and decrypted here with the key from the environment. One address can carry several players.</p>
                <table><thead><tr><th>Address</th><th className="num">Players</th><th className="num">Games</th><th>Approved</th><th>Last seen</th></tr></thead><tbody>
                  {ov.accounts.length ? ov.accounts.map((a) => (
                    <tr key={a.id}>
                      <td><a href={`mailto:${a.email}`}>{a.email}</a></td>
                      <td className="num">{a.players}</td><td className="num">{a.runs}</td>
                      <td>{when(a.created_at)}</td><td>{a.last_seen ? when(a.last_seen) : "—"}</td>
                    </tr>
                  )) : <tr><td colSpan={5} className="empty">None yet.</td></tr>}
                </tbody></table>
              </div>
              <div>
                <h2>Trophies unlocked</h2>
                <table><thead><tr><th>Trophy</th><th className="num">Players</th></tr></thead><tbody>
                  {ov.trophies.length ? ov.trophies.map((t) => <tr key={t.id}><td>{t.id}</td><td className="num">{t.n}</td></tr>)
                    : <tr><td colSpan={2} className="empty">None yet.</td></tr>}
                </tbody></table>
              </div>
            </div>
          </>
        )
      ) : (
        <>
          <p className="sub">Mail goes out through Microsoft Graph. Mode: <b>{status?.mail_mode ?? "..."}</b>{status?.mail_mode === "log" ? " (MAIL_MODE=log: the approve link is written to the log below instead of being sent)" : ""}.</p>
          <form className="admin" onSubmit={save}>
            {FIELDS.map((f) => {
              const st = status?.settings[f.key];
              return (
                <div key={f.key}>
                  <label>{f.label} {st?.set ? <span className="ok">· set{st.updated_at ? ` on ${st.updated_at.slice(0, 10)}` : st.from_env ? " (from environment)" : ""}</span> : <span className="bad">· not set</span>}</label>
                  <input type={f.secret ? "password" : "text"} autoComplete="off" placeholder={f.hint} value={form[f.key] ?? ""} onChange={(e) => setForm({ ...form, [f.key]: e.target.value })} />
                </div>
              );
            })}
            <button type="submit">Save the values entered above</button>
          </form>
          <form className="admin" onSubmit={test}>
            <label>Send a test mail to</label>
            <input type="email" value={testTo} onChange={(e) => setTestTo(e.target.value)} placeholder="you@example.com" />
            <button type="submit">Send test mail</button>
          </form>
          <h2>Last 20 mails</h2>
          {status?.mail_log.length ? (
            <table><thead><tr><th>When</th><th>Subject</th><th className="num">Status</th><th>Detail</th></tr></thead><tbody>
              {status.mail_log.map((m) => <tr key={m.id}><td>{m.sent_at.replace("T", " ").slice(0, 19)}</td><td>{m.subject}</td><td className={"num " + (m.status === 202 || m.status === 0 ? "ok" : "bad")}>{m.status}</td><td style={{ whiteSpace: "pre-wrap", fontSize: 12 }}>{m.detail}</td></tr>)}
            </tbody></table>
          ) : <div className="empty">No mail sent yet.</div>}
        </>
      )}
    </>
  );
}
