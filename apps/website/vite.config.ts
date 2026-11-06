import type { Plugin } from 'vite';
import { defineConfig } from 'vitest/config';
import { PARTIALS } from './partials';

// Static site: dist/ is copied to /var/www/gage/site on the VPS (tools/deploy/deploy-site.sh).
// public/media is filled by scripts/media.mjs from the launcher's footage and the archived key art.
//
// Pages: / (index.html) and the account pages /signup/, /login/, /account/. Caddy serves dist/ with a
// plain file_server and no SPA fallback, so every route is a real <route>/index.html. The inputs are
// relative to the package root, where npm runs Vite.

/** `<!-- @include name -->` in a page becomes PARTIALS[name] (partials.ts); an unknown name fails the build. */
function partials(): Plugin {
  const INCLUDE = /<!--\s*@include\s+([a-z]+)\s*-->/g;
  return {
    name: 'solar-partials',
    transformIndexHtml: {
      order: 'pre',
      handler: (html, ctx) =>
        html.replace(INCLUDE, (_m, name: string) => {
          const part = Object.hasOwn(PARTIALS, name) ? PARTIALS[name] : undefined;
          if (part === undefined) throw new Error(`${ctx.path}: no partial named "${name}" in partials.ts`);
          return part;
        }),
    },
  };
}

export default defineConfig({
  base: '/',
  plugins: [partials()],
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    target: 'es2022',
    assetsInlineLimit: 0,
    chunkSizeWarningLimit: 900,
    rollupOptions: {
      input: {
        main: 'index.html',
        signup: 'signup/index.html',
        login: 'login/index.html',
        account: 'account/index.html',
      },
    },
  },
  // Development only: the platform's public listener runs on 127.0.0.1:8787 (apps/platform README) and
  // serves /v1/*; in production Caddy maps https://solar-rp.com/api/* onto it the same way.
  server: {
    proxy: {
      '/api': {
        target: 'http://127.0.0.1:8787',
        changeOrigin: true,
        rewrite: (path) => path.replace(/^\/api(?=\/|$)/, ''),
      },
    },
  },
  test: {
    include: ['tests/**/*.test.ts'],
    environment: 'node',
  },
});
