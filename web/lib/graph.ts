// Outbound mail through Microsoft Graph (docs/online-and-releases.md section 9).
// Credentials come from the admin page (the `settings` table, encrypted) and fall
// back to environment variables; MAIL_MODE=log writes the mail to the log table
// instead of sending, which is how the smoke test and a fresh deployment run
// before Graph is configured.
import { sql } from "./db";
import { decrypt, encrypt, sha256 } from "./crypto";

export const GRAPH_KEYS = ["graph_tenant_id", "graph_client_id", "graph_client_secret", "graph_sender"] as const;
export type GraphKey = (typeof GRAPH_KEYS)[number];

export async function getSetting(key: string): Promise<string | null> {
  const rows = await sql()`select value_enc from settings where key = ${key}`;
  if (rows.length === 0) return null;
  return decrypt(rows[0].value_enc as string);
}

export async function setSetting(key: string, value: string): Promise<void> {
  await sql()`insert into settings (key, value_enc, updated_at) values (${key}, ${encrypt(value)}, now())
    on conflict (key) do update set value_enc = excluded.value_enc, updated_at = now()`;
}

export async function settingUpdatedAt(key: string): Promise<string | null> {
  const rows = await sql()`select updated_at from settings where key = ${key}`;
  return rows.length ? new Date(rows[0].updated_at as string).toISOString() : null;
}

async function graphConfig() {
  const env: Record<GraphKey, string | undefined> = {
    graph_tenant_id: process.env.GRAPH_TENANT_ID,
    graph_client_id: process.env.GRAPH_CLIENT_ID,
    graph_client_secret: process.env.GRAPH_CLIENT_SECRET,
    graph_sender: process.env.GRAPH_SENDER,
  };
  const out: Record<GraphKey, string> = { graph_tenant_id: "", graph_client_id: "", graph_client_secret: "", graph_sender: "" };
  for (const k of GRAPH_KEYS) out[k] = (await getSetting(k)) ?? env[k] ?? "";
  return out;
}

let tokenCache: { token: string; expires: number } | null = null;

async function graphToken(cfg: Record<GraphKey, string>): Promise<string> {
  if (tokenCache && tokenCache.expires > Date.now() + 60_000) return tokenCache.token;
  const body = new URLSearchParams({
    client_id: cfg.graph_client_id,
    client_secret: cfg.graph_client_secret,
    scope: "https://graph.microsoft.com/.default",
    grant_type: "client_credentials",
  });
  const res = await fetch(`https://login.microsoftonline.com/${encodeURIComponent(cfg.graph_tenant_id)}/oauth2/v2.0/token`, {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body,
  });
  const json = (await res.json()) as { access_token?: string; expires_in?: number; error_description?: string };
  if (!res.ok || !json.access_token) throw new Error(`token request failed: ${res.status} ${json.error_description ?? ""}`.trim());
  tokenCache = { token: json.access_token, expires: Date.now() + (json.expires_in ?? 3600) * 1000 };
  return json.access_token;
}

export interface Mail {
  to: string;
  subject: string;
  text: string;
  html?: string;
}

// Returns the Graph status (202 on success); logs every attempt.
export async function sendMail(mail: Mail): Promise<{ ok: boolean; status: number; detail: string }> {
  const mode = process.env.MAIL_MODE ?? "graph";
  const toHash = sha256(mail.to.toLowerCase());
  if (mode === "log") {
    await sql()`insert into mail_log (to_hash, subject, status, detail) values (${toHash}, ${mail.subject}, ${0}, ${"MAIL_MODE=log: " + mail.text.slice(0, 2000)})`;
    console.log(`[mail:log] to=${mail.to} subject=${mail.subject}\n${mail.text}`);
    return { ok: true, status: 0, detail: "logged, not sent (MAIL_MODE=log)" };
  }
  try {
    const cfg = await graphConfig();
    for (const k of GRAPH_KEYS) if (!cfg[k]) throw new Error(`${k} is not configured (admin page or environment)`);
    const token = await graphToken(cfg);
    const res = await fetch(`https://graph.microsoft.com/v1.0/users/${encodeURIComponent(cfg.graph_sender)}/sendMail`, {
      method: "POST",
      headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
      body: JSON.stringify({
        message: {
          subject: mail.subject,
          body: { contentType: mail.html ? "HTML" : "Text", content: mail.html ?? mail.text },
          toRecipients: [{ emailAddress: { address: mail.to } }],
        },
        saveToSentItems: false,
      }),
    });
    const detail = res.ok ? "sent" : (await res.text()).slice(0, 1000);
    await sql()`insert into mail_log (to_hash, subject, status, detail) values (${toHash}, ${mail.subject}, ${res.status}, ${detail})`;
    return { ok: res.ok, status: res.status, detail };
  } catch (e) {
    const detail = e instanceof Error ? e.message : String(e);
    await sql()`insert into mail_log (to_hash, subject, status, detail) values (${toHash}, ${mail.subject}, ${-1}, ${detail.slice(0, 1000)})`;
    return { ok: false, status: -1, detail };
  }
}

export function registrationMail(to: string, code: string, link: string, displayName: string, machineLabel: string, siteUrl: string): Mail {
  const text = [
    `Someone playing REDLINE as ${displayName} on a machine called ${machineLabel || "unknown"} wants to attach that player to this email address.`,
    "",
    "Approve it by opening this link (it shows an Approve button):",
    link,
    "",
    `Or type this code into the game where it asks:  ${code}`,
    "",
    "Confirming means that, whenever that player has ONLINE switched on, each finished game is posted to the REDLINE leaderboard:",
    "the player name, the score and game statistics, and what the machine is (operating system, GPU, cores, memory, gamepad model).",
    "The address is stored encrypted, is never shown to other players, and can be removed on request.",
    "Each player and each machine that wants to use this address is asked separately. The code expires in 24 hours.",
    "",
    "If this wasn't you, ignore this email and nothing will be stored.",
  ].join("\n");
  const html = `<div style="font-family:system-ui,sans-serif;max-width:560px">
<p>Someone playing <b>REDLINE</b> as <b>${escapeHtml(displayName)}</b> on a machine called <b>${escapeHtml(machineLabel || "unknown")}</b> wants to attach that player to this email address.</p>
<p style="margin:24px 0"><a href="${link}" style="display:inline-block;padding:12px 22px;background:#8c1711;color:#fff;text-decoration:none;border-radius:6px;font-weight:700">Approve this registration</a></p>
<p>Or type this code into the game where it asks:</p>
<p style="font-size:34px;letter-spacing:8px;font-weight:700;margin:12px 0">${code}</p>
<p>The leaderboard lives at <a href="${siteUrl}">${siteUrl}</a>.</p>
<p style="color:#555;font-size:13px">Confirming means that, whenever that player has ONLINE switched on, each finished game is posted to the REDLINE leaderboard: the player name, the score and game statistics, and what the machine is (operating system, GPU, cores, memory, gamepad model). The address is stored encrypted, is never shown to other players, and can be removed on request. Each player and each machine that wants to use this address is asked separately. The code expires in 24 hours.</p>
<p style="color:#555;font-size:13px">If this wasn't you, ignore this email and nothing will be stored.</p>
</div>`;
  return { to, subject: `Confirm REDLINE registration for ${displayName}`, text, html };
}

function escapeHtml(s: string): string {
  return s.replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c] as string);
}
