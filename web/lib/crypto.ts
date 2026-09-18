// Blind index, encryption at rest, tokens and codes (docs/online-and-releases.md
// section 7, "Encryption at rest"). Keys come from the environment only:
//   EMAIL_HMAC_KEY  any long random string; HMAC-SHA256 of the lower-cased address
//   DATA_KEY        32 bytes as base64 or hex; AES-256-GCM for addresses, machine facts, settings
import { createCipheriv, createDecipheriv, createHash, createHmac, randomBytes, randomInt, timingSafeEqual } from "node:crypto";

function need(name: string): string {
  const v = process.env[name];
  if (!v) throw new Error(`${name} is not set`);
  return v;
}

function dataKey(): Buffer {
  const raw = need("DATA_KEY");
  const buf = /^[0-9a-fA-F]{64}$/.test(raw) ? Buffer.from(raw, "hex") : Buffer.from(raw, "base64");
  if (buf.length !== 32) throw new Error("DATA_KEY must be 32 bytes (64 hex chars or 44 base64 chars)");
  return buf;
}

export function normalizeEmail(email: string): string {
  return email.trim().toLowerCase();
}

export function emailHash(email: string): string {
  return createHmac("sha256", need("EMAIL_HMAC_KEY")).update(normalizeEmail(email)).digest("hex");
}

// "v1.<iv>.<ciphertext>.<tag>" in base64url.
export function encrypt(plain: string): string {
  const iv = randomBytes(12);
  const cipher = createCipheriv("aes-256-gcm", dataKey(), iv);
  const enc = Buffer.concat([cipher.update(plain, "utf8"), cipher.final()]);
  return ["v1", iv.toString("base64url"), enc.toString("base64url"), cipher.getAuthTag().toString("base64url")].join(".");
}

export function decrypt(blob: string): string {
  const [v, iv, enc, tag] = blob.split(".");
  if (v !== "v1" || !iv || !enc || !tag) throw new Error("bad ciphertext");
  const decipher = createDecipheriv("aes-256-gcm", dataKey(), Buffer.from(iv, "base64url"));
  decipher.setAuthTag(Buffer.from(tag, "base64url"));
  return Buffer.concat([decipher.update(Buffer.from(enc, "base64url")), decipher.final()]).toString("utf8");
}

export function sha256(s: string): string {
  return createHash("sha256").update(s).digest("hex");
}

export function newToken(): string {
  return randomBytes(32).toString("base64url");
}

export function newCode(): string {
  return String(randomInt(0, 1_000_000)).padStart(6, "0");
}

export function safeEqual(a: string, b: string): boolean {
  const ba = Buffer.from(a), bb = Buffer.from(b);
  return ba.length === bb.length && timingSafeEqual(ba, bb);
}

export function isUuid(s: unknown): s is string {
  return typeof s === "string" && /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(s);
}

export function isEmail(s: unknown): s is string {
  return typeof s === "string" && s.length <= 254 && /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(s);
}
