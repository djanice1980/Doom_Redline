// POST /api/admin/testmail {to}: send a test message through Graph with the stored credentials.
import { isEmail } from "@/lib/crypto";
import { sendMail } from "@/lib/graph";
import { adminChallenge, error, isAdmin, json } from "@/lib/auth";

export const runtime = "nodejs";

export async function POST(req: Request) {
  if (!isAdmin(req)) return adminChallenge();
  let body: Record<string, unknown>;
  try { body = (await req.json()) as Record<string, unknown>; } catch { return error("invalid JSON"); }
  if (!isEmail(body.to)) return error("to must be an email address");
  const result = await sendMail({ to: body.to, subject: "REDLINE test mail", text: "If you can read this, the REDLINE service can send mail through Microsoft Graph." });
  return json(result, result.ok ? 200 : 502);
}
