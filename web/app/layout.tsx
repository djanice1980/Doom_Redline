import type { Metadata } from "next";
import Link from "next/link";
import "./globals.css";

export const metadata: Metadata = {
  title: "REDLINE leaderboard",
  description: "Scores, trophies and standings for REDLINE, the falling-block game that tips over into a Doom fight.",
};

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en">
      <body>
        <header className="top">
          <Link href="/" className="brand">REDLINE</Link>
          <nav>
            <Link href="/">Leaderboard</Link>
            <Link href="/trophies">Trophies</Link>
            <Link href="/stats">Stats</Link>
            <a href="https://github.com/djanice1980/Doom_Redline" target="_blank" rel="noreferrer">Get the game</a>
          </nav>
        </header>
        <main>{children}</main>
        <footer>
          Falling blocks that refuse to die. Scores are posted by players who opted in from the game; only player names and gameplay numbers are shown.
          {" "}<a href="https://github.com/djanice1980/Doom_Redline/blob/main/docs/online-and-releases.md" target="_blank" rel="noreferrer">How the data is handled</a>.
        </footer>
      </body>
    </html>
  );
}
