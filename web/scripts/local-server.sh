#!/usr/bin/env bash
# Runs the production build against the local test database (see README, "Local test").
cd "$(dirname "$0")/.."
export DATABASE_URL=${DATABASE_URL:-postgres://postgres:redline@localhost:55432/redline}
export EMAIL_HMAC_KEY=${EMAIL_HMAC_KEY:-smoke-hmac-key}
export DATA_KEY=${DATA_KEY:-BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwc=}
export ADMIN_PASSWORD=${ADMIN_PASSWORD:-smoke}
export CRON_SECRET=${CRON_SECRET:-smoke-cron}
export SITE_URL=${SITE_URL:-http://localhost:3000}
export MAIL_MODE=${MAIL_MODE:-log}
exec npx next start -p 3000
