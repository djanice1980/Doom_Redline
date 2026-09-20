// The page behind the emailed approve link: shows what is being approved and two
// buttons. Nothing happens on the visit itself, so mail scanners that prefetch
// links cannot approve by accident.
import { isUuid, decrypt } from "@/lib/crypto";
import { sql } from "@/lib/db";
import ConfirmButtons from "./buttons";
import { ensureSchema } from "@/lib/schema";

export const dynamic = "force-dynamic";

export default async function ConfirmPage({ params, searchParams }: { params: Promise<{ id: string }>; searchParams: Promise<{ t?: string }> }) {
  const { id } = await params;
  const { t } = await searchParams;
  await ensureSchema();
  if (!isUuid(id) || !t) return <Bad text="This link is incomplete. Open it from the email again, or ask the game for a new one." />;
  const regs = await sql()`select display_name, machine_label, email_enc, status, expires_at from registrations where id = ${id}`;
  if (regs.length === 0) return <Bad text="This link is not valid." />;
  const reg = regs[0];
  const status = reg.status as string;
  const expired = new Date(reg.expires_at as string).getTime() < Date.now();
  let email = "";
  try { email = decrypt(reg.email_enc as string); } catch { email = ""; }
  const masked = email.replace(/^(.).*(@.*)$/, "$1…$2");
  if (status === "confirmed") return <Done title="Already approved" text="This registration is approved. The game picks that up by itself; there is nothing more to do." />;
  if (status === "declined") return <Done title="Declined" text="This registration was declined and everything collected for that player has been deleted." />;
  if (status !== "pending" || expired) return <Bad text="This link has expired. Ask for a new one from the game (OPTIONS > PLAYER EMAIL)." />;
  return (
    <>
      <h1>Approve REDLINE?</h1>
      <p className="sub">Someone playing as <b>{reg.display_name as string}</b> on a machine called <b>{(reg.machine_label as string) || "unknown"}</b> wants that player on the leaderboard under <b>{masked}</b>.</p>
      <div className="card" style={{ maxWidth: 640 }}>
        <p><b>Until you approve, that player&apos;s games are collected but shown to nobody</b>: not on the leaderboard, not on a player page, and not in this site&apos;s totals. Approving is what makes them visible. Declining deletes everything collected so far.</p>
        <p>What is posted once approved: the player name, the score and game statistics, and what the machine is (operating system, GPU, cores, memory, gamepad model).</p>
        <p>The address is stored encrypted, is never shown to other players, and can be removed on request. Another profile on a machine you have already approved needs no second email.</p>
        <ConfirmButtons id={id} t={t} />
      </div>
    </>
  );
}

function Bad({ text }: { text: string }) {
  return (<><h1>REDLINE registration</h1><div className="empty">{text}</div></>);
}
function Done({ title, text }: { title: string; text: string }) {
  return (<><h1>{title}</h1><div className="card" style={{ maxWidth: 640 }}>{text}</div></>);
}
