# REDLINE online service

The leaderboard site and the API the game talks to (registration by emailed
code, run uploads, boards). Next.js on Vercel, Postgres on Supabase, mail
through Microsoft Graph. The design is in `../docs/online-and-releases.md`;
this file is the set-up guide.

Everything below is a one-time setup. Nothing here needs to be typed into the
game: the game already knows the service URL (see "Point the game at it").

## 1. Database (Supabase)

Project: https://uawrxpddntyqkbbjwtut.supabase.co

1. In the Supabase dashboard open **SQL Editor**, paste the whole of
   `supabase/migrations/0001_init.sql`, and run it. It is idempotent: running
   it twice is fine. (Alternative: `DATABASE_URL=... npm run migrate` from this
   folder.)
2. Get the connection string: **Project Settings > Database > Connection
   string**, pick **Transaction** pooler (port 6543), URI form, and put your
   database password in it. That is `DATABASE_URL` below. The service never
   uses the Supabase REST API or the anon key: every table has row-level
   security on with no policies, so the public API cannot read them.

## 2. Secrets

Make four random values (any terminal with OpenSSL):

```bash
openssl rand -base64 32     # DATA_KEY (encryption at rest: addresses, machine facts, Graph credentials)
openssl rand -base64 32     # EMAIL_HMAC_KEY (blind index of addresses)
openssl rand -base64 24     # ADMIN_PASSWORD (the /admin page)
openssl rand -base64 24     # CRON_SECRET (the nightly ratings job)
```

Keep DATA_KEY safe: if it is lost, the stored addresses and credentials
cannot be read back (the game and the boards keep working, and players can
register again).

## 3. Site (Vercel)

1. The project exists: https://vercel.com/djanice1980s-projects/doom-redline
   (imported from `djanice1980/Doom_Redline`; **Settings > General > Root
   Directory** must be `web`, framework Next.js). Its production URL,
   `https://doom-redline.vercel.app`, is the game's built-in default.
2. Under **Environment Variables** add, for Production (and Preview if you
   like):

   | Name | Value |
   |---|---|
   | `DATABASE_URL` | the pooler connection string from step 1 |
   | `DATA_KEY` | from step 2 |
   | `EMAIL_HMAC_KEY` | from step 2 |
   | `ADMIN_PASSWORD` | from step 2 |
   | `CRON_SECRET` | from step 2 (Vercel uses it to call the nightly job) |
   | `SITE_URL` | the site's URL, e.g. `https://doom-redline.vercel.app` |
   | `MAIL_MODE` | `log` until Graph is configured, then `graph` |

3. Deploy. The first visit shows an empty leaderboard. `vercel.json` schedules
   `/api/cron/ratings` nightly at 03:00 UTC (rank tiers).

## 4. Mail (Microsoft Graph)

The confirmation codes go out as email from a mailbox in your Microsoft 365
tenant.

1. **Entra admin center > App registrations > New registration**: any name
   (REDLINE mail), single tenant, no redirect URI.
2. On the app: **API permissions > Add a permission > Microsoft Graph >
   Application permissions > Mail.Send**, then **Grant admin consent**.
3. **Certificates & secrets > New client secret**: copy the *value* (it is
   shown once).
4. Recommended: limit the app to one mailbox with an Exchange application
   access policy, so the secret can only send as that mailbox:
   ```powershell
   Connect-ExchangeOnline
   New-DistributionGroup -Name "REDLINE senders" -Type Security
   Add-DistributionGroupMember -Identity "REDLINE senders" -Member redline@yourdomain.com
   New-ApplicationAccessPolicy -AppId <client id> -PolicyScopeGroupId "REDLINE senders" -AccessRight RestrictAccess
   ```
5. Open `https://<your site>/admin` (user name anything, password
   `ADMIN_PASSWORD`), enter the tenant ID, client ID, client secret and the
   sender mailbox, save, then **Send test mail** to yourself. The values are
   stored encrypted in the database; the secret is write-only.
6. Set `MAIL_MODE` to `graph` in Vercel and redeploy.

Until step 6, registrations are written to the mail log on the admin page
instead of being sent, which lets you test the whole flow yourself: the code
is in the log entry.

## 5. Point the game at it

The game's default service URL is `https://doom-redline.vercel.app`. If your
Vercel project got a different name, either rename the project in Vercel, or
tell the game:

- `server=https://your-site.vercel.app` in `online.txt` in the save folder
  (`~/.local/share/redline/redline/` on Linux, `%APPDATA%\redline\redline\`
  on Windows), or in `redline.cfg` next to the executable (the installer's
  config file), or
- the environment variable `REDLINE_ONLINE_URL`, or `--online-server <url>`.

In the game ONLINE is on by default; players without a registration are
asked once. **PLAYER EMAIL** under OPTIONS sends the registration email:
clicking its approve link (or typing the code) completes it, the game picks
the approval up by polling, and from then on every finished game is posted
with the world rank on the game-over screen. LEADERBOARD on the title screen
shows the boards; WHAT'S NEW shows the change list and the latest release,
which the service reads from GitHub (`/api/version`).

## Migrations after the first

`supabase/migrations/0002_links_and_polling.sql` adds three columns,
`0004_linter_quiet.sql` adds explicit deny-all policies and locks Supabase's
own `rls_auto_enable()` helper away from the REST API, and
`0003_hardening.sql` answers Supabase's database linter (views run as the
caller, a fixed `search_path` on the ratings function, and no privileges at
all for the REST API roles `anon` and `authenticated`). The routes apply both
themselves on first use (`lib/schema.ts`), so nothing breaks if they are not
run by hand; running them in the SQL editor is still tidy and clears the
linter's findings straight away. The remaining "RLS enabled, no policy"
notes are intended: no policy means no access through the REST API, which
is the design.

## Local test

With Docker: `docker run -d --name redline-pg -e POSTGRES_PASSWORD=redline
-e POSTGRES_DB=redline -p 55432:5432 postgres:16`, then in this folder:

```bash
npm install
npm run smoke        # migrates the local database and drives register -> confirm -> runs -> boards -> ratings
npm run build && scripts/local-server.sh   # the production build on http://localhost:3000 with MAIL_MODE=log
```

`scripts/seed-local-player.ts <player_id> <install_id> NAME` registers and
confirms a player without mail and prints the token, for testing the game
against the local server with `--online-server http://localhost:3000`.

## The admin dashboard

`/admin`, behind `ADMIN_PASSWORD`, has two tabs. **Dashboard** is the owner's
view of everything collected, which is deliberately more than the public pages
show: every player including the ones still waiting on their email, games
posted today and this week, the last fortnight day by day, which versions are
in the wild, what people run the game on (the machine facts, decrypted here and
nowhere else, in aggregate), a players table, recent games, and the addresses
masked to a first letter and a domain. **Mail & settings** is the Graph
credentials and the mail log as before.

The point of the first tab is the question the public pages cannot answer: is
anyone playing? A player who has not clicked their approve link appears here,
with the number of games being held for them, and nowhere else.

## What is stored

See `../docs/online-and-releases.md` sections 6, 7 and 9. In short: player
names and gameplay numbers of approved players are public, and nothing at all
is shown for a player whose address has not been approved; email addresses are stored as an
HMAC for lookup plus AES-GCM ciphertext; machine facts (OS, GPU, cores,
memory, gamepad model) are stored encrypted and shown only in aggregate on
the stats page; a player's games are collected from the moment they register
but shown to nobody until the address owner has clicked the emailed Approve
button, and declining deletes them. To remove someone, delete their row in `accounts`: the
players, machines, runs and trophies under it cascade.

A player can also erase itself: `POST /api/player/delete` with that player's
token, which the game sends when someone deletes a registered profile and
asks for the online record to go too. `POST /api/player/adopt` is the other
half of the same story: a profile deleted and made again mints a new player
id, so after a registration the confirm reply lists the account's other
players and the game offers to carry on as one of them, which moves the new
row's runs onto the old one and deletes the new row. The nightly ratings
cron sweeps up player rows with no runs, no trophies and a month of silence,
then accounts with no players left.
