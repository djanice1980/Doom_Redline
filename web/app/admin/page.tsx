"use client";
// The admin page: Microsoft Graph credentials (write-only), a test mail, and the mail log.
// Guarded by middleware.ts (HTTP Basic with ADMIN_PASSWORD); the browser re-sends the credentials.
import { useEffect, useState } from "react";

type Status = { settings: Record<string, { set: boolean; updated_at: string | null; from_env: boolean }>; mail_mode: string; mail_log: { id: number; subject: string; status: number | null; detail: string | null; sent_at: string }[] };

const FIELDS: { key: string; label: string; hint: string; secret?: boolean }[] = [
  { key: "graph_tenant_id", label: "Tenant ID", hint: "Entra admin center > App registrations > your app > Directory (tenant) ID" },
  { key: "graph_client_id", label: "Client ID", hint: "Application (client) ID of the app registration" },
  { key: "graph_client_secret", label: "Client secret", hint: "Certificates & secrets > New client secret (the value, not the ID). Write-only: it is never shown again.", secret: true },
  { key: "graph_sender", label: "Sender mailbox", hint: "A mailbox in the tenant the app may send as, e.g. redline@yourdomain.com" },
];

export default function AdminPage() {
  const [status, setStatus] = useState<Status | null>(null);
  const [form, setForm] = useState<Record<string, string>>({});
  const [msg, setMsg] = useState<{ ok: boolean; text: string } | null>(null);
  const [testTo, setTestTo] = useState("");

  async function load() {
    const r = await fetch("/api/admin/settings", { cache: "no-store" });
    if (r.ok) setStatus((await r.json()) as Status);
    else setMsg({ ok: false, text: `could not load settings (${r.status})` });
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

  return (
    <>
      <h1>Admin</h1>
      <p className="sub">Mail goes out through Microsoft Graph. Mode: <b>{status?.mail_mode ?? "..."}</b>{status?.mail_mode === "log" ? " (MAIL_MODE=log: codes are written to the log below instead of being sent)" : ""}.</p>
      {msg ? <p className={msg.ok ? "ok" : "bad"}>{msg.text}</p> : null}
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
  );
}
