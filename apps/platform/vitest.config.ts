import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    include: ['test/**/*.test.ts'],
    environment: 'node',
    // Each test file boots its own in-process PGlite (WASM Postgres); the first boot is the slow one.
    testTimeout: 30_000,
    hookTimeout: 60_000,
  },
});
