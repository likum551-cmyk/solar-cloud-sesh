/// <reference types="vitest/config" />
import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { viteSingleFile } from 'vite-plugin-singlefile';

// One self-contained dist/index.html (JS, CSS and fonts inlined) for tools/embed_ui.cmake, plus
// dist/assets/** copied verbatim from public/assets/: the heavy 3D files are never inlined, the
// native host serves them from https://app.gage/assets/ (see NATIVE-INTEGRATION.md).
export default defineConfig({
  base: './',
  plugins: [react(), viteSingleFile({ removeViteModuleLoader: true })],
  build: {
    target: 'es2022',
    outDir: 'dist',
    emptyOutDir: true,
    copyPublicDir: true,
    chunkSizeWarningLimit: 4096,
    reportCompressedSize: false,
  },
  server: {
    host: '127.0.0.1',
    port: 5317,
    strictPort: true,
  },
  preview: {
    host: '127.0.0.1',
    port: 5318,
    strictPort: true,
  },
  test: {
    environment: 'node',
    include: ['tests/**/*.test.{ts,tsx}'],
  },
});
