import { defineConfig } from 'drizzle-kit';

// `npm run db:generate` turns src/db/schema.ts into SQL files in drizzle/ (no database needed).
// `npm run db:migrate` applies them (scripts/migrate.ts) using DATABASE_URL.
export default defineConfig({
  dialect: 'postgresql',
  schema: './src/db/schema.ts',
  out: './drizzle',
});
