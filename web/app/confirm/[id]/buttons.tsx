"use client";
import { useState } from "react";

export default function ConfirmButtons({ id, t }: { id: string; t: string }) {
  const [state, setState] = useState<"idle" | "busy" | "confirmed" | "declined" | "error">("idle");
  const [msg, setMsg] = useState("");
  async function act(action: "approve" | "decline") {
    setState("busy");
    const r = await fetch("/api/confirm-link", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ id, t, action }) });
    const j = (await r.json()) as { ok?: boolean; status?: string; error?: string };
    if (r.ok && j.status === "confirmed") setState("confirmed");
    else if (r.ok && j.status === "declined") setState("declined");
    else { setState("error"); setMsg(j.error ?? `error ${r.status}`); }
  }
  if (state === "confirmed") return <p className="ok" style={{ fontSize: 18 }}>Approved. You can close this page: the game picks it up within a few seconds (or the next time it starts).</p>;
  if (state === "declined") return <p>Declined. Nothing is stored. You can close this page.</p>;
  return (
    <div>
      <button type="button" disabled={state === "busy"} onClick={() => act("approve")}>Approve</button>
      {" "}
      <button type="button" disabled={state === "busy"} onClick={() => act("decline")} style={{ background: "#333" }}>Decline</button>
      {state === "error" ? <p className="bad">{msg}</p> : null}
    </div>
  );
}
