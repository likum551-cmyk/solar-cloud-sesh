import { defineConfig } from 'vitest/config';

// Static site: dist/ is copied to /var/www/gage/site on the VPS (tools/deploy/deploy-site.sh).
// public/media is filled by scripts/media.mjs from the launcher's footage and the archived key art.
export default defineConfig({
  base: '/',
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    target: 'es2022',
    assetsInlineLimit: 0,
    chunkSizeWarningLimit: 900,
  },
  test: {
    include: ['tests/**/*.test.ts'],
    environment: 'node',
  },
});
