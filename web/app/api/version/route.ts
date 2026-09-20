// GET /api/version: {latest, url, published_at, changelog:[{version, date, items}]}
import { versionInfo } from "@/lib/version";

export const runtime = "nodejs";

export async function GET() {
  const info = await versionInfo();
  return new Response(JSON.stringify(info), { status: 200, headers: { "Content-Type": "application/json", "Cache-Control": "public, max-age=120" } });
}

