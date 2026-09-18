// HTTP Basic auth in front of the admin page and its API (ADMIN_PASSWORD).
import { NextResponse, type NextRequest } from "next/server";

export const config = { matcher: ["/admin", "/admin/:path*", "/api/admin/:path*"] };

export function middleware(req: NextRequest) {
  const want = process.env.ADMIN_PASSWORD;
  const h = req.headers.get("authorization") ?? "";
  let ok = false;
  if (want && h.startsWith("Basic ")) {
    try {
      const decoded = atob(h.slice(6));
      const pass = decoded.includes(":") ? decoded.slice(decoded.indexOf(":") + 1) : decoded;
      ok = pass.length === want.length && pass === want;
    } catch { ok = false; }
  }
  if (ok) return NextResponse.next();
  return new NextResponse(want ? "Authentication required" : "ADMIN_PASSWORD is not set on the server", { status: 401, headers: { "WWW-Authenticate": 'Basic realm="REDLINE admin"' } });
}
