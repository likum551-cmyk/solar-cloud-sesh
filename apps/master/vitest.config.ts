import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    include: ['test/**/*.test.ts'],
    environment: 'node',
    // UDP and HTTP tests bind real sockets on 127.0.0.1 with ephemeral ports.
    testTimeout: 10_000,
    hookTimeout: 10_000,
  },
});
